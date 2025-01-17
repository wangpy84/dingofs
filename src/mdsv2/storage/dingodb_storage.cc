// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mdsv2/storage/dingodb_storage.h"

#include <memory>
#include <string>

#include "dingofs/error.pb.h"
#include "fmt/format.h"
#include "glog/logging.h"
#include "mdsv2/common/helper.h"
#include "mdsv2/common/logging.h"

namespace dingofs {

namespace mdsv2 {

DEFINE_int32(dingodb_replica_num, 3, "backend store replicas");

DEFINE_int32(dingodb_scan_batch_size, 1000, "dingodb scan batch size");

const uint32_t kTxnKeepAliveMs = 10 * 1000;

static dingodb::sdk::KVPair ToKVPair(const KeyValue& kv) { return dingodb::sdk::KVPair{kv.key, kv.value}; }

static void KvPairToKeyValue(const dingodb::sdk::KVPair& kv_pair, KeyValue& kv) {
  kv.key = kv_pair.key;
  kv.value = kv_pair.value;
}

static void KvPairsToKeyValues(const std::vector<dingodb::sdk::KVPair>& kv_pairs, std::vector<KeyValue>& kvs) {
  kvs.reserve(kv_pairs.size());
  for (const auto& kv_pair : kv_pairs) {
    kvs.emplace_back(KeyValue{KeyValue::OpType::kPut, kv_pair.key, kv_pair.value});
  }
}

static std::vector<dingodb::sdk::KVPair> ToKVPairs(const std::vector<KeyValue>& kvs) {
  std::vector<dingodb::sdk::KVPair> kv_pairs;
  kv_pairs.reserve(kvs.size());
  for (const auto& kv : kvs) {
    kv_pairs.emplace_back(dingodb::sdk::KVPair{kv.key, kv.value});
  }

  return kv_pairs;
}

bool DingodbStorage::Init(const std::string& addr) {
  DINGO_LOG(INFO) << fmt::format("Init dingo storage, addr({}).", addr);

  auto status = dingodb::sdk::Client::BuildFromAddrs(addr, &client_);
  CHECK(status.ok()) << fmt::format("build dingo sdk client fail, error: {}", status.ToString());

  return true;
}

bool DingodbStorage::Destroy() {
  delete client_;

  return true;
}

Status DingodbStorage::CreateTable(const std::string& name, const TableOption& option, int64_t& table_id) {
  dingodb::sdk::RegionCreator* creator = nullptr;
  auto status = client_->NewRegionCreator(&creator);
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  int64_t region_id = 0;
  status = creator->SetRegionName(name)
               .SetRange(option.start_key, option.end_key)
               .SetReplicaNum(FLAGS_dingodb_replica_num)
               .Wait(true)
               .Create(region_id);
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  table_id = region_id;

  return Status::OK();
}

Status DingodbStorage::DropTable(int64_t table_id) {
  auto status = client_->DropRegion(table_id);
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return Status::OK();
}

Status DingodbStorage::IsExistTable(const std::string& start_key, const std::string& end_key) {
  dingodb::sdk::Coordinator* coordinator{nullptr};
  auto status = client_->NewCoordinator(&coordinator);
  CHECK(status.ok()) << fmt::format("new dingo sdk coordinator fail, error: {}", status.ToString());

  std::vector<int64_t> region_ids;
  status = coordinator->ScanRegions(start_key, end_key, region_ids);
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return region_ids.empty() ? Status(pb::error::ENOT_FOUND, "table not exist") : Status::OK();
}

DingodbStorage::TxnPtr DingodbStorage::NewTxn() {
  dingodb::sdk::TransactionOptions options;
  options.isolation = dingodb::sdk::TransactionIsolation::kSnapshotIsolation;
  options.kind = dingodb::sdk::kOptimistic;
  options.keep_alive_ms = kTxnKeepAliveMs;

  dingodb::sdk::Transaction* txn = nullptr;
  auto status = client_->NewTransaction(options, &txn);
  CHECK(status.ok()) << fmt::format("new transaction fail, error: {}", status.ToString());

  return DingodbStorage::TxnPtr(txn);
}

Status DingodbStorage::Put(WriteOption option, const std::string& key, const std::string& value) {
  auto txn = NewTxn();
  if (txn == nullptr) {
    return Status(pb::error::EBACKEND_STORE, "new transaction fail");
  }

  auto status = option.is_if_absent ? txn->PutIfAbsent(key, value) : txn->Put(key, value);
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  status = txn->PreCommit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }
  status = txn->Commit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return Status::OK();
}

Status DingodbStorage::Put(WriteOption option, KeyValue& kv) {
  auto txn = NewTxn();
  if (txn == nullptr) {
    return Status(pb::error::EBACKEND_STORE, "new transaction fail");
  }

  if (kv.opt_type == KeyValue::OpType::kPut) {
    auto status = option.is_if_absent ? txn->PutIfAbsent(kv.key, kv.value) : txn->Put(kv.key, kv.value);
    if (!status.ok()) {
      return Status(pb::error::EBACKEND_STORE, status.ToString());
    }
  } else if (kv.opt_type == KeyValue::OpType::kDelete) {
    auto status = txn->Delete(kv.key);
    if (!status.ok()) {
      return Status(pb::error::EBACKEND_STORE, status.ToString());
    }
  }

  auto status = txn->PreCommit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }
  status = txn->Commit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return Status::OK();
}

