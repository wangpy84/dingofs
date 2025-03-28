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
 * Created Date: Tue Jul 27 17:11:44 CST 2021
 * Author: wuhanqing
 */

#include "mds/codec/codec.h"

#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include "dingofs/mds.pb.h"

namespace dingofs {
namespace mds {
namespace codec {

using pb::common::FSType;
using pb::common::Volume;

TEST(CodecTest, TestEncodeProtobufMessage) {
  pb::mds::FsInfo fsinfo;

  // empty message
  ASSERT_FALSE(fsinfo.IsInitialized());

  fsinfo.set_fsid(1);
  fsinfo.set_fsname("hello");
  fsinfo.set_status(pb::mds::FsStatus::INITED);
  fsinfo.set_rootinodeid(1);
  fsinfo.set_capacity(8192);
  fsinfo.set_blocksize(4096);
  fsinfo.set_mountnum(0);
  fsinfo.set_fstype(FSType::TYPE_VOLUME);
  fsinfo.set_enablesumindir(false);
  fsinfo.set_owner("test");
  fsinfo.set_txsequence(0);
  fsinfo.set_txowner("owner");

  Volume volume;
  volume.set_blocksize(4096);
  volume.set_volumename("/dingofs");
  volume.set_user("test");
  volume.set_blockgroupsize(4096);
  volume.set_bitmaplocation(pb::common::BitmapLocation::AtEnd);
  volume.set_slicesize(1ULL * 1024 * 1024 * 1024);
  volume.set_autoextend(false);

  fsinfo.mutable_detail()->set_allocated_volume(new Volume(volume));

  std::string value1;
  std::string value2;

  ASSERT_TRUE(EncodeProtobufMessage(fsinfo, &value1));
  ASSERT_TRUE(fsinfo.SerializeToString(&value2));
  ASSERT_EQ(value1, value2);
}

TEST(CodecTest, TestDecodeProtobufMessage) {
  pb::mds::FsInfo fsinfo;

  fsinfo.set_fsid(1);
  fsinfo.set_fsname("hello");
  fsinfo.set_status(pb::mds::FsStatus::INITED);
  fsinfo.set_rootinodeid(1);
  fsinfo.set_capacity(8192);
  fsinfo.set_blocksize(4096);
  fsinfo.set_mountnum(0);
  fsinfo.set_fstype(FSType::TYPE_VOLUME);
  fsinfo.set_enablesumindir(false);
  fsinfo.set_owner("test");
  fsinfo.set_txsequence(0);
  fsinfo.set_txowner("owner");

  Volume volume;
  volume.set_blocksize(4096);
  volume.set_volumename("/dingofs");
  volume.set_user("test");
  volume.set_blockgroupsize(4096);
  volume.set_bitmaplocation(pb::common::BitmapLocation::AtEnd);
  volume.set_slicesize(1ULL * 1024 * 1024 * 1024);
  volume.set_autoextend(false);

  fsinfo.mutable_detail()->set_allocated_volume(new Volume(volume));

  std::string value;
  ASSERT_TRUE(EncodeProtobufMessage(fsinfo, &value));

  pb::mds::FsInfo decoded;
  ASSERT_TRUE(DecodeProtobufMessage(value, &decoded));

  ASSERT_TRUE(
      google::protobuf::util::MessageDifferencer::Equals(fsinfo, decoded));
}

}  // namespace codec
}  // namespace mds
}  // namespace dingofs
