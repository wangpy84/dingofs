/*
 *  Copyright (c) 2020 NetEase Inc.
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
 * Created Date: Saturday March 9th 2019
 * Author: lixiaocui1
 */

#include "mds/kvstorageclient/etcd_client.h"

#include <glog/logging.h>

#include <cassert>

#include "utils/string_util.h"

namespace dingofs {
namespace kvstorage {
int EtcdClientImp::Init(EtcdConf conf, int timeout, int retryTimes) {
  this->timeout_ = timeout;
  this->retryTimes_ = retryTimes;
  return NewEtcdClientV3(conf);
}

void EtcdClientImp::CloseClient() { EtcdCloseClient(); }

int EtcdClientImp::Put(const std::string& key, const std::string& value) {
  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    errCode = EtcdClientPut(timeout_, const_cast<char*>(key.c_str()),
                            const_cast<char*>(value.c_str()), key.size(),
                            value.size());
    needRetry = NeedRetry(errCode);
  } while (needRetry && ++retry <= retryTimes_);

  return errCode;
}

int EtcdClientImp::PutRewithRevision(const std::string& key,
                                     const std::string& value,
                                     int64_t* revision) {
  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    EtcdClientPutRewtihRevision_return res = EtcdClientPutRewtihRevision(
        timeout_, const_cast<char*>(key.c_str()),
        const_cast<char*>(value.c_str()), key.size(), value.size());
    if (res.r0 == EtcdErrCode::EtcdOK) {
      *revision = res.r1;
    }
    errCode = res.r0;
    needRetry = NeedRetry(errCode);
  } while (needRetry && ++retry <= retryTimes_);

  return errCode;
}

int EtcdClientImp::Get(const std::string& key, std::string* out) {
  assert(out != nullptr);
  out->clear();

  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    EtcdClientGet_return res =
        EtcdClientGet(timeout_, const_cast<char*>(key.c_str()), key.size());
    errCode = res.r0;
    needRetry = NeedRetry(errCode);
    if (res.r0 == EtcdErrCode::EtcdOK) {
      *out = std::string(res.r1, res.r1 + res.r2);
      free(res.r1);
    } else if (res.r0 == EtcdErrCode::EtcdKeyNotExist) {
      LOG(INFO) << "key '" << key << "' not exist";
    } else {
      LOG(WARNING) << "get key err: " << res.r0 << ", retry:" << retry
                   << ", needRetry:" << retry;
    }
  } while (needRetry && ++retry <= retryTimes_);

  return errCode;
}

int EtcdClientImp::List(const std::string& startKey, const std::string& endKey,
                        std::vector<std::string>* out) {
  assert(out != nullptr);
  out->clear();

  std::vector<std::pair<std::string, std::string>> kvs;
  int errCode = List(startKey, endKey, &kvs);
  if (errCode != EtcdErrCode::EtcdOK) {
    return errCode;
  }

  for (const auto& kv : kvs) {
    out->push_back(kv.second);
  }

  return errCode;
}

int EtcdClientImp::List(const std::string& startKey, const std::string& endKey,
                        std::vector<std::pair<std::string, std::string>>* out) {
  assert(out != nullptr);
  out->clear();

  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    EtcdClientList_return res = EtcdClientList(
        timeout_, const_cast<char*>(startKey.c_str()),
        const_cast<char*>(endKey.c_str()), startKey.size(), endKey.size());
    errCode = res.r0;
    needRetry = NeedRetry(errCode);
    if (res.r0 != EtcdErrCode::EtcdOK) {
      LOG(WARNING) << "list file of [start:" << startKey << ", end:" << endKey
                   << "] err: " << res.r0 << ", retry: " << retry
                   << ", needRetry: " << needRetry;
    } else {
      for (int i = 0; i < res.r2; i++) {
        EtcdClientGetMultiObject_return objRes =
            EtcdClientGetMultiObject(res.r1, i);
        if (objRes.r0 != EtcdErrCode::EtcdOK) {
          LOG(ERROR) << "get object:" << res.r1 << " index: " << i
                     << "err: " << objRes.r0;
          EtcdClientRemoveObject(res.r1);
          return objRes.r0;
        }

        out->emplace_back(std::string(objRes.r3, objRes.r3 + objRes.r4),
                          std::string(objRes.r1, objRes.r1 + objRes.r2));
        free(objRes.r1);
        free(objRes.r3);
      }
      EtcdClientRemoveObject(res.r1);
    }
  } while (needRetry && ++retry <= retryTimes_);

  return errCode;
}

