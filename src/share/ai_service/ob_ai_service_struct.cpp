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

#include "share/ai_service/ob_ai_service_struct.h"
#include "share/schema/ob_column_schema.h"
#include "lib/json/ob_json.h"


#define USING_LOG_PREFIX SHARE

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::json;

namespace oceanbase
{
namespace share
{
namespace
{
// A character that may be part of an unquoted identifier of an ai column expression.
bool is_identifier_char(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
         || '_' == c || '$' == c;
}
} // anonymous namespace

const ObString ObAiModelEndpointInfo::DEFAULT_SCOPE = "ALL";

const char *VALID_PROVIDERS[] = {
  "ALIYUN-OPENAI",
  "ALIYUN-DASHSCOPE",
  "DEEPSEEK",
  "SILICONFLOW",
  "COHERE",
  "HUNYUAN-OPENAI",
  "OPENAI",
  "OLLAMA"
};

#define EXTRACT_JSON_ELEM_STR(json_key, member) \
  EXTRACT_JSON_ELEM_STR_WITH_PROCESS(json_key, member, "void")

#define EXTRACT_JSON_ELEM_STR_WITH_PROCESS(json_key, member, post_process) \
      if (elem.first.case_compare(json_key) == 0) { \
        if (elem.second->json_type() != ObJsonNodeType::J_STRING) { \
          ret = OB_AI_FUNC_PARAM_VALUE_INVALID; \
          LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, elem.first.length(), elem.first.ptr()); \
          LOG_WARN("invalid json type", K(ret), K(elem.first), K(elem.second->json_type())); \
        } else { \
          member = ObString(elem.second->get_data_length(), elem.second->get_data()); \
          post_process; \
        } \
      } else

#define EXTRACT_JSON_ELEM_END() \
  { \
    ret = OB_AI_FUNC_PARAM_INVALID; \
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_INVALID, elem.first.length(), elem.first.ptr()); \
    LOG_WARN("unknown json key param", K(ret), K(elem.first)); \
  }

int ObAiModelEndpointInfo::parse_from_json_base(common::ObArenaAllocator &allocator, const ObString &name, const ObIJsonBase &params_jbase)
{
  int ret = OB_SUCCESS;
  reset();
  name_ = name;
  if (OB_FAIL(merge_delta_endpoint(allocator, params_jbase))) {
  }
  LOG_INFO("parse from json base", K(ret), K(params_jbase), K(params_jbase.json_type()), K(params_jbase.element_count()));
  return ret;
}

int ObAiModelEndpointInfo::deep_copy(
    common::ObIAllocator &allocator,
    const ObAiModelEndpointInfo &other)
{
  int ret = OB_SUCCESS;
  reset();
  endpoint_id_ = other.endpoint_id_;
  if (OB_FAIL(ob_write_string(allocator, other.name_, name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.scope_, scope_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.ai_model_name_, ai_model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.url_, url_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.access_key_, access_key_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.provider_, provider_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_model_name_, request_model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.parameters_, parameters_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_transform_fn_, request_transform_fn_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.response_transform_fn_, response_transform_fn_))) {
  }
  return ret;
}

int ObAiModelEndpointInfo::check_valid() const
{
  int ret = OB_SUCCESS;
  if (name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("name"), "name");
  } else if (scope_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("scope"), "scope");
  } else if (scope_.case_compare(DEFAULT_SCOPE) != 0) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("scope"), "scope");
  } else if (ai_model_name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("ai_model_name"), "ai_model_name");
  } else if (url_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("url"), "url");
  } else if (access_key_.empty() && !is_keyless_provider(provider_)) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("access_key"), "access_key");
    LOG_WARN("access_key is empty", K(ret), K(*this));
  } else if (provider_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("provider"), "provider");
  } else if (!is_valid_provider(provider_)) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("provider"), "provider");
  } else if (!parameters_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("parameters"), "parameters");
  } else if (!request_transform_fn_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("request_transform_fn"), "request_transform_fn");
  } else if (!response_transform_fn_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("response_transform_fn"), "response_transform_fn");
  }
  return ret;
}

bool ObAiModelEndpointInfo::is_valid_provider(const ObString &provider)
{
  bool is_valid = false;
  for (int i = 0; i < ARRAYSIZEOF(VALID_PROVIDERS); i++) {
    if (provider.case_compare(VALID_PROVIDERS[i]) == 0) {
      is_valid = true;
      break;
    }
  }
  return is_valid;
}

