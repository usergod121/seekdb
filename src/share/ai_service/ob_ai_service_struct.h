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

#ifndef OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_
#define OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "common/json_type/ob_json_base.h"
#include "share/schema/ob_schema_struct.h"
#include "share/ai_service/ob_ai_model_info.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
}
namespace share
{
namespace schema
{
class ObColumnSchemaV2;
}

// Layer-neutral AI service endpoint info from user side.
class ObAiModelEndpointInfo
{
  friend class ObAiServiceProxy;
public:
  ObAiModelEndpointInfo() { reset(); }
  ~ObAiModelEndpointInfo() = default;

  void reset()
  {
    name_.reset();
    scope_ = DEFAULT_SCOPE;
    ai_model_name_.reset();
    url_.reset();
    access_key_.reset();
    provider_.reset();
    request_model_name_.reset();
    parameters_.reset();
    request_transform_fn_.reset();
    response_transform_fn_.reset();
    endpoint_id_ = OB_INVALID_ID;
  }

  int parse_from_json_base(common::ObArenaAllocator &allocator,const ObString &name, const common::ObIJsonBase &params_jbase);
  int merge_delta_endpoint(common::ObArenaAllocator &allocator, const ObIJsonBase &delta_endpoint);
  int deep_copy(common::ObIAllocator &allocator, const ObAiModelEndpointInfo &other);
  int check_valid() const;

  const ObString &get_name() const { return name_; }
  const ObString &get_scope() const { return scope_; }
  const ObString &get_url() const { return url_; }
  const ObString &get_encrypted_access_key() const { return access_key_; }
  int get_unencrypted_access_key(common::ObIAllocator &allocator, ObString &unencrypted_access_key) const;
  const ObString &get_ai_model_name() const { return ai_model_name_; }
  const ObString &get_request_model_name() const { return request_model_name_; }
  const ObString &get_provider() const { return provider_; }
  const ObString &get_parameters() const { return parameters_; }
  const ObString &get_request_transform_fn() const { return request_transform_fn_; }
  const ObString &get_response_transform_fn() const { return response_transform_fn_; }
  uint64_t get_endpoint_id() const { return endpoint_id_; }
  void set_endpoint_id(uint64_t endpoint_id) { endpoint_id_ = endpoint_id; }

  TO_STRING_KV(K_(name),
               K_(scope),
               K_(endpoint_id),
               K_(url),
               K_(access_key),
               K_(ai_model_name),
               K_(provider),
               K_(request_model_name),
               K_(parameters),
               K_(request_transform_fn),
               K_(response_transform_fn));
private:
  static const ObString DEFAULT_SCOPE;
  static bool is_valid_provider(const ObString &provider);
  // providers served locally, such as ollama, do not need an access key
  static bool is_keyless_provider(const ObString &provider);
  int encrypt_access_key_(common::ObIAllocator &allocator, const ObString &access_key, ObString &encrypted_access_key);
  int decrypt_access_key_(common::ObIAllocator &allocator, const ObString &encrypted_access_key, ObString &unencrypted_access_key) const;
private:
  uint64_t endpoint_id_;
  ObString name_;
  ObString scope_;
  ObString ai_model_name_;
  ObString url_;
  ObString access_key_;
  ObString provider_;
  ObString request_model_name_;
  ObString parameters_;
  ObString request_transform_fn_;
  ObString response_transform_fn_;
};

// Definition of an ai column. It is stored with the column schema, so it is persisted in
// __all_column and survives a restart. The state/request/result spec columns that an ai
// column is materialized with are hidden columns of the same table, their names are
// derived from the ai column name (see build_hidden_column_name()), so the definition
// does not have to track column ids.
class ObAiColumnSpec
{
public:
  enum HiddenColumnType
  {
    HIDDEN_STATE_COLUMN = 0,
    HIDDEN_REQUEST_COLUMN = 1,
    HIDDEN_RESULT_SPEC_COLUMN = 2,
    HIDDEN_COLUMN_MAX = 3,
  };

