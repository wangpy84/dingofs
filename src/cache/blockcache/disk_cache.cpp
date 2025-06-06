/*
 * Copyright (c) 2024 dingodb.com, Inc. All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Project: DingoFS
 * Created Date: 2024-08-19
 * Author: Jingli Chen (Wine93)
 */

#include "cache/blockcache/disk_cache.h"

#include <fcntl.h>
#include <glog/logging.h>

#include <memory>

#include "absl/cleanup/cleanup.h"
#include "base/math/math.h"
#include "base/string/string.h"
#include "base/time/time.h"
#include "cache/blockcache/block_reader.h"
#include "cache/blockcache/disk_cache_layout.h"
#include "cache/blockcache/disk_cache_manager.h"
#include "cache/blockcache/disk_cache_metric.h"
#include "cache/blockcache/disk_state_machine_impl.h"
#include "cache/common/common.h"
#include "cache/utils/access_log.h"
#include "cache/utils/aio.h"
#include "cache/utils/aio_queue.h"
#include "cache/utils/aio_uring.h"
#include "cache/utils/aio_usrbio.h"
#include "cache/utils/local_filesystem.h"
#include "cache/utils/phase_timer.h"
#include "cache/utils/posix.h"
#include "stub/metric/metric.h"

namespace dingofs {
namespace cache {
namespace blockcache {

using dingofs::base::math::kMiB;
using dingofs::base::string::GenUuid;
using dingofs::base::string::TrimSpace;
using dingofs::base::time::TimeNow;
using dingofs::cache::utils::IoRing;
using dingofs::cache::utils::LinuxIoUring;
using dingofs::cache::utils::LogGuard;
using dingofs::cache::utils::Phase;
using dingofs::cache::utils::PhaseTimer;
using dingofs::cache::utils::Posix;
using dingofs::cache::utils::Usrbio;

DiskCache::DiskCache(DiskCacheOption option)
    : option_(option), use_direct_write_(false), running_(false) {
  metric_ = std::make_shared<DiskCacheMetric>(option);
  layout_ = std::make_shared<DiskCacheLayout>(option.cache_dir());
  disk_state_machine_ = std::make_shared<DiskStateMachineImpl>(metric_);
  disk_state_health_checker_ =
      std::make_unique<DiskStateHealthChecker>(layout_, disk_state_machine_);
  fs_ = std::make_shared<LocalFileSystem>([&](Status status) {
    if (status.ok()) {
      disk_state_machine_->IOSucc();
    } else {
      disk_state_machine_->IOErr();
    }
    return status;
  });
  manager_ = std::make_shared<DiskCacheManager>(option.cache_size_mb() * kMiB,
                                                layout_, fs_, metric_);
  loader_ = std::make_unique<DiskCacheLoader>(layout_, fs_, manager_, metric_);

  std::shared_ptr<IoRing> io_ring;
  if (option_.filesystem_type() == "3fs") {
    io_ring =
        std::make_shared<Usrbio>(option_.cache_dir(), option_.ioring_blksize());
  } else {
    io_ring = std::make_shared<LinuxIoUring>();
  }
  aio_queue_ = std::make_shared<AioQueueImpl>(io_ring);
}

Status DiskCache::Init(UploadFunc uploader) {
  if (running_.exchange(true)) {
    return Status::OK();  // already running
  }

  Status status = aio_queue_->Init(option_.ioring_iodepth());
  if (status.ok()) {
    status = CreateDirs();
    if (status.ok()) {
      status = LoadLockFile();
    }
  }
  if (!status.ok()) {
    return status;
  }

  uploader_ = uploader;
  metric_->Init();   // for restart
  DetectDirectIO();  // detect filesystem whether support direct IO, filesystem
                     // like tmpfs (/dev/shm) will not support it.
  disk_state_machine_->Start();         // monitor disk state
  disk_state_health_checker_->Start();  // probe disk health
  manager_->Start();                    // manage disk capacity, cache expire
  loader_->Start(uuid_, uploader);      // load stage and cache block
  metric_->SetUuid(uuid_);
  metric_->SetRunningStatus(kCacheUp);

  LOG(INFO) << "Disk cache (dir=" << GetRootDir() << ") is up.";
  return Status::OK();
}

Status DiskCache::Shutdown() {
  if (!running_.exchange(false)) {
    return Status::OK();
  }

  LOG(INFO) << "Disk cache (dir=" << GetRootDir() << ") is shutting down...";

  loader_->Stop();
  manager_->Stop();
  disk_state_health_checker_->Stop();
  disk_state_machine_->Stop();
  aio_queue_->Shutdown();
  metric_->SetRunningStatus(kCacheDown);

  LOG(INFO) << "Disk cache (dir=" << GetRootDir() << ") is down.";
  return Status::OK();
}

Status DiskCache::Stage(const BlockKey& key, const Block& block,
                        BlockContext ctx) {
  Status status;
  PhaseTimer timer;
  auto metric_guard = ::absl::MakeCleanup([&] {
    if (status.ok()) {
      metric_->AddStageBlock(1);
    } else {
      metric_->AddStageSkip();
    }
  });
  LogGuard log([&]() {
    return StrFormat("stage(%s,%d): %s%s", key.Filename(), block.size,
                     status.ToString(), timer.ToString());
  });

  status = Check(kWantExec | kWantStage);
  if (!status.ok()) {
    return status;
  }

  timer.NextPhase(Phase::kWriteFile);
  std::string stage_path(GetStagePath(key));
  std::string cache_path(GetCachePath(key));
  status =
      fs_->WriteFile(stage_path, block.data, block.size, use_direct_write_);
  if (!status.ok()) {
    return status;
  }

  timer.NextPhase(Phase::kLink);
  status = fs_->HardLink(stage_path, cache_path);
  if (status.ok()) {
    timer.NextPhase(Phase::kCacheAdd);
    manager_->Add(key, CacheValue(block.size, TimeNow()), BlockPhase::kStaging);
  } else {
    LOG(WARNING) << "Link " << stage_path << " to " << cache_path
                 << " failed: " << status.ToString();
    status = Status::OK();  // ignore link error
  }

  timer.NextPhase(Phase::kEnqueueUpload);
  uploader_(key, stage_path, ctx);
  return status;
}

Status DiskCache::RemoveStage(const BlockKey& key, BlockContext /*ctx*/) {
  Status status;
  auto metric_guard = ::absl::MakeCleanup([&] {
    if (status.ok()) {
      metric_->AddStageBlock(-1);
    }
  });
  LogGuard log([&]() {
    return StrFormat("removestage(%s): %s", key.Filename(), status.ToString());
  });

  // NOTE: we will try to delete stage file even if the disk cache
  //       is down or unhealthy, so we remove the Check(...) here.
  status = fs_->RemoveFile(GetStagePath(key));
  manager_->Add(key, CacheValue(), BlockPhase::kUploaded);
  return status;
}

Status DiskCache::Cache(const BlockKey& key, const Block& block) {
  Status status;
  PhaseTimer timer;
  LogGuard log([&]() {
    return StrFormat("cache(%s,%d): %s%s", key.Filename(), block.size,
                     status.ToString(), timer.ToString());
  });

  status = Check(kWantExec | kWantCache);
  if (!status.ok()) {
    return status;
  }

  timer.NextPhase(Phase::kWriteFile);
  status = fs_->WriteFile(GetCachePath(key), block.data, block.size);
  if (!status.ok()) {
    return status;
  }

  timer.NextPhase(Phase::kCacheAdd);
  manager_->Add(key, CacheValue(block.size, TimeNow()), BlockPhase::kCached);
  return status;
}

Status DiskCache::Load(const BlockKey& key,
                       std::shared_ptr<BlockReader>& reader) {
  Status status;
  PhaseTimer timer;
  auto metric_guard = ::absl::MakeCleanup([&] {
    if (status.ok()) {
      metric_->AddCacheHit();
    } else {
      metric_->AddCacheMiss();
    }
  });
  LogGuard log([&]() {
    return StrFormat("load(%s): %s%s", key.Filename(), status.ToString(),
                     timer.ToString());
  });

  status = Check(kWantExec);
  if (!status.ok()) {
    return status;
  } else if (!IsCached(key)) {
    return Status::NotFound("cache not found");
  }

  timer.NextPhase(Phase::kOpenFile);
  status = NewBlockReader(key, reader);

  // Delete corresponding key of block which maybe already deleted by accident.
  if (status.IsNotFound()) {
    manager_->Delete(key);
  }
  return status;
}

Status DiskCache::NewBlockReader(const BlockKey& key,
                                 std::shared_ptr<BlockReader>& reader) {
  return fs_->Do([&]() {
    int fd;
    auto status = Posix::Open(GetCachePath(key), O_RDONLY, &fd);
    if (!status.ok()) {
      return status;
    }

    if (option_.filesystem_type() == "3fs") {
      reader = std::make_shared<RemoteBlockReader>(fd, option_.ioring_blksize(),
                                                   fs_, aio_queue_);
    } else {
      reader = std::make_shared<LocalBlockReader>(fd, fs_);
    }
    return status;
  });
}

bool DiskCache::IsCached(const BlockKey& key) {
  std::string cache_path = GetCachePath(key);
  if (manager_->Exist(key)) {
    return true;
  } else if (loader_->IsLoading() && fs_->FileExists(cache_path)) {
    return true;
  }
  return false;
}

std::string DiskCache::Id() { return uuid_; }

Status DiskCache::CreateDirs() {
  std::vector<std::string> dirs{
      layout_->GetRootDir(),
      layout_->GetStageDir(),
      layout_->GetCacheDir(),
      layout_->GetProbeDir(),
  };

  for (const auto& dir : dirs) {
    auto status = fs_->MkDirs(dir);
    if (!status.ok()) {
      return status;
    }
  }
  return Status::OK();
}

Status DiskCache::LoadLockFile() {
  size_t length;
  std::shared_ptr<char> buffer;
  auto lock_path = layout_->GetLockPath();
  auto status = fs_->ReadFile(lock_path, buffer, &length);
  if (status.ok()) {
    uuid_ = TrimSpace(std::string(buffer.get(), length));
  } else if (status.IsNotFound()) {
    uuid_ = GenUuid();
    status = fs_->WriteFile(lock_path, uuid_.c_str(), uuid_.size());
  }
  return status;
}

void DiskCache::DetectDirectIO() {
  std::string filepath = layout_->GetDetectPath();
  auto status = fs_->Do([filepath]() {
    int fd;
    auto status = Posix::Create(filepath, &fd, true);
    Posix::Close(fd);
    Posix::Unlink(filepath);
    return status;
  });

  if (status.ok()) {
    use_direct_write_ = true;
    LOG(INFO) << "The filesystem of disk cache (dir=" << layout_->GetRootDir()
              << ") supports direct IO.";
  } else {
    use_direct_write_ = false;
    LOG(INFO) << "The filesystem of disk cache (dir=" << layout_->GetRootDir()
              << ") not support direct IO, using buffer IO, detect rc = "
              << status.ToString();
  }
  metric_->SetUseDirectWrite(use_direct_write_);
}

// Check cache status:
//   1. check running status (UP/DOWN)
//   2. check disk healthy (HEALTHY/UNHEALTHY)
//   3. check disk free space (FULL or NOT)
Status DiskCache::Check(uint8_t want) {
  if (!running_.load(std::memory_order_relaxed)) {
    return Status::CacheDown("disk cache is down");
  }

  if ((want & kWantExec) && !IsHealthy()) {
    return Status::CacheUnhealthy("disk cache is unhealthy");
  } else if ((want & kWantStage) && StageFull()) {
    return Status::CacheFull("disk cache is full");
  } else if ((want & kWantCache) && CacheFull()) {
    return Status::CacheFull("disk cache is full");
  }
  return Status::OK();
}

bool DiskCache::IsLoading() const { return loader_->IsLoading(); }

bool DiskCache::IsHealthy() const {
  return disk_state_machine_->GetDiskState() == DiskState::kDiskStateNormal;
}

bool DiskCache::StageFull() const { return manager_->StageFull(); }

bool DiskCache::CacheFull() const { return manager_->CacheFull(); }

std::string DiskCache::GetRootDir() const { return layout_->GetRootDir(); }

std::string DiskCache::GetStagePath(const BlockKey& key) const {
  return layout_->GetStagePath(key);
}

std::string DiskCache::GetCachePath(const BlockKey& key) const {
  return layout_->GetCachePath(key);
}

}  // namespace blockcache
}  // namespace cache
}  // namespace dingofs
