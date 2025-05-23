/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * @Project: dingo
 * @Date: 2021-11-8 11:01:48
 * @Author: chenwei
 */

#ifndef DINGOFS_SRC_MDS_SCHEDULE_COORDINATOR_H_
#define DINGOFS_SRC_MDS_SCHEDULE_COORDINATOR_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "dingofs/schedule.pb.h"
#include "mds/schedule/operatorController.h"
#include "mds/schedule/scheduleMetrics.h"
#include "mds/schedule/schedule_define.h"
#include "mds/schedule/scheduler.h"
#include "mds/schedule/topoAdapter.h"
#include "mds/topology/topology_item.h"
#include "utils/interruptible_sleeper.h"

namespace dingofs {
namespace mds {
namespace schedule {

using utils::InterruptibleSleeper;

using pb::mds::heartbeat::ConfigChangeType;

class Coordinator {
 public:
  Coordinator() = default;
  explicit Coordinator(const std::shared_ptr<TopoAdapter>& topo);
  virtual ~Coordinator();
  /**
   * @brief deal with copyset info reported by the metaserver
   *
   * @param[in] originInfo Copyset info reported by heartbeat
   * @param[out] newConf   Configuration change generated for metaserver
   *
   * @return candidate ID if there's any new configuration generated,
   *         UNINITIALIZE_ID if not
   */
  virtual MetaServerIdType CopySetHeartbeat(
      const mds::topology::CopySetInfo& origin_info,
      const pb::mds::heartbeat::ConfigChangeInfo& config_ch_info,
      pb::mds::heartbeat::CopySetConf* out);

  virtual pb::mds::schedule::ScheduleStatusCode QueryMetaServerRecoverStatus(
      const std::vector<MetaServerIdType>& id_list,
      std::map<MetaServerIdType, bool>* status_map);

  /**
   * @brief determine whether the specified metaserver is the target of the
   *        AddOperator of specified copyset
   *
   * @param[in] msId Metaserver specified
   * @param[in] key Copyset specified
   */
  virtual bool MetaserverGoingToAdd(MetaServerIdType ms_id, CopySetKey key);

  /**
   * @brief Initialize the scheduler according to the configuration
   *
   * @param[in] conf
   * @param[in] metrics ScheduleMetric for calculation when
   *                    adding/delecting operator
   */
  void InitScheduler(const ScheduleOption& conf,
                     std::shared_ptr<ScheduleMetrics> metrics);

  /**
   * @brief run schedulers in background according to scheduler configuration
   */
  void Run();

  /**
   * @brief stop background scheduler threads
   */
  void Stop();

  // TODO(lixiaocui): external interface, and add according to the requirement
  //                  of operation and mantainance
  /**
   * @brief interface for the administrator
   *
   * @param[in] id CopysetID
   * @param[in] type Config change type: transfer-leader/add-peer/remove-peer
   * @param[in] item Item changed. It would be new leader ID when
   *                 transfer-leader, add target when add-peer, removed target
   *                 when remove a peer
   */
  void DoConfigChange(CopySetKey id, ConfigChangeType type,
                      MetaServerIdType item);

  /**
   * @brief For test unit
   */
  std::shared_ptr<OperatorController> GetOpController();

 private:
  /**
   * @brief SetScheduleRunning Stop all the schedulers if set to false
   *
   * @param[in] flag
   */
  void SetSchedulerRunning(bool flag);

  /**
   * @brief regular task for running different scheduler
   *
   * @param[in] s Schedulers for running
   * @param[in] type Scheduler type
   */
  void RunScheduler(const std::shared_ptr<Scheduler>& s, SchedulerType type);

  /**
   * @brief BuildCopySetConf Build copyset configuration for metaserver
   *
   * @param[in] res Result of applyOperator
   * @param[out] copyset configuration for metaserver
   *
   * @return true if succeeded and false if failed
   */
  bool BuildCopySetConf(const CopySetConf& res,
                        pb::mds::heartbeat::CopySetConf* out);

  /**
   * @brief ScheduleNeedRun Determine whether specific type of scheduler is
   *                        allowed to run
   *
   * @param[in] type Scheduler type
   *
   * @return true if allow, false is not
   */
  bool ScheduleNeedRun(SchedulerType type);

  /**
   * @brief ScheduleName Name of specific type of scheduler
   *
   * @param[in] type Scheduler type
   *
   * @return scheduler name
   */
  std::string ScheduleName(SchedulerType type);

  bool IsMetaServerRecover(const MetaServerInfo& info);

  std::shared_ptr<TopoAdapter> topo_;
  ScheduleOption conf_;

  std::map<SchedulerType, std::shared_ptr<Scheduler>> schedulerController_;
  std::map<SchedulerType, dingofs::utils::Thread> runSchedulerThreads_;
  std::shared_ptr<OperatorController> opController_;

  InterruptibleSleeper sleeper_;
};
}  // namespace schedule
}  // namespace mds
}  // namespace dingofs

#endif  // DINGOFS_SRC_MDS_SCHEDULE_COORDINATOR_H_
