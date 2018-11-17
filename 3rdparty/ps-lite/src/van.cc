/**
 *  Copyright (c) 2015 by Contributors
 */
#include "ps/internal/van.h"
#include <thread>
#include <chrono>
#include <climits>
#include "ps/base.h"
#include "ps/sarray.h"
#include "ps/internal/postoffice.h"
#include "ps/internal/customer.h"
#include "./network_utils.h"
#include "./meta.pb.h"
#include "./zmq_van.h"
#include "./resender.h"
namespace ps {

// interval in second between to heartbeast signals. 0 means no heartbeat.
// don't send heartbeast in default. because if the scheduler received a
// heartbeart signal from a node before connected to that node, then it could be
// problem.
static const int kDefaultHeartbeatInterval = 0;

Van* Van::Create(const std::string& type) {
  if (type == "zmq") {
    return new ZMQVan();
  } else {
    LOG(FATAL) << "unsupported van type: " << type;
    return nullptr;
  }
}

void Van::ProcessTerminateCommand() {
  PS_VLOG(1) << my_node().ShortDebugString() << " is stopped";
  ready_ = false;
}

void Van::ProcessUpdateEnvVariable(Message* msg,  Meta* nodes){
  if(msg->meta.request){
    // call Update EnvVariable
    std::string env_var = std::string(msg->data[0].data());
    std::string val = std::string(msg->data[1].data());
    // in case of node removal, new data represents nodes to remove
    std::string data;
    if(msg->data.size() > 2) {
      data = std::string(msg->data[2].data());
    }
    PS_VLOG(1) << "Pid:" << getpid() << " ProcessUpdate Got env_var:" << env_var <<
    " value: "<< val ; 
    Postoffice::Get()->updateEnvironmentVariable(env_var, val, data, nodes);
    // Send Ack Msg Back to scheduler
    Message update_env_ack;
    update_env_ack.meta.recver = kScheduler;
    update_env_ack.meta.control.cmd = Control::UPDATE_ENV_VAR;
    update_env_ack.meta.timestamp = timestamp_++;
    update_env_ack.meta.request = false;
    update_env_ack.meta.sender = my_node_.id;
    PS_VLOG(1) << "Process:" << getpid() << " Seding back ack to scheduler";

    // send back ack
    Send(update_env_ack);
  } else {
    CHECK_EQ(Postoffice::Get()->is_scheduler(), 1) << " UpdateEnv Response received is expected on scheduler. My role is:" << my_node().role << " Process:"<<getpid();
    PS_VLOG(1) << "Process:" << getpid() << " Notifying Scheduler for repsonse";
    Postoffice::Get()->notifyUpdateEnvReceived(); 
    PS_VLOG(1) << "Process:" << getpid() << " Scheduler sent messages for repsonse";  
  }
}

void Van::RemoveNodeId(const std::unordered_set<int>& removed_node_ids){
  // remove node if from worker and server list
  for(auto id: removed_node_ids){
    if(worker_node_.find(id) != worker_node_.end()){
      worker_node_.erase(id);
      PS_VLOG(1) << "Pid:" << getpid() << " Removed node id :" << id << " from worker-list";
    } else if (server_node_.find(id) != server_node_.end()){
      server_node_.erase(id);
      PS_VLOG(1) << "Pid:" << getpid() << " Removed node id :" << id << " from server-list";
    }
  }

  // remove from connected_nodes
  auto it = connected_nodes_.begin();
  while(it != connected_nodes_.end()){
    if(removed_node_ids.find((*it).second) != removed_node_ids.end()){
      PS_VLOG(1) << " Process: " << getpid() << " Host:" << (*it).first;
      PS_VLOG(1) << " Process: " << getpid() << " dropping node id " << (*it).second << " from connected nodes";
      it = connected_nodes_.erase(it);
    } else {
      it++;
    }
  }
}

void Van::ProcessAddNodeCommandAtScheduler(
        Message* msg, Meta* nodes, Meta* recovery_nodes) {
  recovery_nodes->control.cmd = Control::ADD_NODE;
  time_t t = time(NULL);
  size_t num_nodes = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();
  PS_VLOG(1) << "PID:" << getpid() << " num_nodes:" << num_nodes; 
  if (nodes->control.node.size() == num_nodes + is_scheduler_added) {
    // sort the nodes according their ip and port,
    std::sort(nodes->control.node.begin(), nodes->control.node.end(),
              [](const Node& a, const Node& b) {
                  return (a.hostname.compare(b.hostname) | (a.port < b.port)) > 0;
              });
    bool had_empty_node_id = false;
    // assign node rank
    for (auto& node : nodes->control.node) {
      std::string node_host_ip = node.hostname + ":" + std::to_string(node.port);
      PS_VLOG(1) << "Pid:" << getpid() << " Node hostIP:" << node_host_ip;
      if(node.role == Node::SCHEDULER && is_scheduler_added) continue;
      had_empty_node_id = (node.id == Node::kEmpty);
      if (connected_nodes_.find(node_host_ip) == connected_nodes_.end()) {
        PS_VLOG(1) << "Pid:" << getpid() <<  "Couldn't find " << node_host_ip << " Node is: " << node.DebugString();
        CHECK_EQ(node.id, Node::kEmpty);
        int id = node.role == Node::SERVER ?
                 Postoffice::ServerRankToID(num_servers_) :
                 Postoffice::WorkerRankToID(num_workers_);
        PS_VLOG(1) << "Pid:" << getpid() <<  "assign rank=" << id << " to node " << node.DebugString();
        node.id = id;
        Connect(node);
        Postoffice::Get()->UpdateHeartbeat(node.id, t);
        connected_nodes_[node_host_ip] = id;
      } else if(had_empty_node_id){
        int id = node.role == Node::SERVER ?
                 Postoffice::ServerRankToID(num_servers_) :
                 Postoffice::WorkerRankToID(num_workers_);
        shared_node_mapping_[id] = connected_nodes_[node_host_ip];
        node.id = connected_nodes_[node_host_ip];
      }
      if(had_empty_node_id){
        if (node.role == Node::SERVER) num_servers_++;
        if (node.role == Node::WORKER) num_workers_++;
      } 
      if(node.role == Node::SERVER){
        server_node_.insert(node.id);
      } else if (node.role == Node::WORKER){
        worker_node_.insert(node.id);
      }
    }
    Postoffice::Get()->syncWorkerNodeIdsGroup(worker_node_);
    // my_node(scheduler should not be pushed if it is already there)
    if(is_scheduler_added == 0) {
      nodes->control.node.push_back(my_node_);
      is_scheduler_added = 1;
    }
    nodes->control.cmd = Control::ADD_NODE;
    Message back;
    back.meta = *nodes;
    for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
      int recver_id = r;
      if (shared_node_mapping_.find(r) == shared_node_mapping_.end()) {
        PS_VLOG(1) << "Sending add node back to recvr :" << r;
        back.meta.recver = recver_id;
        back.meta.timestamp = timestamp_++;
        Send(back);
      }
    }
    PS_VLOG(1) << "the scheduler is connected to "
               << worker_node_.size() << " workers and " << server_node_.size() << " servers";
    ready_ = true;
  } else if (!recovery_nodes->control.node.empty()) {
    auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_);
    std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
    // send back the recovery node
    CHECK_EQ(recovery_nodes->control.node.size(), 1);
    Connect(recovery_nodes->control.node[0]);
    Postoffice::Get()->UpdateHeartbeat(recovery_nodes->control.node[0].id, t);
    Message back;
    for (int r : Postoffice::Get()->GetNodeIDs(kWorkerGroup + kServerGroup)) {
      if (r != recovery_nodes->control.node[0].id
          && dead_set.find(r) != dead_set.end()) {
        // do not try to send anything to dead node
        continue;
      }
      // only send recovery_node to nodes already exist
      // but send all nodes to the recovery_node
      back.meta = (r == recovery_nodes->control.node[0].id) ? *nodes : *recovery_nodes;
      back.meta.recver = r;
      back.meta.timestamp = timestamp_++;
      Send(back);
    }
  }
}

