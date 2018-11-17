#include <fstream>
#include <vector>
#include <unordered_set>
#include "ps/internal/elastic_training.h"
#include "ps/internal/env.h"
#include "ps/internal/postoffice.h"

namespace ps {
void ETDefaultNodeManager::getCurrentWorkerSet() {
  const char *host_file = NULL;
  host_file = CHECK_NOTNULL(Environment::Get()->find("WORKER_HOST_FILE"));
  std::ifstream ff(host_file, std::ifstream::in);
  std::string worker;
  workers_.clear();
  while(getline(ff, worker)){
    workers_.insert(worker);
    PS_VLOG(1) << "PID:" << getpid() << " ETManager inserted worker:" << worker; 
  }
};

ETDefaultNodeManager::ETDefaultNodeManager() {
  getCurrentWorkerSet();
  log_segnum = 0;
};

void ETNodeManager::launchCommandOnNewWorker(const std::string& worker_ip, const std::vector<std::pair<std::string, std::string> >& env) {
  PS_VLOG(1) << "Process:" << getpid() << " Launching command on new worker";
    // TODO use libssh, that will help redirect the output to logfile
  const char* launch_script = NULL;
  

  launch_script = CHECK_NOTNULL(Environment::Get()->find("MXNET_LAUNCH_SCRIPT_PATH"));
  std::string cmd = "python " + std::string(launch_script);
  cmd += " -n 1 ";
  cmd += " --host ";
  cmd += worker_ip;
  cmd += " --launch-worker True ";
  cmd += " --env NEW_WORKER:1 --env DMLC_NUM_WORKER:" + std::to_string(Postoffice::Get()->num_workers());
  cmd += " --env DMLC_NUM_SERVER:" + std::to_string(Postoffice::Get()->num_servers());
  cmd += " --env DMLC_PS_ROOT_URI:" + std::string(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_URI")));
  cmd += " --env DMLC_PS_ROOT_PORT:" + std::string(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_PORT")));
  const char *itf = Environment::Get()->find("DMLC_INTERFACE");
  if(itf) {
      cmd += " --env DMLC_INTERFACE:" + std::string(itf);
  }
  // TODO get all env from scheduler copied to worker

  for(auto entry: env){
    cmd += " --env ";
    cmd += entry.first;
    cmd += ":";
    cmd += entry.second;
    cmd += " ";     
  }
  const char* training_command = NULL;
  training_command = CHECK_NOTNULL(Environment::Get()->find("TRAINING_CMD"));
  cmd += " ";
  cmd += std::string(training_command);
  cmd += " &";
  PS_VLOG(1) << " Launching with command:" << cmd;
  std::system(cmd.c_str());
};

void ETDefaultNodeManager::invokeMembershipChange(const std::vector<std::pair<std::string, std::string> > env, std::function<void()> res_cb, Meta* nodes)  {
  findMembershipChanges();
  if(workers_removed_.size() > 0) {
    std::string worker_removed_string = "";
    for(size_t i=0; i< workers_removed_.size(); ++i){
      worker_removed_string += workers_removed_[i];
      if(i < workers_removed_.size() -1){
        worker_removed_string += ",";
      }
    }  
    Postoffice::Get()->updateEnvironmentVariable("DMLC_NUM_WORKER", std::to_string(Postoffice::Get()->num_workers() - workers_removed_.size()), worker_removed_string, nodes);
    // above will send message to and come back, let's install remove callback
  } else if(workers_added_.size() > 0) {
    Postoffice::Get()->updateEnvironmentVariable("DMLC_NUM_WORKER", std::to_string(Postoffice::Get()->num_workers() + workers_added_.size()), "", nodes);
  } else {
    OnSuccessUpdatingEnv(std::move(env), std::move(res_cb));
    return;
  }
  on_success_resp_cb_ = std::bind(&ETDefaultNodeManager::OnSuccessUpdatingEnv, this,  std::move(env), std::move(res_cb)); 
};

void ETDefaultNodeManager::OnSuccessUpdatingEnv(const std::vector<std::pair<std::string, std::string> > env, std::function<void()> res_cb){
  PS_VLOG(1) << "Process:" << getpid() << " Invoked OnSuccessUpdatingEnv";
  for(auto new_worker_host : workers_added_){
    // launch ssh script on new machine with extra parameters
    launchCommandOnNewWorker(new_worker_host, env);
  }
  /* if worker was removed then we update the workers list accrodingly
     Worker removal takes priority over worker addition
     In case worker_host file, one of the worker get substituted by another worker,
     in first iteration the worker which was removed will be removed first, in next epoch new
     substituted worker will be added.
     For ex: if host file contains:
     A
     B
     C
    and it changes to
     A
     B
     D

     first C will be removed, and worker_ set will contain A,B
     In next epoch D will be added, and worker set will contain A,B,D
  */
  const char *host_file = NULL;
  host_file = CHECK_NOTNULL(Environment::Get()->find("WORKER_HOST_FILE"));
  std::string host_file_log = std::string(host_file) + "_log";
  std::ofstream worker_host_log_file;
  worker_host_log_file.open(host_file_log, std::ios_base::app);
  if(workers_removed_.size() > 0){
    for(auto wid : workers_removed_) {
      workers_.erase(wid);
      // add a log line to worker_host_log file
      worker_host_log_file << log_segnum++ << " REMOVED " << wid << " " << std::chrono::system_clock::now().time_since_epoch().count() << "\n";
    }
  } else if(workers_added_.size() > 0){
    for(auto wid : workers_added_){
      workers_.insert(wid);
      // add a log line to worker_host_log file
      // SEQNUM ADDED IP Time
      worker_host_log_file << log_segnum++ <<" ADDED " << wid << " " << std::chrono::system_clock::now().time_since_epoch().count() << "\n";
    }
  }
  for(auto w : workers_){
    PS_VLOG(1) << "Current Worker:" << w;
  }
  workers_removed_.clear();
  workers_added_.clear();
  res_cb();
}

void ETDefaultNodeManager::findMembershipChanges(){
  const char *host_file = NULL;
  host_file = CHECK_NOTNULL(Environment::Get()->find("WORKER_HOST_FILE"));
  PS_VLOG(1) << " In find membership changes";
  std::ifstream ff(host_file, std::ifstream::in);
  std::string worker;
  std::unordered_set<std::string> cur_workers;
  while(getline(ff, worker)){
    cur_workers.insert(worker);
  }
  for(auto p: cur_workers){
    if(workers_.find(p) == workers_.end()){
      workers_added_.push_back(p);
      PS_VLOG(1) << " Worker added:" << p;
    }
  }
  for(auto p: workers_){
    if(cur_workers.find(p) == cur_workers.end()){
      workers_removed_.push_back(p);
      PS_VLOG(1) << "Worker removed:" << p;
    }
  }
};
}