bool ObAiModelEndpointInfo::is_keyless_provider(const ObString &provider)
{
  // Only local inference servers run without credentials so far.
  return provider.case_compare("OLLAMA") == 0;
}

int ObAiModelEndpointInfo::merge_delta_endpoint(common::ObArenaAllocator &allocator, const ObIJsonBase &delta_jbase)
{
  int ret = OB_SUCCESS;
  JsonObjectIterator iter = delta_jbase.object_iterator();
  bool has_api_key = false;
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair elem;
    if (OB_FAIL(iter.get_elem(elem))) {
    } else {
      EXTRACT_JSON_ELEM_STR("scope", scope_)
      EXTRACT_JSON_ELEM_STR("ai_model_name", ai_model_name_)
      EXTRACT_JSON_ELEM_STR("url", url_)
      EXTRACT_JSON_ELEM_STR_WITH_PROCESS("access_key", access_key_, has_api_key = true)
      EXTRACT_JSON_ELEM_STR("provider", provider_)
      EXTRACT_JSON_ELEM_STR("request_model_name", request_model_name_)
      EXTRACT_JSON_ELEM_STR("parameters", parameters_)
      EXTRACT_JSON_ELEM_STR("request_transform_fn", request_transform_fn_)
      EXTRACT_JSON_ELEM_STR("response_transform_fn", response_transform_fn_)
      EXTRACT_JSON_ELEM_END()
    }
    iter.next();
  }

  if (OB_SUCC(ret)) {
    // A keyless provider stores an empty string rather than NULL: the row reader treats a
    // null column as an error, while an empty value simply means "no credentials".
    if (is_keyless_provider(provider_) && access_key_.empty()) {
      access_key_.assign_ptr("", 0);
    }
    if (has_api_key && !access_key_.empty() && OB_FAIL(encrypt_access_key_(allocator, access_key_, access_key_))) {
      LOG_WARN("failed to encrypt access key", K(ret));
    } else if (OB_FAIL(check_valid())) {
    }
  }

  LOG_INFO("merge delta endpoint", K(ret), K(delta_jbase), K(delta_jbase.json_type()), K(delta_jbase.element_count()));
  return ret;
}

int ObAiModelEndpointInfo::encrypt_access_key_(ObIAllocator &allocator, const ObString &access_key, ObString &encrypted_access_key)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ob_write_string(allocator, access_key, encrypted_access_key))) {
  }
  return ret;
}

int ObAiModelEndpointInfo::decrypt_access_key_(ObIAllocator &allocator, const ObString &encrypted_access_key, ObString &unencrypted_access_key) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ob_write_string(allocator, encrypted_access_key, unencrypted_access_key))) {
  }
  return ret;
}

const char *EndpointType::ENDPOINT_TYPE_STR[] = {
  "DENSE_EMBEDDING",
  "SPARSE_EMBEDDING",
  "COMPLETION",
  "RERANK",
};

EndpointType::TYPE EndpointType::str_to_endpoint_type(const ObString &type_str)
{
  STATIC_ASSERT(static_cast<int64_t>(EndpointType::MAX_TYPE) == ARRAYSIZEOF(ENDPOINT_TYPE_STR) + 1, "endpoint type str len is mismatch");
  EndpointType::TYPE endpoint_type = EndpointType::INVALID_TYPE;
  bool is_found = false;
  for (int i = 1; i < EndpointType::MAX_TYPE && !is_found; i++) {
    if (type_str.case_compare(ENDPOINT_TYPE_STR[i-1]) == 0) {
      endpoint_type = static_cast<EndpointType::TYPE>(i);
      is_found = true;
    }
  }
  return endpoint_type;
}

int ObAiModelEndpointInfo::get_unencrypted_access_key(common::ObIAllocator &allocator, ObString &unencrypted_access_key) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(decrypt_access_key_(allocator, access_key_, unencrypted_access_key))) {
  }
  return ret;
}

int ObAiServiceModelInfo::parse_from_json_base(const ObString &name, const common::ObIJsonBase &params_jbase)
{
  int ret = OB_SUCCESS;
  reset();
  name_ = name;
  JsonObjectIterator iter = params_jbase.object_iterator();
  ObString type_str;
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair elem;
    if (OB_FAIL(iter.get_elem(elem))) {
    } else {
      EXTRACT_JSON_ELEM_STR("model_name", model_name_)
      EXTRACT_JSON_ELEM_STR_WITH_PROCESS("type", type_str, type_ = EndpointType::str_to_endpoint_type(type_str))
      EXTRACT_JSON_ELEM_END()
    }
    iter.next();
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(check_valid())) {
    }
  }

  LOG_TRACE("parse from json base", K(ret), K(params_jbase), K(params_jbase.json_type()), K(params_jbase.element_count()));
  return ret;
}

