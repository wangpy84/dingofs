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
 * Date: Wed Aug 18 15:30:42 CST 2021
 * Author: wuhanqing
 */

#include <butil/at_exit.h>
#include <gtest/gtest.h>

int main(int argc, char* argv[]) {
  // braft need it
  butil::AtExitManager atExit;

  testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
