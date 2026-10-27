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

#ifndef OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_COLUMN_INFER_H_
#define OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_COLUMN_INFER_H_

#include "lib/string/ob_string.h"
#include "lib/ob_define.h"
#include "common/object/ob_object.h"
#include "sql/engine/file_sql/ob_file_sql_def.h"

namespace oceanbase
{
namespace sql
{

/// Deterministic column type inference for the text based file formats.
///
/// Values are folded into a set of observations and the final type is a pure
/// function of that set, so a file always yields the same schema:
///
///   only NULL / empty        -> VARCHAR, nullable
///   BOOLEAN only             -> BOOLEAN
///   integer only             -> BIGINT
///   integer + double         -> DOUBLE
///   DATE only                -> DATE
///   DATE + DATETIME          -> DATETIME
///   anything mixed with text -> VARCHAR
///
/// A column is nullable as soon as one row carried no value for it.
class ObFileColumnInfer
{
public:
  ObFileColumnInfer() { reset(); }

  /// Fold one raw field into the observations.  An empty field means NULL when
  /// `empty_is_null` is set, which is the CSV/JSONL default.
  void observe(const common::ObString &value, bool empty_is_null = true);

  /// Resolve the observations into the output column types.
  void resolve(ObObjType &type, const char *&type_name, int64_t &type_name_len,
               bool &nullable) const;

  /// Reset for reuse on the next column.
  void reset();

  int64_t value_count() const { return value_count_; }
  bool saw_null() const { return saw_null_; }

  /// Value classifiers, exposed for the readers and for unit testing.
  static bool is_bool_value(const common::ObString &value);
  static bool is_integer_value(const common::ObString &value);
  static bool is_double_value(const common::ObString &value);
  static bool is_date_value(const common::ObString &value);
  static bool is_datetime_value(const common::ObString &value);

private:
  bool saw_null_;
  bool saw_int_;
  bool saw_double_;
  bool saw_bool_;
  bool saw_date_;
  bool saw_datetime_;
  bool saw_text_;
  int64_t value_count_;
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_COLUMN_INFER_H_