int ObAiServiceModelInfo::check_valid() const
{
  int ret = OB_SUCCESS;
  if (name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("name"), "name");
  } else if (model_name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("model_name"), "model_name");
  } else if (type_ == EndpointType::MAX_TYPE) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("type"), "type");
  } else if (type_ == EndpointType::INVALID_TYPE) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("type"), "type");
  }
  return ret;
}

OB_SERIALIZE_MEMBER(ObAiServiceModelInfo, name_, type_, model_name_);

namespace
{
// The definition of an ai column travels inside the column's extended type info, which is
// a persisted varbinary of __all_column: a magic number followed by the length prefixed
// strings of the definition and by the retry budget.
const char *const AI_COLUMN_HIDDEN_COLUMN_NAME_PREFIX[ObAiColumnSpec::HIDDEN_COLUMN_MAX] = {
  "__ai_state_",
  "__ai_request_",
  "__ai_result_spec_",
};

int serialize_str(char *buf, const int64_t buf_len, int64_t &pos, const ObString &str)
{
  int ret = OB_SUCCESS;
  const int32_t len = str.length();
  if (OB_ISNULL(buf) || OB_UNLIKELY(pos < 0 || pos + static_cast<int64_t>(sizeof(len)) > buf_len)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("failed to serialize ai column string", K(ret), K(pos), K(buf_len), K(str));
  } else {
    MEMCPY(buf + pos, &len, sizeof(len));
    pos += sizeof(len);
    if (pos + len > buf_len) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("failed to serialize ai column string", K(ret), K(pos), K(buf_len), K(str));
    } else {
      MEMCPY(buf + pos, str.ptr(), len);
      pos += len;
    }
  }
  return ret;
}

int deserialize_str(const char *buf, const int64_t data_len, int64_t &pos, ObString &str)
{
  int ret = OB_SUCCESS;
  int32_t len = 0;
  if (OB_ISNULL(buf) || OB_UNLIKELY(pos < 0 || pos + static_cast<int64_t>(sizeof(len)) > data_len)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("failed to deserialize ai column string", K(ret), K(pos), K(data_len));
  } else {
    MEMCPY(&len, buf + pos, sizeof(len));
    pos += sizeof(len);
    if (OB_UNLIKELY(len < 0 || pos + len > data_len)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("failed to deserialize ai column string", K(ret), K(pos), K(data_len), K(len));
    } else if (0 == len) {
      str.reset();
    } else {
      str.assign_ptr(const_cast<char *>(buf + pos), len);
      pos += len;
    }
  }
  return ret;
}
} // anonymous namespace

int64_t ObAiColumnSpec::get_serialize_size() const
{
  int64_t size = static_cast<int64_t>(sizeof(int32_t)) + static_cast<int64_t>(sizeof(int64_t))
                 + 3 * static_cast<int64_t>(sizeof(int32_t))
                 + expr_.length() + normalized_expr_.length() + sync_mode_.length()
                 + static_cast<int64_t>(sizeof(int32_t));
  for (int64_t i = 0; i < dep_columns_.count(); ++i) {
    size += static_cast<int64_t>(sizeof(int32_t)) + dep_columns_.at(i).length();
  }
  return size;
}

int ObAiColumnSpec::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(buf)
      || OB_UNLIKELY(pos + static_cast<int64_t>(sizeof(AI_COLUMN_SPEC_MAGIC)) > buf_len)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("failed to serialize ai column spec", K(ret), K(pos), K(buf_len));
  } else {
    MEMCPY(buf + pos, &AI_COLUMN_SPEC_MAGIC, sizeof(AI_COLUMN_SPEC_MAGIC));
    pos += sizeof(AI_COLUMN_SPEC_MAGIC);
    if (OB_FAIL(serialize_str(buf, buf_len, pos, expr_))) {
    } else if (OB_FAIL(serialize_str(buf, buf_len, pos, normalized_expr_))) {
    } else if (OB_FAIL(serialize_str(buf, buf_len, pos, sync_mode_))) {
    } else if (OB_UNLIKELY(pos + static_cast<int64_t>(sizeof(int64_t)) > buf_len)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("failed to serialize ai column spec", K(ret), K(pos), K(buf_len));
    } else {
      MEMCPY(buf + pos, &max_retries_, sizeof(max_retries_));
      pos += sizeof(max_retries_);
      const int32_t dep_count = static_cast<int32_t>(dep_columns_.count());
      if (OB_UNLIKELY(pos + static_cast<int64_t>(sizeof(dep_count)) > buf_len)) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("failed to serialize ai column spec", K(ret), K(pos), K(buf_len));
      } else {
        MEMCPY(buf + pos, &dep_count, sizeof(dep_count));
        pos += sizeof(dep_count);
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < dep_columns_.count(); ++i) {
        if (OB_FAIL(serialize_str(buf, buf_len, pos, dep_columns_.at(i)))) {
        }
      }
    }
  }
  return ret;
}

