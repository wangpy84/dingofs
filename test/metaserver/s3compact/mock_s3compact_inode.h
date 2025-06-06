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

/*************************************************************************
> File Name: mock_s3compact_impl.h
> Author:
> Created Time: Tue 7 Sept 2021
 ************************************************************************/

#ifndef DINGOFS_TEST_METASERVER_S3COMPACT_MOCK_S3COMPACT_INODE_H_
#define DINGOFS_TEST_METASERVER_S3COMPACT_MOCK_S3COMPACT_INODE_H_

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "dingofs/common.pb.h"
#include "dingofs/metaserver.pb.h"
#include "metaserver/compaction/s3compact_inode.h"

using dingofs::metaserver::copyset::CopysetNode;
using dingofs::pb::common::PartitionInfo;
using dingofs::pb::metaserver::MetaStatusCode;
using dingofs::pb::metaserver::S3ChunkInfoList;

namespace dingofs {
namespace metaserver {

class MockCompactInodeJob : public CompactInodeJob {
 public:
  using CompactInodeJob::CompactInodeJob;

  MetaStatusCode UpdateInode(
      CopysetNode* copysetNode, const PartitionInfo& pinfo, uint64_t inodeId,
      ::google::protobuf::Map<uint64_t, S3ChunkInfoList>&& s3ChunkInfoAdd,
      ::google::protobuf::Map<uint64_t, S3ChunkInfoList>&& s3ChunkInfoRemove) {
    return UpdateInode_rvr(copysetNode, pinfo, inodeId, s3ChunkInfoAdd,
                           s3ChunkInfoRemove);
  }
  MOCK_METHOD5(
      UpdateInode_rvr,
      MetaStatusCode(CopysetNode*, const PartitionInfo&, uint64_t,
                     ::google::protobuf::Map<uint64_t, S3ChunkInfoList>,
                     ::google::protobuf::Map<uint64_t, S3ChunkInfoList>));
};

class MockCopysetNodeWrapper : public CopysetNodeWrapper {
 public:
  MockCopysetNodeWrapper() : CopysetNodeWrapper(nullptr) {}
  MOCK_METHOD0(IsLeaderTerm, bool());
  MOCK_METHOD0(IsValid, bool());
};

}  // namespace metaserver
}  // namespace dingofs
#endif  // DINGOFS_TEST_METASERVER_S3COMPACT_MOCK_S3COMPACT_INODE_H_
