/**
 *  Copyright (c) 2015 by Contributors
 */
#ifndef PS_ELASTIC_TRAINING_H_
#define PS_ELASTIC_TRAINING_H_
#include <string>
#include <unordered_set>
#include <fstream>
#include "ps/internal/env.h"
#include "dmlc/logging.h"
#include <unistd.h>

namespace ps {

/**
 * \brief an interface for Node manager for elastic training
 * 
 */
class ETNodeManager {
 public:
  /**
   * \brief takes action to do a membership change if required
   * 
   */
  virtual void invokeMembershipChange(const std::vector<std::pair<std::string, std::string> > env, std::function<void()> res_cb, Meta* nodes)=0;
  /**
   * \brief launches training script on new worker node
   */
  void launchCommandOnNewWorker(const std::string& worker_ip, const std::vector<std::pair<std::string, std::string> >& env);
  virtual void invokeSuccessResponseCallback()=0;
};

class ETDefaultNodeManager: public ETNodeManager {
  public:
    explicit ETDefaultNodeManager();
    virtual void invokeMembershipChange(const std::vector<std::pair<std::string, std::string> > env, std::function<void()> res_cb, Meta* nodes);
    virtual void invokeSuccessResponseCallback () {
      LOG(INFO) << " Invoke success callback called Process:" << getpid();
      return on_success_resp_cb_();
    }
  private:
    void findMembershipChanges();
    void OnSuccessUpdatingEnv(const std::vector<std::pair<std::string, std::string> > env, std::function<void()> res_cb);
    void getCurrentWorkerSet();
    std::unordered_set<std::string> workers_;
    std::vector<std::string> workers_added_, workers_removed_;
    std::function<void()> on_success_resp_cb_;
    uint log_segnum;
};


}// namespace ps
#endif  // PS_ELASTIC_TRAINING_H_