int ObAiColumnSpec::deserialize(const char *buf, const int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  int32_t magic = 0;
  reset();
  if (OB_ISNULL(buf) || OB_UNLIKELY(pos + static_cast<int64_t>(sizeof(magic)) > data_len)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("failed to deserialize ai column spec", K(ret), K(pos), K(data_len));
  } else {
    MEMCPY(&magic, buf + pos, sizeof(magic));
    pos += sizeof(magic);
    if (OB_UNLIKELY(AI_COLUMN_SPEC_MAGIC != magic && AI_COLUMN_SPEC_MAGIC_V1 != magic)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid ai column spec magic", K(ret), K(magic));
    } else if (OB_FAIL(deserialize_str(buf, data_len, pos, expr_))) {
    } else if (OB_FAIL(deserialize_str(buf, data_len, pos, normalized_expr_))) {
    } else if (OB_FAIL(deserialize_str(buf, data_len, pos, sync_mode_))) {
    } else if (OB_UNLIKELY(pos + static_cast<int64_t>(sizeof(int64_t)) > data_len)) {
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("failed to deserialize ai column spec", K(ret), K(pos), K(data_len));
    } else {
      MEMCPY(&max_retries_, buf + pos, sizeof(max_retries_));
      pos += sizeof(max_retries_);
    }
    if (OB_SUCC(ret) && AI_COLUMN_SPEC_MAGIC == magic) {
      int32_t dep_count = 0;
      if (OB_UNLIKELY(pos + static_cast<int64_t>(sizeof(dep_count)) > data_len)) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("failed to deserialize ai column spec", K(ret), K(pos), K(data_len));
      } else {
        MEMCPY(&dep_count, buf + pos, sizeof(dep_count));
        pos += sizeof(dep_count);
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < static_cast<int64_t>(dep_count); ++i) {
        ObString dep_column;
        if (OB_UNLIKELY(dep_count < 0)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("invalid ai column dependency count", K(ret), K(dep_count));
        } else if (OB_FAIL(deserialize_str(buf, data_len, pos, dep_column))) {
        } else if (OB_FAIL(dep_columns_.push_back(dep_column))) {
        }
      }
    }
  }
  return ret;
}

int ObAiColumnSpec::set_to_column(common::ObIAllocator &allocator,
                                  share::schema::ObColumnSchemaV2 &column) const
{
  int ret = OB_SUCCESS;
  const int64_t buf_len = get_serialize_size();
  char *buf = NULL;
  int64_t pos = 0;
  common::ObSEArray<common::ObString, 1> type_info;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid ai column spec", K(ret), K(expr_), K(normalized_expr_));
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(buf_len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate ai column spec buffer", K(ret), K(buf_len));
  } else if (OB_FAIL(serialize(buf, buf_len, pos))) {
  } else if (OB_FAIL(type_info.push_back(ObString(static_cast<int32_t>(pos), buf)))) {
  } else if (OB_FAIL(column.set_extended_type_info(type_info))) {
  } else if (column.is_ai_column()) {
    // the definition of an ai column normally lives on the ai column itself
  } else {
    // ... unless the ai column cannot own it: a vector column keeps its dimension in the
    // extended type info (and the reader rejects any array whose size is not exactly one),
    // so for those the definition is carried by the hidden result spec column instead
    column.set_ai_column_spec();
  }
  return ret;
}

int ObAiColumnSpec::load_from_column(const share::schema::ObColumnSchemaV2 &column,
                                     ObAiColumnSpec &spec)
{
  int ret = OB_SUCCESS;
  const common::ObIArray<common::ObString> &type_info = column.get_extended_type_info();
  int64_t pos = 0;
  spec.reset();
  if (OB_UNLIKELY(!column.is_ai_column() && !column.is_ai_column_spec())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("not an ai column", K(ret), K(column.get_column_name_str()));
  } else if (OB_UNLIKELY(type_info.count() != 1)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid ai column type info", K(ret), K(type_info.count()));
  } else if (OB_FAIL(spec.deserialize(type_info.at(0).ptr(), type_info.at(0).length(), pos))) {
  }
  return ret;
}

