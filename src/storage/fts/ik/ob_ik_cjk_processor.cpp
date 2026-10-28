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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ik/ob_ik_cjk_processor.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
int ObIKCJKProcessor::push_hit(const ObDATrieHit &hit)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(hits_cnt_ >= MAX_LIVE_HITS)) {
    // Unreachable with the built in dictionaries, see MAX_LIVE_HITS.
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("too many live dictionary hits", K(ret), K_(hits_cnt));
  } else {
    // ObDATrieHit::operator= deliberately keeps the dictionary pointer of the
    // target, because the DAT reader relies on that when it extends a hit in place,
    // so the pointer has to be carried into the slot explicitly.
    hits_[hits_cnt_].dict_ = hit.dict_;
    hits_[hits_cnt_] = hit;
    ++hits_cnt_;
  }
  return ret;
}

int ObIKCJKProcessor::do_process(TokenizeContext &ctx,
                                 const char *ch,
                                 const uint8_t char_len,
                                 const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;
  const ObString single_word(char_len, ch);

  if (ObFTCharUtil::CharType::USELESS != type) {
    // handle previous hits_ first and then check from this char
    int64_t i = 0;
    while (OB_SUCC(ret) && i < hits_cnt_) {
      ObDATrieHit &hit = hits_[i];
      if (OB_FAIL(dict_main_->match_with_hit(single_word, hit, hit))) {
      } else if (hit.is_match()) {
        if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                  hit.start_pos_,
                                  hit.end_pos_ - hit.start_pos_,
                                  hit.char_cnt_,
                                  ObIKTokenType::IK_CHINESE_TOKEN))) {
        } else {
          // match will record the start_cursor
          ++i;
        }
      } else if (hit.is_prefix()) {
        // nothing
        ++i;
      } else if (hit.is_unmatch()) {
        // This hit died: drop it and shift the survivors down one slot, which keeps
        // their order without touching the allocator.  The dictionary pointer has to
        // be moved by hand, see push_hit().
        for (int64_t j = i + 1; j < hits_cnt_; ++j) {
          hits_[j - 1].dict_ = hits_[j].dict_;
          hits_[j - 1] = hits_[j];
        }
        --hits_cnt_;
      } else {
        ret = OB_UNEXPECT_INTERNAL_ERROR;
      }
    }
    if (OB_SUCC(ret)) {
      // Start from this char
      // find range of dict
      ObDATrieHit hit(dict_main_, ctx.get_cursor());
      if (OB_FAIL(dict_main_->match(single_word, hit))) {
      } else if (hit.is_match()) {
        // output token
        if (OB_FAIL(push_hit(hit))) {
        } else if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                         ctx.get_cursor(),
                                         char_len,
                                         1,
                                         ObIKTokenType::IK_CHINESE_TOKEN))) {
        }
      } else if (hit.is_prefix()) {
        if (OB_FAIL(push_hit(hit))) {
        }
      } else {
        // ignore mismatch
      }
    }
  } else {
    // stop previous match
    hits_cnt_ = 0;
  }

  if (OB_FAIL(ret) || ctx.is_last()) {
    hits_cnt_ = 0;
  }
  return ret;
}

} //  namespace storage
} //  namespace oceanbase