void Van::UpdateLocalID(Message* msg, std::unordered_set<int>* deadnodes_set,
                        Meta* nodes, Meta* recovery_nodes) {
  auto& ctrl = msg->meta.control;
  size_t num_nodes = Postoffice::Get()->num_servers() + Postoffice::Get()->num_workers();
  // assign an id
  if (msg->meta.sender == Meta::kEmpty) {
    CHECK(is_scheduler_);
    CHECK_EQ(ctrl.node.size(), 1);
    PS_VLOG(1) << "Pid:" << getpid() << " UpdateLocalId sender is empty, node->control size:" << nodes->control.node.size();
    if (nodes->control.node.size() < num_nodes + is_scheduler_added) {
      nodes->control.node.push_back(ctrl.node[0]);
      PS_VLOG(1) << "Pid:" << getpid() << " Adding node to scheduler nodes:" << ctrl.node[0].DebugString();
    } else {
      // some node dies and restarts
      CHECK(ready_.load());
      PS_VLOG(1) << " Not adding node to scheduler nodes. Probably some node died and restarted.";
      for (size_t i = 0; i < nodes->control.node.size() - 1; ++i) {
        const auto& node = nodes->control.node[i];
        if (deadnodes_set->find(node.id) != deadnodes_set->end() &&
            node.role == ctrl.node[0].role) {
          auto& recovery_node = ctrl.node[0];
          // assign previous node id
          recovery_node.id = node.id;
          recovery_node.is_recovery = true;
          PS_VLOG(1) << "replace dead node " << node.DebugString()
                     << " by node " << recovery_node.DebugString();
          nodes->control.node[i] = recovery_node;
          recovery_nodes->control.node.push_back(recovery_node);
          break;
        }
      }
    }
  }

  // update my id
  for (size_t i = 0; i < ctrl.node.size(); ++i) {
    const auto& node = ctrl.node[i];
    if (my_node_.hostname == node.hostname && my_node_.port == node.port) {
    if (getenv("DMLC_RANK") == nullptr || my_node_.id == Meta::kEmpty) {
        my_node_ = node;
        std::string rank = std::to_string(Postoffice::IDtoRank(node.id));
#ifdef _MSC_VER
        _putenv_s("DMLC_RANK", rank.c_str());
#else
        setenv("DMLC_RANK", rank.c_str(), true);
#endif
      }
    }
  }
}

