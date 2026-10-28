/*
 * Copyright (c) 2025 OceanBase.
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

#ifndef _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_
#define _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_

#include "lib/allocator/ob_allocator.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_processor.h"

namespace oceanbase
{
namespace storage
{
class ObFTDictHub;
class ObIKCJKProcessor : public ObIIKProcessor
{
public:
  ObIKCJKProcessor() : dict_main_(nullptr), cjk_start_(-1), cjk_end_(-1), hits_cnt_(0) {}
  ~ObIKCJKProcessor() override = default;

  void init(const ObIFTDict &dict_main) { dict_main_ = &dict_main; }

public:
  int do_process(TokenizeContext &ctx,
                 const char *ch,
                 const uint8_t char_len,
                 const ObFTCharUtil::CharType type) override;

private:
  int push_hit(const ObDATrieHit &hit);

private:
  // One live hit per start position, and a hit only survives while it is still a
  // prefix of a dictionary word, so the number of live hits is bounded by the
  // longest dictionary word: 16 characters for the built in IK dictionary.  The
  // hits therefore live in a fixed array and the per character walk never
  // allocates.
  static constexpr int64_t MAX_LIVE_HITS = 32;

  const ObIFTDict *dict_main_;
  int64_t cjk_start_;
  int64_t cjk_end_;
  ObDATrieHit hits_[MAX_LIVE_HITS];
  int64_t hits_cnt_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObIKCJKProcessor);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_IK_OB_IK_CJK_PROCESSOR_H_