int EtcdClientImp::Delete(const std::string& key) {
  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    errCode =
        EtcdClientDelete(timeout_, const_cast<char*>(key.c_str()), key.size());
    needRetry = NeedRetry(errCode);
  } while (needRetry && ++retry <= retryTimes_);
  return errCode;
}

int EtcdClientImp::DeleteRewithRevision(const std::string& key,
                                        int64_t* revision) {
  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    EtcdClientDeleteRewithRevision_return res = EtcdClientDeleteRewithRevision(
        timeout_, const_cast<char*>(key.c_str()), key.size());
    if (res.r0 == EtcdErrCode::EtcdOK) {
      *revision = res.r1;
    }
    errCode = res.r0;
    needRetry = NeedRetry(errCode);
  } while (needRetry && ++retry <= retryTimes_);

  return errCode;
}

int EtcdClientImp::TxnN(const std::vector<Operation>& ops) {
  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    if (ops.size() == 2) {
      errCode = EtcdClientTxn2(timeout_, ops[0], ops[1]);
    } else if (ops.size() == 3) {
      errCode = EtcdClientTxn3(timeout_, ops[0], ops[1], ops[2]);
    } else {
      LOG(ERROR) << "do not support Txn " << ops.size();
      return EtcdErrCode::EtcdInvalidArgument;
    }
    needRetry = NeedRetry(errCode);
  } while (needRetry && ++retry <= retryTimes_);
  return errCode;
}

int EtcdClientImp::GetCurrentRevision(int64_t* revision) {
  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    std::string key = "dingotest";
    EtcdClientGet_return res =
        EtcdClientGet(timeout_, const_cast<char*>(key.c_str()), key.size());
    errCode = res.r0;
    needRetry = NeedRetry(errCode);
    if (res.r0 == EtcdErrCode::EtcdOK ||
        res.r0 == EtcdErrCode::EtcdKeyNotExist) {
      *revision = res.r3;
      errCode = EtcdErrCode::EtcdOK;
    } else {
      LOG(WARNING) << "get current revision fail, errcode: " << res.r0
                   << ", retry: " << retry << ", needRetry: " << needRetry;
    }
  } while (needRetry && ++retry <= retryTimes_);

  return errCode;
}

int EtcdClientImp::ListWithLimitAndRevision(const std::string& startKey,
                                            const std::string& endKey,
                                            int64_t limit, int64_t revision,
                                            std::vector<std::string>* values,
                                            std::string* lastKey) {
  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    EtcdClientListWithLimitAndRevision_return res =
        EtcdClientListWithLimitAndRevision(
            timeout_, const_cast<char*>(startKey.c_str()),
            const_cast<char*>(endKey.c_str()), startKey.size(), endKey.size(),
            limit, revision);

    errCode = res.r0;
    needRetry = NeedRetry(errCode);
    if (res.r0 != EtcdErrCode::EtcdOK) {
      LOG(WARNING) << "ListByLimitAndRevision [start:" << startKey
                   << ", end:" << endKey << "] err: " << res.r0
                   << ", retry: " << retry << ", needRetry: " << needRetry;
    } else {
      for (int i = 0; i < res.r2; i++) {
        EtcdClientGetMultiObject_return objRes =
            EtcdClientGetMultiObject(res.r1, i);

        if (objRes.r0 != EtcdErrCode::EtcdOK) {
          LOG(ERROR) << "get object:" << res.r1 << " index:" << i
                     << ", count:" << res.r2 << " err: " << objRes.r0;
          EtcdClientRemoveObject(res.r1);
          return objRes.r0;
        }

        values->emplace_back(std::string(objRes.r1, objRes.r1 + objRes.r2));
        if (i == res.r2 - 1) {
          *lastKey = std::string(objRes.r3, objRes.r3 + objRes.r4);
        }
        free(objRes.r1);
        free(objRes.r3);
      }
    }
    EtcdClientRemoveObject(res.r1);
  } while (needRetry && ++retry <= retryTimes_);

  return errCode;
}