void Van::ProcessHearbeat(Message* msg) {
  auto& ctrl = msg->meta.control;
  time_t t = time(NULL);
  for (auto &node : ctrl.node) {
    Postoffice::Get()->UpdateHeartbeat(node.id, t);
    if (is_scheduler_) {
      Message heartbeat_ack;
      heartbeat_ack.meta.recver = node.id;
      heartbeat_ack.meta.control.cmd = Control::HEARTBEAT;
      heartbeat_ack.meta.control.node.push_back(my_node_);
      heartbeat_ack.meta.timestamp = timestamp_++;
      heartbeat_ack.meta.sender = my_node().id;
      // send back heartbeat
      Send(heartbeat_ack);
    }
  }
}

void Van::ProcessBarrierCommand(Message* msg, Meta* nodes) {
  auto& ctrl = msg->meta.control;
  if (msg->meta.request) {
    if (barrier_count_.empty()) {
      barrier_count_.resize(8, 0);
    }
    int group = ctrl.barrier_group;
    ++barrier_count_[group];
    PS_VLOG(1) << "Barrier count for " << group << " : " << barrier_count_[group] << " total required:"
    << static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size());
    if (barrier_count_[group] ==
        static_cast<int>(Postoffice::Get()->GetNodeIDs(group).size())) {
      barrier_count_[group] = 0;
      if(ctrl.cmd == Control::Command::MEMBERSHIP_CHANGE_BARRIER){
         // call Update EnvVariable
        int max_receiver_id = INT_MIN;
        for (int r : Postoffice::Get()->GetNodeIDs(group)) {
          if(r > max_receiver_id) max_receiver_id = r;
        }
        std::vector<std::pair<std::string, std::string> > env;
        PS_VLOG(1) << "MembershipChangeBarrier Msg is:" << msg->DebugString();
        for(size_t i=0 ; i < msg->data.size() ; i+=2){
          std::string k = std::string(msg->data[i].data());
          std::string v = std::string(msg->data[i+1].data());
          PS_VLOG(1) << "MembershipChangeBarrier: Got Key Value: " << k << " " <<v;
          env.emplace_back(k,v);
        }

        ps::Postoffice::Get()->et_node_manager()->invokeMembershipChange(std::move(env), std::bind(&Van::SendResponseToGroup, this, group, max_receiver_id, msg->meta.customer_id, msg->meta.app_id, Control::Command::MEMBERSHIP_CHANGE_BARRIER), nodes);
        return;
      }
      SendResponseToGroup(group, INT_MAX/*max_receiver_id*/, msg->meta.customer_id, msg->meta.app_id, Control::Command::BARRIER);
    }
  } else {
    Postoffice::Get()->Manage(*msg);
  }
}
void Van::SendResponseToGroup(int group, int max_recver_id, int custId, int appId, Control::Command c) {
  Message res;
  res.meta.request = false;
  res.meta.app_id = appId;
  res.meta.customer_id = custId;
  res.meta.control.cmd = c;
  res.meta.sender = my_node().id;
  if(c == Control::MEMBERSHIP_CHANGE_BARRIER){
    PS_VLOG(1) << " Sending MCBresponse to group:" << group;
  }
  for (int r : Postoffice::Get()->GetNodeIDs(group)) {
    int recver_id = r;
    if(r > max_recver_id){
      PS_VLOG(1) << " Skipping receiver with id:"<< r << " as max_receiver_id is:"<< max_recver_id;
      continue;
    }
    if (shared_node_mapping_.find(r) == shared_node_mapping_.end()) {
      res.meta.recver = recver_id;
      res.meta.timestamp = timestamp_++;
      CHECK_GT(Send(res), 0);
    }
  }
}

