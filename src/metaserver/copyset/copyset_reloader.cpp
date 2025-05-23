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
 * Project: dingo
 * Date: Sun 22 Aug 2021 10:40:42 AM CST
 * Author: wuhanqing
 */

#include "metaserver/copyset/copyset_reloader.h"

#include <glog/logging.h>

#include <iomanip>
#include <string>
#include <vector>

#include "absl/memory/memory.h"
#include "metaserver/common/types.h"
#include "metaserver/copyset/copyset_node_manager.h"
#include "metaserver/copyset/types.h"
#include "metaserver/copyset/utils.h"
#include "utils/string_util.h"
#include "utils/timeutility.h"
#include "utils/uri_parser.h"

namespace dingofs {
namespace metaserver {
namespace copyset {

using ::dingofs::utils::TaskThreadPool;
using ::dingofs::utils::TimeUtility;
using ::dingofs::utils::UriParser;

CopysetReloader::CopysetReloader(CopysetNodeManager* copysetNodeManager)
    : nodeManager_(copysetNodeManager),
      options_(),
      taskPool_(nullptr),
      running_(false) {}

bool CopysetReloader::Init(const CopysetNodeOptions& options) {
  if (options.loadConcurrency < 1) {
    LOG(ERROR) << "Copyset reloader init failed, load concurrency should "
                  "great than 1";
    return false;
  }

  options_ = options;
  taskPool_ = absl::make_unique<TaskThreadPool<>>();
  taskPool_->Start(options_.loadConcurrency);

  return true;
}

bool CopysetReloader::ReloadCopysets() {
  if (running_.exchange(true)) {
    return true;
  }

  std::string dataDir = UriParser::GetPathFromUri(options_.dataUri);

  if (!options_.localFileSystem->DirExists(dataDir)) {
    LOG(INFO) << "Data path '" << dataDir << "' not exist";
    return true;
  }

  std::vector<std::string> copysets;
  if (0 != options_.localFileSystem->List(dataDir, &copysets)) {
    LOG(ERROR) << "Failed to get copyset list from data path " << dataDir;
    return false;
  }

  LOG(INFO) << "Begin to load copysets under '" << dataDir << "'";
  for (const auto& copyset : copysets) {
    if (!ReloadOneCopyset(copyset)) {
      LOG(ERROR) << "Reload copyset " << copyset
                 << " failed, data path: " << dataDir;
      return false;
    }
  }

  if (!WaitLoadFinish()) {
    LOG(ERROR) << "Load copysets under '" << dataDir << "' failed";
    return false;
  }

  LOG(INFO) << "Load copysets under '" << dataDir << "' success";

  return true;
}

bool CopysetReloader::ReloadOneCopyset(const std::string& copyset) {
  LOG(INFO) << "Found copyset " << copyset;

  GroupNid groupid = 0;
  if (!dingofs::utils::StringToUll(copyset, &groupid)) {
    LOG(ERROR) << "Parse " << copyset << " to GroupNid failed";
    return false;
  }

  PoolId poolId = GetPoolId(groupid);
  CopysetId copysetId = GetCopysetId(groupid);

  LOG(INFO) << "Parse " << copyset << " as "
            << ToGroupIdString(poolId, copysetId);

  taskPool_->Enqueue(&CopysetReloader::LoadCopyset, this, poolId, copysetId);

  return true;
}

void CopysetReloader::LoadCopyset(PoolId poolId, CopysetId copysetId) {
  LOG(INFO) << "Begin to load copyset: " << ToGroupIdString(poolId, copysetId);

  auto begin = TimeUtility::GetTimeofDayMs();
  braft::Configuration conf;

  bool success =
      nodeManager_->CreateCopysetNode(poolId, copysetId, conf, false);
  if (!success) {
    LOG(WARNING) << "Failed to create copyset "
                 << ToGroupIdString(poolId, copysetId);
    load_copyset_errors_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  auto* copyset = nodeManager_->GetCopysetNode(poolId, copysetId);
  CheckCopysetUntilLoadFinished(copyset);

  LOG(INFO) << "Load copyset " << ToGroupIdString(poolId, copysetId)
            << " success, time used(ms): "
            << TimeUtility::GetTimeofDayMs() - begin;
}

bool CopysetReloader::CheckCopysetUntilLoadFinished(CopysetNode* node) {
  if (!node) {
    LOG(WARNING) << "node ptr is null";
    return false;
  }

  uint32_t retryTimes = 0;
  PoolId poolId = node->GetPoolId();
  CopysetId copysetId = node->GetCopysetId();
  const std::string copysetName = ToGroupIdString(poolId, copysetId);

  while (retryTimes < options_.checkRetryTimes) {
    if (running_.load(std::memory_order_relaxed)) {
      return false;
    }

    braft::NodeStatus leaderStatus;
    bool success = node->GetLeaderStatus(&leaderStatus);

    if (!success) {
      ++retryTimes;

      usleep(1000 * options_.raftNodeOptions.election_timeout_ms);
      continue;
    }

    braft::NodeStatus status;
    node->GetStatus(&status);

    // last log of the current replica lags behind the first log saved on
    // the leader, in this case, current replica will be restored by
    // installing snapshots which can be ignored to avoid blocking the
    // thread
    bool mayInstallSnapshot = leaderStatus.first_index > status.last_index;
    if (mayInstallSnapshot) {
      LOG(WARNING) << "Copyset " << copysetName
                   << " may installing snapshot, stop checking. "
                   << "first log index on leader is: "
                   << leaderStatus.first_index
                   << ", last log index on current node is: "
                   << status.last_index;
      return false;
    }

    int64_t margin = leaderStatus.committed_index - status.known_applied_index;
    bool catchup = margin < static_cast<int64_t>(options_.finishLoadMargin);
    if (catchup) {
      LOG(INFO) << "Load copyset " << copysetName
                << " finished, leader committed index: "
                << leaderStatus.committed_index
                << ", node applied index: " << status.known_applied_index;
      return true;
    }

    retryTimes = 0;

    usleep(1000 * options_.checkLoadMarginIntervalMs);
  }

  LOG(WARNING) << "Check copyset " << copysetName << " failled";
  return false;
}

bool CopysetReloader::WaitLoadFinish() {
  while (running_.load(std::memory_order_relaxed) &&
         taskPool_->QueueSize() != 0) {
    ::sleep(1);
  }

  taskPool_->Stop();
  taskPool_.reset();

  return load_copyset_errors_.load(std::memory_order_relaxed) <= 0;
}

}  // namespace copyset
}  // namespace metaserver
}  // namespace dingofs