int ObAiColumnSpec::load_from_ai_column(const share::schema::ObTableSchema &table,
                                        const share::schema::ObColumnSchemaV2 &ai_column,
                                        ObAiColumnSpec &spec)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  const share::schema::ObColumnSchemaV2 *spec_column = NULL;
  char name_buf[OB_MAX_COLUMN_NAME_LENGTH];
  ObString spec_column_name;
  spec.reset();
  if (OB_UNLIKELY(!ai_column.is_ai_column())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("not an ai column", K(ret), K(ai_column.get_column_name_str()));
  } else if (OB_SUCCESS == (tmp_ret = load_from_column(ai_column, spec)) && spec.is_valid()) {
    // the definition is stored on the ai column itself
  } else if (OB_FAIL(build_hidden_column_name(HIDDEN_RESULT_SPEC_COLUMN,
                                              ai_column.get_column_name_str(),
                                              name_buf,
                                              sizeof(name_buf),
                                              spec_column_name))) {
  } else if (OB_ISNULL(spec_column = table.get_column_schema(spec_column_name))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the result spec column of an ai column is missing",
             K(ret), K(spec_column_name), K(ai_column.get_column_name_str()));
  } else if (OB_FAIL(load_from_column(*spec_column, spec))) {
  }
  return ret;
}

int ObAiColumnSpec::build_hidden_column_name(const HiddenColumnType type,
                                             const ObString &ai_column_name,
                                             char *buf,
                                             const int64_t buf_len,
                                             ObString &name)
{
  int ret = OB_SUCCESS;
  name.reset();
  if (OB_UNLIKELY(type < HIDDEN_STATE_COLUMN || type >= HIDDEN_COLUMN_MAX) || OB_ISNULL(buf)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(type), KP(buf));
  } else {
    const char *prefix = AI_COLUMN_HIDDEN_COLUMN_NAME_PREFIX[type];
    const int64_t prefix_len = static_cast<int64_t>(strlen(prefix));
    const int64_t total_len = prefix_len + ai_column_name.length();
    if (OB_UNLIKELY(ai_column_name.empty() || total_len > buf_len)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("ai column name is too long", K(ret), K(ai_column_name), K(total_len), K(buf_len));
    } else {
      MEMCPY(buf, prefix, prefix_len);
      MEMCPY(buf + prefix_len, ai_column_name.ptr(), ai_column_name.length());
      name.assign_ptr(buf, static_cast<int32_t>(total_len));
    }
  }
  return ret;
}

bool ObAiColumnSpec::is_dep_column(const ObString &name) const
{
  bool found = false;
  for (int64_t i = 0; !found && i < dep_columns_.count(); ++i) {
    found = (0 == dep_columns_.at(i).case_compare(name));
  }
  return found;
}

bool ObAiColumnSpec::is_referenced_column(const ObString &column_name) const
{
  bool found = is_dep_column(column_name);
  bool in_literal = false;
  char quote = '\0';
  for (int64_t i = 0; !found && i < expr_.length(); ++i) {
    const char c = expr_.ptr()[i];
    if (in_literal) {
      if ('\\' == c) {
        ++i; // an escaped character of the literal
      } else if (c == quote) {
        in_literal = false;
      }
    } else if ('\'' == c || '"' == c || '`' == c) {
      in_literal = true;
      quote = c;
    } else if (is_identifier_char(c)) {
      const int64_t begin = i;
      while (i + 1 < expr_.length() && is_identifier_char(expr_.ptr()[i + 1])) {
        ++i;
      }
      int64_t next = i + 1;
      while (next < expr_.length() && ' ' == expr_.ptr()[next]) {
        ++next;
      }
      // a name that the expression calls is a function of the expression, not a column
      const bool is_call = (next < expr_.length() && '(' == expr_.ptr()[next]);
      found = !is_call
              && 0 == ObString(static_cast<int64_t>(i - begin + 1), expr_.ptr() + begin)
                          .case_compare(column_name);
    }
  }
  return found;
}

ObString ObAiColumnSpec::get_result_instruction(const common::ColumnType type)
{
  ObString instruction;
  if (ob_is_json_tc(type)) {
    instruction = ObString::make_string("Return exactly one valid JSON value.");
  } else if (ob_is_integer_type(type)) {
    instruction = ObString::make_string("Return exactly one base-10 integer.");
  }
  return instruction;
}

} // namespace share
} // namespace oceanbase
