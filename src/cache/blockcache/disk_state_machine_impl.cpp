// Copyright (c) 2024 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "cache/blockcache/disk_state_machine_impl.h"

#include <glog/logging.h>

#include <functional>
#include <memory>

#include "base/time/time.h"
#include "utils/executor/timer_impl.h"
#include "cache/blockcache/disk_state_machine.h"

namespace dingofs {
namespace cache {
namespace blockcache {

void NormalDiskState::IOErr() {
  io_error_count_.fetch_add(1);
  if (io_error_count_.load() > FLAGS_disk_state_normal2unstable_io_error_num) {
    disk_state_machine->OnEvent(DiskStateEvent::kDiskStateEventUnstable);
  }
}

void NormalDiskState::Tick() { io_error_count_.store(0); }

void UnstableDiskState::IOSucc() {
  io_succ_count_.fetch_add(1);
  if (io_succ_count_.load() > FLAGS_disk_state_unstable2normal_io_succ_num) {
    disk_state_machine->OnEvent(DiskStateEvent::kDiskStateEventNormal);
  }
}

void UnstableDiskState::Tick() {
  uint64_t now =
      duration_cast<seconds>(steady_clock::now().time_since_epoch()).count();
  if (now - start_time_ > (uint64_t)FLAGS_disk_state_unstable2down_s) {
    disk_state_machine->OnEvent(DiskStateEvent::kDiskStateEventDown);
  }

  io_succ_count_.store(0);
}

DiskStateMachineImpl::DiskStateMachineImpl(
    std::shared_ptr<DiskCacheMetric> metric)
    : state_(std::make_unique<BaseDiskState>(this)), metric_(metric) {}

bool DiskStateMachineImpl::Start() {
  dingofs::utils::WriteLockGuard lk(rw_lock_);

  if (running_) {
    return true;
  }

  state_ = std::make_unique<NormalDiskState>(this);
  metric_->SetHealthyStatus(DiskStateToString(state_->GetDiskState()));

  bthread::ExecutionQueueOptions options;
  options.bthread_attr = BTHREAD_ATTR_NORMAL;

  if (bthread::execution_queue_start(&disk_event_queue_id_, &options,
                                     EventThread, this) != 0) {
    LOG(ERROR) << "Fail start execution queue for process event";
    return false;
  }

  timer_ = std::make_unique<TimerImpl>();
  CHECK(timer_->Start());

  running_ = true;

  timer_->Add([this] { TickTock(); }, FLAGS_disk_state_tick_duration_s * 1000);

  LOG(INFO) << "Success start disk state machine";

  return true;
}

bool DiskStateMachineImpl::Stop() {
  dingofs::utils::WriteLockGuard lk(rw_lock_);

  if (!running_) {
    return true;
  }

  LOG(INFO) << "Try to stop disk state machine";

  running_ = false;

  if (bthread::execution_queue_stop(disk_event_queue_id_) != 0) {
    LOG(ERROR) << "Fail stop execution queue for process event";
    return false;
  }

  if (bthread::execution_queue_join(disk_event_queue_id_) != 0) {
    LOG(ERROR) << "Fail join execution queue for process event";
    return false;
  }

  timer_->Stop();

  metric_->SetHealthyStatus(DiskStateToString(kDiskStateUnknown));

  return true;
}

void DiskStateMachineImpl::TickTock() {
  dingofs::utils::WriteLockGuard lk(rw_lock_);

  if (!running_) {
    return;
  }

  state_->Tick();

  timer_->Add([this] { TickTock(); }, FLAGS_disk_state_tick_duration_s * 1000);
}

void DiskStateMachineImpl::OnEvent(DiskStateEvent event) {
  CHECK_EQ(0, bthread::execution_queue_execute(disk_event_queue_id_, event));
}

int DiskStateMachineImpl::EventThread(
    void* meta, bthread::TaskIterator<DiskStateEvent>& iter) {
  if (iter.is_queue_stopped()) {
    LOG(INFO) << "Execution queue is stopped";
    return 0;
  }

  auto* state_machine = reinterpret_cast<DiskStateMachineImpl*>(meta);

  for (; iter; ++iter) {
    state_machine->ProcessEvent(*iter);
  }

  return 0;
}

void DiskStateMachineImpl::ProcessEvent(DiskStateEvent event) {
  dingofs::utils::WriteLockGuard lk(rw_lock_);

  LOG(INFO) << "ProcessEvent event: " << DiskStateEventToString(event)
            << " in state:" << DiskStateToString(state_->GetDiskState());

  switch (state_->GetDiskState()) {
    case DiskState::kDiskStateNormal:
      if (event == kDiskStateEventUnstable) {
        state_ = std::make_unique<UnstableDiskState>(this);
      }
      break;
    case kDiskStateUnStable:
      if (event == kDiskStateEventNormal) {
        state_ = std::make_unique<NormalDiskState>(this);
      } else if (event == kDiskStateEventDown) {
        state_ = std::make_unique<DownDiskState>(this);
      }
      break;
    case kDiskStateUnknown:
    case kDiskStateDown:
      break;
    default:
      LOG(FATAL) << "Unknown disk state " << state_->GetDiskState();
  }

  metric_->SetHealthyStatus(DiskStateToString(state_->GetDiskState()));
  LOG(INFO) << "After process, current disk state is "
            << DiskStateToString(state_->GetDiskState());
}

}  // namespace blockcache
}  // namespace cache
}  // namespace dingofs