  ObAiColumnSpec() { reset(); }
  ~ObAiColumnSpec() = default;

  void reset()
  {
    expr_.reset();
    normalized_expr_.reset();
    sync_mode_.reset();
    max_retries_ = 0;
    dep_columns_.reuse();
  }

  bool is_valid() const { return !expr_.empty() && !normalized_expr_.empty(); }

  int set_expr(const ObString &expr) { expr_ = expr; return OB_SUCCESS; }
  int set_normalized_expr(const ObString &normalized_expr) { normalized_expr_ = normalized_expr; return OB_SUCCESS; }
  int set_sync_mode(const ObString &sync_mode) { sync_mode_ = sync_mode; return OB_SUCCESS; }
  void set_max_retries(const int64_t max_retries) { max_retries_ = max_retries; }

  const ObString &get_expr() const { return expr_; }
  const ObString &get_normalized_expr() const { return normalized_expr_; }
  const ObString &get_sync_mode() const { return sync_mode_; }
  int64_t get_max_retries() const { return max_retries_; }
  // Names of the columns the expression reads. When one of them is assigned by an update,
  // the generated value is invalidated.
  int add_dep_column(const common::ObString &name) { return dep_columns_.push_back(name); }
  const common::ObIArray<common::ObString> &get_dep_columns() const { return dep_columns_; }
  bool is_dep_column(const common::ObString &name) const;
  // Whether the expression of this ai column reads the given column, e.g. whether an update
  // that assigns `content` invalidates an ai column defined as
  // ai_complete('model', concat('Classify: ', content)). The names inside a string literal and
  // the names of the functions the expression calls are not columns, and a name is a whole
  // identifier, so `content` is not read by an expression that only mentions `content_id`.
  // The dependency list is asked first, it is empty as long as the ddl leaves it empty.
  bool is_referenced_column(const common::ObString &column_name) const;

  int64_t get_serialize_size() const;
  int serialize(char *buf, const int64_t buf_len, int64_t &pos) const;
  // The strings of a deserialized spec point into buf, they are not copied.
  int deserialize(const char *buf, const int64_t data_len, int64_t &pos);

  // Attach this spec to column. The spec is deep copied into the column schema.
  int set_to_column(common::ObIAllocator &allocator, share::schema::ObColumnSchemaV2 &column) const;
  // Read back the spec of an ai column. The strings point into the column schema.
  static int load_from_column(const share::schema::ObColumnSchemaV2 &column, ObAiColumnSpec &spec);
  // Read back the spec of an ai column of a table: the definition lives on the ai column
  // itself unless that column's type needs its extended type info (a vector column keeps its
  // dimension there), in which case the hidden result spec column carries it.
  static int load_from_ai_column(const share::schema::ObTableSchema &table,
                                 const share::schema::ObColumnSchemaV2 &ai_column,
                                 ObAiColumnSpec &spec);

  // Build the name of a hidden column of an ai column, e.g. "__ai_state_tags".
  static int build_hidden_column_name(const HiddenColumnType type,
                                      const ObString &ai_column_name,
                                      char *buf,
                                      const int64_t buf_len,
                                      ObString &name);

  // Instruction appended to the ai column expression, it tells the model which form the
  // result of the declared column type has to have. Empty when no instruction is needed.
  static ObString get_result_instruction(const common::ColumnType type);

private:
  static const int32_t AI_COLUMN_SPEC_MAGIC = 0x41494332; // "AIC2"
  // version 1 of the definition did not carry the dependency columns
  static const int32_t AI_COLUMN_SPEC_MAGIC_V1 = 0x41494331; // "AIC1"
  ObString expr_;
  ObString normalized_expr_;
  ObString sync_mode_;
  int64_t max_retries_;
  common::ObSEArray<common::ObString, 4> dep_columns_;
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_SERVICE_OB_AI_SERVICE_STRUCT_H_
