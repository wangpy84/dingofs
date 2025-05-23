/*
 * Copyright (c) 2025 dingodb.com, Inc. All Rights Reserved
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
 * Created Date: 2025-05-07
 * Author: Jingli Chen (Wine93)
 */

#ifndef DINGOFS_SRC_OPTIONS_CACHE_APP_H_
#define DINGOFS_SRC_OPTIONS_CACHE_APP_H_

#include "options/cache/block_cache.h"
#include "options/cache/cache_group_node.h"
#include "options/cache/global.h"
#include "options/cache/remote_block_cache.h"

// options level:
//   app -> { global , cache_group_node, remote_cache }
//   cache_group_node -> block_cache -> disk_cache -> disk_state
//   remote_block_cache -> remote_node

namespace dingofs {
namespace options {
namespace cache {

class AppOption : public BaseOption {
  BIND_suboption(global_option, "global", GlobalOption);
  BIND_suboption(cache_group_node_option, "cache_group_node",
                 CacheGroupNodeOption);
  BIND_suboption(remote_block_cache_option, "remote_block_cache",
                 RemoteBlockCacheOption);
};

}  // namespace cache
}  // namespace options
}  // namespace dingofs

#endif  // DINGOFS_SRC_OPTIONS_CACHE_APP_H_