Status DingodbStorage::Put(WriteOption option, const std::vector<KeyValue>& kvs) {
  auto txn = NewTxn();
  if (txn == nullptr) {
    return Status(pb::error::EBACKEND_STORE, "new transaction fail");
  }

  for (const auto& kv : kvs) {
    if (kv.opt_type == KeyValue::OpType::kPut) {
      auto status = option.is_if_absent ? txn->PutIfAbsent(kv.key, kv.value) : txn->Put(kv.key, kv.value);
      if (!status.ok()) {
        return Status(pb::error::EBACKEND_STORE, status.ToString());
      }
    } else if (kv.opt_type == KeyValue::OpType::kDelete) {
      auto status = txn->Delete(kv.key);
      if (!status.ok()) {
        return Status(pb::error::EBACKEND_STORE, status.ToString());
      }
    }
  }

  auto status = txn->PreCommit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }
  status = txn->Commit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return Status::OK();
}

Status DingodbStorage::Get(const std::string& key, std::string& value) {
  auto txn = NewTxn();
  if (txn == nullptr) {
    return Status(pb::error::EBACKEND_STORE, "new transaction fail");
  }

  auto status = txn->Get(key, value);
  if (!status.ok()) {
    return status.IsNotFound() ? Status(pb::error::ENOT_FOUND, status.ToString())
                               : Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  status = txn->PreCommit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }
  status = txn->Commit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return Status::OK();
}

Status DingodbStorage::Scan(const Range& range, std::vector<KeyValue>& kvs) {
  auto txn = NewTxn();
  if (txn == nullptr) {
    return Status(pb::error::EBACKEND_STORE, "new transaction fail");
  }

  std::vector<dingodb::sdk::KVPair> kv_pairs;
  auto status = txn->Scan(range.start_key, range.end_key, FLAGS_dingodb_scan_batch_size, kv_pairs);
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  KvPairsToKeyValues(kv_pairs, kvs);

  status = txn->PreCommit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }
  status = txn->Commit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return Status::OK();
}

Status DingodbStorage::Delete(const std::string& key) {
  auto txn = NewTxn();
  if (txn == nullptr) {
    return Status(pb::error::EBACKEND_STORE, "new transaction fail");
  }

  auto status = txn->Delete(key);
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  status = txn->PreCommit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }
  status = txn->Commit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return Status::OK();
}

Status DingodbStorage::Delete(const std::vector<std::string>& keys) {
  auto txn = NewTxn();
  if (txn == nullptr) {
    return Status(pb::error::EBACKEND_STORE, "new transaction fail");
  }

  for (const auto& key : keys) {
    auto status = txn->Delete(key);
    if (!status.ok()) {
      return Status(pb::error::EBACKEND_STORE, status.ToString());
    }
  }

  auto status = txn->PreCommit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }
  status = txn->Commit();
  if (!status.ok()) {
    return Status(pb::error::EBACKEND_STORE, status.ToString());
  }

  return Status::OK();
}

}  // namespace mdsv2
}  // namespace dingofs