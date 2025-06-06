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
 * Created Date: 2021-9-9
 * Author: chengyi
 */

#include "metaserver/s3/metaserver_s3.h"

#include <glog/logging.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>

#include "aws/mock_s3_adapter.h"

namespace dingofs {
namespace metaserver {
using ::testing::_;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::Return;
using ::testing::SetArgPointee;

class ClientS3Test : public testing::Test {
 protected:
  ClientS3Test() {}
  ~ClientS3Test() override = default;
  void SetUp() override {
    s3Adapter_ = std::make_shared<dingofs::blockaccess::aws::MockS3Adapter>();
    s3Client_ = new S3ClientImpl();
    s3Client_->SetAdaptor(s3Adapter_);
  }

  void TearDown() override {
    delete s3Client_;
    s3Client_ = nullptr;
  }

 protected:
  std::shared_ptr<dingofs::blockaccess::aws::MockS3Adapter> s3Adapter_;
  S3ClientImpl* s3Client_;
};

TEST_F(ClientS3Test, delete_object_not_exist) {
  EXPECT_CALL(*s3Adapter_, DeleteObject(_)).WillRepeatedly(Return(-1));
  EXPECT_CALL(*s3Adapter_, ObjectExist(_)).WillRepeatedly(Return(false));
  int ret = s3Client_->Delete("123");
  ASSERT_EQ(ret, 1);
}

TEST_F(ClientS3Test, delete_error) {
  EXPECT_CALL(*s3Adapter_, DeleteObject(_)).WillRepeatedly(Return(-1));
  EXPECT_CALL(*s3Adapter_, ObjectExist(_)).WillRepeatedly(Return(true));
  int ret = s3Client_->Delete("123");
  ASSERT_EQ(ret, -1);
}

TEST_F(ClientS3Test, delete_sucess) {
  EXPECT_CALL(*s3Adapter_, ObjectExist(_)).WillRepeatedly(Return(true));
  EXPECT_CALL(*s3Adapter_, DeleteObject(_)).WillRepeatedly(Return(0));
  int ret = s3Client_->Delete("123");
  ASSERT_EQ(ret, 0);
}

TEST_F(ClientS3Test, delete_batch_fail) {
  EXPECT_CALL(*s3Adapter_, DeleteObjects(_)).WillOnce(Return(-1));
  int ret = s3Client_->DeleteBatch({"123", "456"});
  ASSERT_EQ(ret, -1);
}

TEST_F(ClientS3Test, delete_batch_sucess) {
  EXPECT_CALL(*s3Adapter_, DeleteObjects(_)).WillOnce(Return(0));
  int ret = s3Client_->DeleteBatch({"123", "456"});
  ASSERT_EQ(ret, 0);
}
}  // namespace metaserver
}  // namespace dingofs