void Van::ProcessDataMsg(Message* msg) {
  // data msg
  CHECK_NE(msg->meta.sender, Meta::kEmpty);
  CHECK_NE(msg->meta.recver, Meta::kEmpty);
  CHECK_NE(msg->meta.app_id, Meta::kEmpty);
  // TODO check only if 
  if(ready_ && ps::Postoffice::Get()->removed_hosts() && !IsSenderIdValid(msg->meta.sender)){
    PS_VLOG(1) << "Pid:" << getpid() <<  " Message came from invalid sender id:" << msg->meta.sender << " Dropping message.";
    return;
  }

  int app_id = msg->meta.app_id;
  int customer_id = Postoffice::Get()->is_worker() ? msg->meta.customer_id : app_id;
  auto* obj = Postoffice::Get()->GetCustomer(app_id, customer_id, 5);
  CHECK(obj) << "timeout (5 sec) to wait App " << app_id << " customer " << customer_id \
    << " ready at " << my_node_.role;
  obj->Accept(*msg);
}

void Van::ProcessAddNodeCommand(Message* msg, Meta* nodes, Meta* recovery_nodes) {
  auto dead_nodes = Postoffice::Get()->GetDeadNodes(heartbeat_timeout_);
  std::unordered_set<int> dead_set(dead_nodes.begin(), dead_nodes.end());
  auto& ctrl = msg->meta.control;

  UpdateLocalID(msg, &dead_set, nodes, recovery_nodes);

  if (is_scheduler_) {
    ProcessAddNodeCommandAtScheduler(msg, nodes, recovery_nodes);
  } else {
    for (const auto& node : ctrl.node) {
      std::string addr_str = node.hostname + ":" + std::to_string(node.port);
      PS_VLOG(1) << "Pid:" << getpid() <<" Got node:" << addr_str << " to connect to";
      if (connected_nodes_.find(addr_str) == connected_nodes_.end()) {
        Connect(node);
        connected_nodes_[addr_str] = node.id;
        if (!node.is_recovery && node.role == Node::SERVER) ++num_servers_;
        if (!node.is_recovery && node.role == Node::WORKER) ++num_workers_;
      }
      if(node.role == Node::WORKER){
        worker_node_.insert(node.id);
      } else if (node.role == Node::SERVER){
        server_node_.insert(node.id);
      }
    }
    Postoffice::Get()->syncWorkerNodeIdsGroup(worker_node_);
    PS_VLOG(1) << "Pid: " << getpid() << " " << my_node_.ShortDebugString() << " is connected to others";
    for(auto id: worker_node_){
      PS_VLOG(1) << "Pid: " << getpid() << " Worker node Id: " << id;
    }
    for(auto id: server_node_) {
      PS_VLOG(1) << "Pid: " << getpid() << " Server node id" << id;
    }
    ready_ = true;
  }
}

