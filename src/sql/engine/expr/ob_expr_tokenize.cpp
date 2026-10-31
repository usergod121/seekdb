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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#include <atomic>

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
  } else if (OB_FAIL(ObJsonExprHelper::pack_json_res(expr,
                                                     ctx,
                                                     temp_allocator,
                                                     json_result,
                                                     expr_datum))) {
  }

  return ret;
}

namespace
{
// OB_NEWx / OB_DELETEx expand the type into a destructor call, so the json types are named through
// local aliases here.
typedef common::ObJsonArray FtJsonArray;
typedef common::ObJsonString FtJsonString;

// The tokenizer is a pure function of the text, the parser, its arguments, the collation and the
// output mode, and the same text is tokenized again and again (a query string, a document template,
// a repeated SELECT). Remembering the produced words turns those repeats into a copy of the word
// list. Entries hold their own copies of the key and of the words, and a hit is only taken after
// comparing the key byte for byte.
struct FtTokenizeCacheEntry
{
  FtTokenizeCacheEntry *next_;
  uint64_t signature_;
  int64_t text_len_;
  int64_t parser_len_;
  int64_t props_len_;
  int32_t word_count_;
  const char *text_;
  const char *parser_;
  const char *props_;
  // [int32 word_len][word bytes...] repeated word_count_ times, in the produced order
  const char *words_;
};

class FtTokenizeCache
{
public:
  static constexpr int64_t BUCKET_CNT = 128;
  static constexpr int64_t MAX_BYTES = 8LL * 1024 * 1024;
  static constexpr int64_t MAX_WORDS = 4096;
  static constexpr int64_t MAX_TEXT_LEN = 64LL * 1024;

public:
  static FtTokenizeCache &instance()
  {
    static thread_local FtTokenizeCache cache;
    return cache;
  }

  static bool enabled()
  {
    // TEMPORARY (A/B switch, delete before commit)
    static const bool on = (nullptr == ::getenv("OB_FTS_TOKENIZE_CACHE_OFF"));
    return on;
  }

  static uint64_t signature(const common::ObCollationType collation,
                            const common::ObObjType type,
                            const int64_t mode,
                            const common::ObString &parser,
                            const common::ObString &props)
  {
    uint64_t hash = 1469598103934665603ULL;
    hash = mix(hash, static_cast<uint64_t>(collation));
    hash = mix(hash, static_cast<uint64_t>(type));
    hash = mix(hash, static_cast<uint64_t>(mode));
    hash = mix_bytes(hash, parser.ptr(), parser.length());
    hash = mix_bytes(hash, props.ptr(), props.length());
    return hash;
  }

  // Reproduce the JSON array from the remembered word list, in the produced order.
  bool lookup(const uint64_t signature,
              const common::ObString &text,
              const common::ObString &parser,
              const common::ObString &props,
              common::ObIAllocator &allocator,
              common::ObIJsonBase *&result)
  {
    bool served = false;
    std::lock_guard<std::mutex> guard(mutex_);
    for (FtTokenizeCacheEntry *entry = buckets_[slot_of(signature)];
         !served && nullptr != entry;
         entry = entry->next_) {
      if (entry->signature_ == signature
          && entry->text_len_ == text.length()
          && entry->parser_len_ == parser.length()
          && entry->props_len_ == props.length()
          && 0 == memcmp(entry->text_, text.ptr(), text.length())
          && 0 == memcmp(entry->parser_, parser.ptr(), parser.length())
          && 0 == memcmp(entry->props_, props.ptr(), props.length())) {
        served = build_json(entry, allocator, result);
      }
    }
    return served;
  }

  void store(const uint64_t signature,
             const common::ObString &text,
             const common::ObString &parser,
             const common::ObString &props,
             const storage::ObFTWordMap &words)
  {
    const int64_t word_count = words.size();
    if (word_count <= 0 || word_count > MAX_WORDS || text.length() > MAX_TEXT_LEN) {
      return;
    }
    int64_t words_bytes = 0;
    for (storage::ObFTWordMap::const_iterator it = words.begin(); it != words.end(); ++it) {
      words_bytes += static_cast<int64_t>(sizeof(int32_t))
                     + it->first.get_word().get_string().length();
    }
    const int64_t entry_bytes = static_cast<int64_t>(sizeof(FtTokenizeCacheEntry))
                                + text.length() + parser.length() + props.length() + words_bytes;
    char *const block = static_cast<char *>(::malloc(entry_bytes));
    if (nullptr == block) {
      return;
    }
    FtTokenizeCacheEntry *const entry = new (block) FtTokenizeCacheEntry();
    char *const text_copy = block + sizeof(FtTokenizeCacheEntry);
    char *const parser_copy = text_copy + text.length();
    char *const props_copy = parser_copy + parser.length();
    char *const words_copy = props_copy + props.length();
    memcpy(text_copy, text.ptr(), text.length());
    memcpy(parser_copy, parser.ptr(), parser.length());
    memcpy(props_copy, props.ptr(), props.length());
    char *pos = words_copy;
    for (storage::ObFTWordMap::const_iterator it = words.begin(); it != words.end(); ++it) {
      const common::ObString word = it->first.get_word().get_string();
      const int32_t len = static_cast<int32_t>(word.length());
      memcpy(pos, &len, sizeof(len));
      memcpy(pos + sizeof(len), word.ptr(), len);
      pos += sizeof(len) + len;
    }
    entry->signature_ = signature;
    entry->text_len_ = text.length();
    entry->parser_len_ = parser.length();
    entry->props_len_ = props.length();
    entry->word_count_ = static_cast<int32_t>(word_count);
    entry->text_ = text_copy;
    entry->parser_ = parser_copy;
    entry->props_ = props_copy;
    entry->words_ = words_copy;
    std::lock_guard<std::mutex> guard(mutex_);
    if (bytes_ + entry_bytes > MAX_BYTES) {
      reset_locked();
    }
    const int64_t slot = slot_of(signature);
    entry->next_ = buckets_[slot];
    buckets_[slot] = entry;
    bytes_ += entry_bytes;
  }

private:
  FtTokenizeCache() : bytes_(0) { memset(buckets_, 0, sizeof(buckets_)); }