int EtcdClientImp::CompareAndSwap(const std::string& key,
                                  const std::string& preV,
                                  const std::string& target) {
  bool needRetry = false;
  int retry = 0;
  int errCode;
  do {
    errCode = EtcdClientCompareAndSwap(timeout_, const_cast<char*>(key.c_str()),
                                       const_cast<char*>(preV.c_str()),
                                       const_cast<char*>(target.c_str()),
                                       key.size(), preV.size(), target.size());
    needRetry = NeedRetry(errCode);
  } while (needRetry && ++retry <= retryTimes_);
  return errCode;
}

int EtcdClientImp::CampaignLeader(const std::string& pfx,
                                  const std::string& leaderName,
                                  uint32_t sessionInterSec,
                                  uint32_t electionTimeoutMs,
                                  uint64_t* leaderOid) {
  LOG(INFO) << "campaign leader, pfx{" << pfx << "}, leaderName{" << leaderName
            << "}";
  EtcdElectionCampaign_return res = EtcdElectionCampaign(
      const_cast<char*>(pfx.c_str()), pfx.size(),
      const_cast<char*>(leaderName.c_str()), leaderName.size(), sessionInterSec,
      electionTimeoutMs);
  if (res.r0 == EtcdErrCode::EtcdCampaignLeaderSuccess) {
    *leaderOid = res.r1;
  }
  return res.r0;
}

int EtcdClientImp::LeaderObserve(uint64_t leaderOid,
                                 const std::string& leaderName) {
  return EtcdLeaderObserve(leaderOid, const_cast<char*>(leaderName.c_str()),
                           leaderName.size());
}

int EtcdClientImp::LeaderResign(uint64_t leaderOid, uint64_t timeoutMs) {
  return EtcdLeaderResign(leaderOid, timeoutMs);
}

void EtcdClientImp::SetTimeout(int timeout) { this->timeout_ = timeout; }

bool EtcdClientImp::NeedRetry(int errCode) {
  switch (errCode) {
    case EtcdErrCode::EtcdOK:
    case EtcdErrCode::EtcdCanceled:
    case EtcdErrCode::EtcdUnknown:
    case EtcdErrCode::EtcdInvalidArgument:
    case EtcdErrCode::EtcdNotFound:
    case EtcdErrCode::EtcdAlreadyExists:
    case EtcdErrCode::EtcdPermissionDenied:
    case EtcdErrCode::EtcdOutOfRange:
    case EtcdErrCode::EtcdUnimplemented:
    case EtcdErrCode::EtcdInternal:
    case EtcdErrCode::EtcdDataLoss:
    case EtcdErrCode::EtcdUnauthenticated:
    case EtcdErrCode::EtcdTxnUnkownOp:
    case EtcdErrCode::EtcdKeyNotExist:
    case EtcdErrCode::EtcdValueNotEqual:
      return false;

    case EtcdErrCode::EtcdDeadlineExceeded:
    case EtcdErrCode::EtcdResourceExhausted:
    case EtcdErrCode::EtcdFailedPrecondition:
    case EtcdErrCode::EtcdAborted:
    case EtcdErrCode::EtcdUnavailable:
      return true;
  }
  return false;
}

}  // namespace kvstorage
}  // namespace dingofs