void Van::Start(int customer_id) {
  // get scheduler info
  start_mu_.lock();

  if (init_stage == 0) {
    scheduler_.hostname = std::string(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_URI")));
    scheduler_.port = atoi(CHECK_NOTNULL(Environment::Get()->find("DMLC_PS_ROOT_PORT")));
    scheduler_.role = Node::SCHEDULER;
    scheduler_.id = kScheduler;
    is_scheduler_ = Postoffice::Get()->is_scheduler();

    // get my node info
    if (is_scheduler_) {
      my_node_ = scheduler_;
    } else {
      auto role = is_scheduler_ ? Node::SCHEDULER :
                  (Postoffice::Get()->is_worker() ? Node::WORKER : Node::SERVER);
      const char *nhost = Environment::Get()->find("DMLC_NODE_HOST");
      std::string ip;
      if (nhost) ip = std::string(nhost);
      if (ip.empty()) {
        const char *itf = Environment::Get()->find("DMLC_INTERFACE");
        std::string interface;
        if (itf) interface = std::string(itf);
        if (interface.size()) {
          GetIP(interface, &ip);
        } else {
          GetAvailableInterfaceAndIP(&interface, &ip);
        }
        CHECK(!interface.empty()) << "failed to get the interface";
      }
      int port = GetAvailablePort();
      const char *pstr = Environment::Get()->find("PORT");
      if (pstr) port = atoi(pstr);
      CHECK(!ip.empty()) << "failed to get ip";
      CHECK(port) << "failed to get a port";
      PS_VLOG(1) << "Role is " <<  role << " port is:"<<port;
      my_node_.hostname = ip;
      my_node_.role = role;
      my_node_.port = port;
      // cannot determine my id now, the scheduler will assign it later
      // set it explicitly to make re-register within a same process possible
      my_node_.id = Node::kEmpty;
      my_node_.customer_id = customer_id;
    }

    // bind.
    my_node_.port = Bind(my_node_, is_scheduler_ ? 0 : 40);
    PS_VLOG(1) <<"PId:" << getpid() << " Bind to " << my_node_.DebugString();
    CHECK_NE(my_node_.port, -1) << "bind failed";

    // connect to the scheduler
    Connect(scheduler_);
    PS_VLOG(1) <<"PId:" << getpid() << " Coonected to scheduler";


    // for debug use
    if (Environment::Get()->find("PS_DROP_MSG")) {
      drop_rate_ = atoi(Environment::Get()->find("PS_DROP_MSG"));
    }
    // start receiver
    receiver_thread_ = std::unique_ptr<std::thread>(
            new std::thread(&Van::Receiving, this));
    init_stage++;
  }
  start_mu_.unlock();

  if (!is_scheduler_) {
    // let the scheduler know myself
    Message msg;
    Node customer_specific_node = my_node_;
    customer_specific_node.customer_id = customer_id;
    msg.meta.recver = kScheduler;
    msg.meta.control.cmd = Control::ADD_NODE;
    msg.meta.control.node.push_back(customer_specific_node);
    msg.meta.timestamp = timestamp_++;
    Send(msg);
    PS_VLOG(1) <<"PId:" << getpid() << " sent add node to scheduler";

  }

  // wait until ready
  while (!ready_.load()) {
  //  PS_VLOG(1) << "PID:" << getpid() <<  " Sleeping for being ready";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  start_mu_.lock();
  if (init_stage == 1) {
    // resender
    if (Environment::Get()->find("PS_RESEND") && atoi(Environment::Get()->find("PS_RESEND")) != 0) {
      int timeout = 1000;
      if (Environment::Get()->find("PS_RESEND_TIMEOUT")) {
        timeout = atoi(Environment::Get()->find("PS_RESEND_TIMEOUT"));
      }
      resender_ = new Resender(timeout, 10, this);
    }

    if (!is_scheduler_) {
      // start heartbeat thread
      heartbeat_thread_ = std::unique_ptr<std::thread>(
              new std::thread(&Van::Heartbeat, this));
    }
    init_stage++;
  }
  start_mu_.unlock();
}

void Van::Stop() {
  // stop threads
  Message exit;
  exit.meta.control.cmd = Control::TERMINATE;
  exit.meta.recver = my_node_.id;
  // only customer 0 would call this method
  exit.meta.customer_id = 0;
  int ret = SendMsg(exit);
  CHECK_NE(ret, -1);
  receiver_thread_->join();
  init_stage = 0;
  if (!is_scheduler_) heartbeat_thread_->join();
  if (resender_) delete resender_;
  ready_ = false;
  connected_nodes_.clear();
  shared_node_mapping_.clear();
  send_bytes_ = 0;
  timestamp_ = 0;
  my_node_.id = Meta::kEmpty;
  barrier_count_.clear();
}

int Van::Send(const Message& msg) {
  int send_bytes = SendMsg(msg);
  CHECK_NE(send_bytes, -1);
  send_bytes_ += send_bytes;
  if (resender_) resender_->AddOutgoing(msg);
  if (Postoffice::Get()->verbose() >= 2) {
    PS_VLOG(2) << msg.DebugString();
  }
  return send_bytes;
}

void Van::DropSenderHosts(const std::unordered_set<int>& drop_senders){
  DropSender(drop_senders);
  RemoveNodeId(drop_senders);
}

int Van::GetMyRank(){
  Node::Role my_role = my_node_.role;
  if(my_role == Node::SCHEDULER){
    return Postoffice::IDtoRank(my_node_.id);
  } else if (my_role == Node::WORKER) {
     auto itr = worker_node_.find(my_node_.id);
     if(itr == worker_node_.end()){
       PS_VLOG(1) << "Couldn't find my worker id:" << my_node_.id << " in registered worker nodes";
     } else {
       return std::distance(worker_node_.begin(), itr);
     }
  } else {
    auto itr = server_node_.find(my_node_.id);
     if(itr == server_node_.end()){
       PS_VLOG(1) << "Couldn't find my server id:" << my_node_.id << " in registered server nodes";
     } else {
       return std::distance(server_node_.begin(), itr);
     }
  }
  throw std::runtime_error("Couldn't find my id in registered worker or server nodes");
}

std::unordered_set<int> Van::GetNodeIdSet(const std::unordered_set<std::string>& senders){
  std::unordered_set<int> node_ids;
  for(auto it = connected_nodes_.begin(); it != connected_nodes_.end(); it++){
    std::size_t found = it->first.rfind(':');
    std::string hostIp = it->first.substr(0, found);
    PS_VLOG(1) << "Pid:" << getpid() << " connected node:" << it->first<< " host Ip:" << hostIp;
    if(senders.find(hostIp) != senders.end()){
      PS_VLOG(1) << "Adding node id:" << it->second << " to return list.";
      node_ids.insert(it->second);
    }
  }
  return node_ids;
}

void Van::Receiving() {
  Meta nodes;
  Meta recovery_nodes;  // store recovery nodes
  recovery_nodes.control.cmd = Control::ADD_NODE;

  while (true) {
    Message msg;
    int recv_bytes = RecvMsg(&msg);
    // For debug, drop received message
    if (ready_.load() && drop_rate_ > 0) {
      unsigned seed = time(NULL) + my_node_.id;
      if (rand_r(&seed) % 100 < drop_rate_) {
        LOG(WARNING) << "Drop message " << msg.DebugString();
        continue;
      }
    }
    if(ready_ && ps::Postoffice::Get()->removed_hosts() && msg.meta.sender != Meta::kEmpty && !IsSenderIdValid(msg.meta.sender)){
      PS_VLOG(1) << " Dropping message from sender:" << msg.meta.sender << " as sender not in valid sender host.";
      continue;
    }
    CHECK_NE(recv_bytes, -1);
    recv_bytes_ += recv_bytes;
    if (Postoffice::Get()->verbose() >= 1) {
      PS_VLOG(2) << msg.DebugString();
    }
    // duplicated message
    if (resender_ && resender_->AddIncomming(msg)) continue;

    if (!msg.meta.control.empty()) {
      // control msg
      auto& ctrl = msg.meta.control;
      if (ctrl.cmd == Control::TERMINATE) {
        ProcessTerminateCommand();
        break;
      } else if (ctrl.cmd == Control::ADD_NODE) {
        ProcessAddNodeCommand(&msg, &nodes, &recovery_nodes);
      } else if (ctrl.cmd == Control::BARRIER || ctrl.cmd == Control::Command::MEMBERSHIP_CHANGE_BARRIER) {
        PS_VLOG(1) << "Process:"<< getpid() << " received message:" << msg.DebugString(); 

        ProcessBarrierCommand(&msg, &nodes);
      } else if (ctrl.cmd == Control::HEARTBEAT) {
        ProcessHearbeat(&msg);
      } else if(ctrl.cmd == Control::UPDATE_ENV_VAR) {
        PS_VLOG(1) << "Process:" << getpid() << " Got msg to update_env_var";
        ProcessUpdateEnvVariable(&msg, &nodes);
      } else {
        LOG(WARNING) << "Drop unknown typed message " << msg.DebugString();
      }
    } else {
      ProcessDataMsg(&msg);
    }
  }
}

void Van::PackMeta(const Meta& meta, char** meta_buf, int* buf_size) {
  // convert into protobuf
  PBMeta pb;
  pb.set_head(meta.head);
  if (meta.app_id != Meta::kEmpty) pb.set_app_id(meta.app_id);
  if (meta.timestamp != Meta::kEmpty) pb.set_timestamp(meta.timestamp);
  if (meta.body.size()) pb.set_body(meta.body);
  pb.set_push(meta.push);
  pb.set_request(meta.request);
  pb.set_simple_app(meta.simple_app);
  pb.set_customer_id(meta.customer_id);
  for (auto d : meta.data_type) pb.add_data_type(d);
  if (!meta.control.empty()) {
    auto ctrl = pb.mutable_control();
    ctrl->set_cmd(meta.control.cmd);
    if (meta.control.cmd == Control::BARRIER || meta.control.cmd == Control::MEMBERSHIP_CHANGE_BARRIER) {
      ctrl->set_barrier_group(meta.control.barrier_group);
    } else if (meta.control.cmd == Control::ACK) {
      ctrl->set_msg_sig(meta.control.msg_sig);
    }
    for (const auto& n : meta.control.node) {
      auto p = ctrl->add_node();
      p->set_id(n.id);
      p->set_role(n.role);
      p->set_port(n.port);
      p->set_hostname(n.hostname);
      p->set_is_recovery(n.is_recovery);
      p->set_customer_id(n.customer_id);
    }
  }
  // to string
  *buf_size = pb.ByteSize();
  *meta_buf = new char[*buf_size+1];
  CHECK(pb.SerializeToArray(*meta_buf, *buf_size))
    << "failed to serialize protbuf";
}

void Van::UnpackMeta(const char* meta_buf, int buf_size, Meta* meta) {
  // to protobuf
  PBMeta pb;
  CHECK(pb.ParseFromArray(meta_buf, buf_size))
    << "failed to parse string into protobuf";

  // to meta
  meta->head = pb.head();
  meta->app_id = pb.has_app_id() ? pb.app_id() : Meta::kEmpty;
  meta->timestamp = pb.has_timestamp() ? pb.timestamp() : Meta::kEmpty;
  meta->request = pb.request();
  meta->push = pb.push();
  meta->simple_app = pb.simple_app();
  meta->body = pb.body();
  meta->customer_id = pb.customer_id();
  meta->data_type.resize(pb.data_type_size());
  for (int i = 0; i < pb.data_type_size(); ++i) {
    meta->data_type[i] = static_cast<DataType>(pb.data_type(i));
  }
  if (pb.has_control()) {
    const auto& ctrl = pb.control();
    meta->control.cmd = static_cast<Control::Command>(ctrl.cmd());
    meta->control.barrier_group = ctrl.barrier_group();
    meta->control.msg_sig = ctrl.msg_sig();
    for (int i = 0; i < ctrl.node_size(); ++i) {
      const auto& p = ctrl.node(i);
      Node n;
      n.role = static_cast<Node::Role>(p.role());
      n.port = p.port();
      n.hostname = p.hostname();
      n.id = p.has_id() ? p.id() : Node::kEmpty;
      n.is_recovery = p.is_recovery();
      n.customer_id = p.customer_id();
      meta->control.node.push_back(n);
    }
  } else {
    meta->control.cmd = Control::EMPTY;
  }
}

void Van::Heartbeat() {
  const char* val = Environment::Get()->find("PS_HEARTBEAT_INTERVAL");
  const int interval = val ? atoi(val) : kDefaultHeartbeatInterval;
  while (interval > 0 && ready_.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(interval));
    Message msg;
    msg.meta.recver = kScheduler;
    msg.meta.control.cmd = Control::HEARTBEAT;
    msg.meta.control.node.push_back(my_node_);
    msg.meta.timestamp = timestamp_++;
    Send(msg);
  }
}
}  // namespace ps
