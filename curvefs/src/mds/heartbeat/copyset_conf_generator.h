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
 * @Project: curve
 * @Date: 2021-11-11 15:28:43
 * @Author: chenwei
 */

#ifndef CURVEFS_SRC_MDS_HEARTBEAT_COPYSET_CONF_GENERATOR_H_
#define CURVEFS_SRC_MDS_HEARTBEAT_COPYSET_CONF_GENERATOR_H_

#include <memory>
#include <string>

#include "curvefs/proto/heartbeat.pb.h"
#include "curvefs/src/mds/schedule/coordinator.h"
#include "curvefs/src/mds/topology/topology.h"
#include "curvefs/src/mds/topology/topology_item.h"

using ::curvefs::mds::heartbeat::ConfigChangeInfo;
using ::curvefs::mds::schedule::Coordinator;
using ::curvefs::mds::topology::CopySetInfo;
using ::curvefs::mds::topology::MetaServer;
using ::curvefs::mds::topology::MetaServerIdType;
using ::curvefs::mds::topology::Topology;
using ::std::chrono::steady_clock;

namespace curvefs {
namespace mds {
namespace heartbeat {
class CopysetConfGenerator {
 public:
  CopysetConfGenerator(std::shared_ptr<Topology> topo,
                       std::shared_ptr<Coordinator> coordinator,
                       steady_clock::time_point mds_start_time,
                       uint64_t clean_follower_after_ms)
      : topo_(topo),
        coordinator_(coordinator),
        mdsStartTime_(mds_start_time),
        cleanFollowerAfterMs_(clean_follower_after_ms) {}

  /*
   * @brief GenCopysetConf  decide if there's any new config for metaserver
   *                        according to reported and stored copyset info
   *
   * @param[in] reportId metaserverId that the heartbeat report belongs to
   * @param[in] reportCopySetInfo copyset info reported by the heartbeat
   * @param[in] configChInfo configuration changes reported by the heartbeat
   * @param[out] copysetConf configuration to distribute
   *
   * @return true if there's any configuration, false if not
   */
  bool GenCopysetConf(
      MetaServerIdType report_id,
      const ::curvefs::mds::topology::CopySetInfo& report_copy_set_info,
      const ::curvefs::mds::heartbeat::ConfigChangeInfo& config_ch_info,
      ::curvefs::mds::heartbeat::CopySetConf* copyset_conf);

 private:
  /*
   * @brief LeaderGenCopysetConf Deal with info from leader copyset,
   *                             basically it pass the data to the scheduler
   *
   * @param[in] copySetInfo copyset info reported
   * @param[in] configChInfo configuration changes reported by the heartbeat
   * @param[out] copysetConf new configuration that the scheduler generate
   *
   * @return ::curvefs::mds::topology::UNINITIALIZE_ID if there isn't any
   *           configuration to dispatch，otherwise other value
   *           apart from UNINITIALIZE_ID
   */
  MetaServerIdType LeaderGenCopysetConf(
      const ::curvefs::mds::topology::CopySetInfo& copy_set_info,
      const ::curvefs::mds::heartbeat::ConfigChangeInfo& config_ch_info,
      ::curvefs::mds::heartbeat::CopySetConf* copyset_conf);

  /*
   * @brief FollowerGenCopysetConf deal with follower copyset info.
   *                               decide whether the reported metaserver is
   *                               within the copies of a copyset, if not,
   *                               generate corresponding instruction for
   *                               deletion
   *
   * @param[in] reportId metaserver ID of the heartbeat
   * @param[in] reportCopySetInfo copyset info reported
   * @param[in] recordCopySetInfo copyset info recorded by the mds
   * @param[out] copysetConf configuration instruction generated by scheduler
   *
   * @return true if there's any config instruction, false if not
   */
  bool FollowerGenCopysetConf(
      MetaServerIdType report_id,
      const ::curvefs::mds::topology::CopySetInfo& report_copy_set_info,
      const ::curvefs::mds::topology::CopySetInfo& record_copy_set_info,
      ::curvefs::mds::heartbeat::CopySetConf* copyset_conf);

  /*
   * @brief BuildPeerByMetaserverId generate a string in the format of
   *                                 'ip:port:id' according to csId
   *
   * @param[in] csId metaserver ID
   *
   * @return string 'ip:port:id' generated, '' if there's any error
   */
  std::string BuildPeerByMetaserverId(MetaServerIdType ms_id);

  std::shared_ptr<Topology> topo_;
  std::shared_ptr<Coordinator> coordinator_;

  // MDS will start cleaning copysets some time after it started rather than
  // //NOLINT start immediately (delay cleaning), this can avoid error in cases
  // like the example below:
  // 1. An operator: ABC+D(epoch: 8) generated by MDS, and has been dispatched
  // to leader           //NOLINT
  // 2. MDS restart and the operator has lost, the configuration recorded by MDS
  // is ABC(epoch: 8)  //NOLINT
  // 3. Leader has installed a snapshot on D and replayed the journal.
  //    Expired configuration is updated to D, ABE(epoch : 5) for example
  // 4. Heartbeat reported by D, and the config reported is ABE(epoch: 5)
  // 5. Epoch recorded by MDS is larger than what the follower reported.
  //    In this case instruction would have been dispatched to clean copyset on,
  //    but it shouldn't  //NOLINT since D is already a member of the copyset at
  //    this moment
  // Why delay cleaning?
  // In normal cases the leader will report candidates within a heartbeat, and
  // candidates          //NOLINT willnot be cleaned even if they report expired
  // config after that.
  steady_clock::time_point mdsStartTime_;
  // after started for this peroid of time, clean data on followers is enable
  // //NOLINT
  uint64_t cleanFollowerAfterMs_;
};
}  // namespace heartbeat
}  // namespace mds
}  // namespace curvefs

#endif  // CURVEFS_SRC_MDS_HEARTBEAT_COPYSET_CONF_GENERATOR_H_