  static uint64_t mix(const uint64_t hash, const uint64_t value)
  {
    return (hash ^ value) * 1099511628211ULL;
  }
  static uint64_t mix_bytes(uint64_t hash, const char *bytes, const int64_t len)
  {
    const unsigned char *p = reinterpret_cast<const unsigned char *>(bytes);
    for (int64_t i = 0; i < len; ++i) {
      hash = mix(hash, p[i]);
    }
    return hash;
  }
  static int64_t slot_of(const uint64_t signature)
  {
    return static_cast<int64_t>(signature & (BUCKET_CNT - 1));
  }
  static bool build_json(const FtTokenizeCacheEntry *entry,
                         common::ObIAllocator &allocator,
                         common::ObIJsonBase *&result)
  {
    bool ok = false;
    FtJsonArray *array = nullptr;
    if (OB_ISNULL(array = OB_NEWx(FtJsonArray, &allocator, &allocator))) {
    } else {
      ok = true;
      const char *pos = entry->words_;
      for (int32_t i = 0; ok && i < entry->word_count_; ++i) {
        int32_t len = 0;
        memcpy(&len, pos, sizeof(len));
        const char *word = pos + sizeof(len);
        FtJsonString *token = nullptr;
        const common::ObString key(len, word);
        if (OB_ISNULL(token = OB_NEWx(FtJsonString, &allocator, key))) {
          ok = false;
        } else if (OB_SUCCESS != array->append(token)) {
          OB_DELETEx(FtJsonString, &allocator, token);
          ok = false;
        }
        pos = word + len;
      }
      if (ok) {
        result = array;
      } else {
        OB_DELETEx(FtJsonArray, &allocator, array);
      }
    }
    return ok;
  }
  void reset_locked()
  {
    for (int64_t i = 0; i < BUCKET_CNT; ++i) {
      FtTokenizeCacheEntry *entry = buckets_[i];
      while (nullptr != entry) {
        FtTokenizeCacheEntry *next = entry->next_;
        ::free(entry);
        entry = next;
      }
      buckets_[i] = nullptr;
    }
    bytes_ = 0;
  }

private:
  std::mutex mutex_;
  FtTokenizeCacheEntry *buckets_[BUCKET_CNT];
  int64_t bytes_;
};
} // namespace

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;
  bool served_from_cache = false;
  uint64_t cache_signature = 0;
  // only the plain token array is memoised: it is the shape the expression returns for a bare call
  const bool cacheable = (TokenizeParam::OUTPUT_MODE::DEFAULT == param.output_mode_)
                         && FtTokenizeCache::enabled()
                         && param.fulltext_.length() > 0;

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.parser_property_))) {
  } else if (cacheable
             && FALSE_IT(cache_signature = FtTokenizeCache::signature(
                             param.meta_.get_collation_type(),
                             param.meta_.get_type(),
                             static_cast<int64_t>(param.output_mode_),
                             param.parser_name_,
                             param.properties_))) {
  } else if (cacheable
             && FALSE_IT(served_from_cache = FtTokenizeCache::instance().lookup(
                             cache_signature, param.fulltext_, param.parser_name_,
                             param.properties_, allocator, result))) {
  } else if (served_from_cache) {
    // the array was reproduced from the words this text produced before
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else if (cacheable) {
        FtTokenizeCache::instance().store(cache_signature, param.fulltext_, param.parser_name_,
                                          param.properties_, token_map);
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;
  if (OB_FAIL(parser.parse_from_str(parser_name_.ptr(), parser_name_.length()))) {
  } else if (properties.empty()) {
    // No parser arguments were passed, so the defaults of the parser are exactly
    // what the reform below would inject: use them directly instead of building a
    // JSON document and parsing it back on every evaluation.
    if (OB_FAIL(parser_property_.set_default_for_parser(parser))) {
    }
  } else {
    storage::ObFTParserJsonProps parser_properties;
    if (OB_FAIL(parser_properties.init())) {
    } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
    } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                               ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                               true))) {
    } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
    } else if (OB_FAIL(parser_property_.parse_for_parser_helper(parser, properties_))) {
    }
  }

  return ret;
}

namespace
{
// Loading the built-in dictionary tables of a parser is process wide and
// idempotent, and the loader itself remembers that it happened.  Remembering it
// here as well keeps every TOKENIZE call from rebuilding the loader key, taking
// the dictionary lock, looking the loader up in its hash map and bumping its
// reference count, which is a measurable part of a short TOKENIZE call.
std::atomic<ObCharsetType> g_ik_dict_loaded_charset(ObCharsetType::CHARSET_INVALID);
} // namespace

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (!need_to_load_dic) {
  } else {
    const ObCharsetType charset = ObCharset::charset_type_by_coll(meta_.get_collation_type());
    if (ObCharsetType::CHARSET_INVALID == charset) {
      ret = OB_INVALID_ARGUMENT;
    } else if (g_ik_dict_loaded_charset.load(std::memory_order_relaxed) == charset) {
      // already loaded by an earlier statement.
    } else if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                   ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                   ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                   dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    } else {
      g_ik_dict_loaded_charset.store(charset, std::memory_order_relaxed);
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
