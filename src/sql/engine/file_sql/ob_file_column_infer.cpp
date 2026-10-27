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

#include "sql/engine/file_sql/ob_file_column_infer.h"
#include "sql/engine/file_sql/ob_file_value_classify.h"

namespace oceanbase
{
namespace sql
{

bool ObFileColumnInfer::is_integer_value(const common::ObString &value)
{
  return file_value::is_integer(value.ptr(), value.length());
}

bool ObFileColumnInfer::is_double_value(const common::ObString &value)
{
  return file_value::is_double(value.ptr(), value.length());
}

bool ObFileColumnInfer::is_date_value(const common::ObString &value)
{
  return file_value::is_date(value.ptr(), value.length());
}

bool ObFileColumnInfer::is_datetime_value(const common::ObString &value)
{
  return file_value::is_datetime(value.ptr(), value.length());
}

bool ObFileColumnInfer::is_bool_value(const common::ObString &value)
{
  return file_value::is_bool(value.ptr(), value.length());
}

void ObFileColumnInfer::observe(const common::ObString &value, bool empty_is_null)
{
  ++value_count_;
  switch (file_value::classify(value.ptr(), value.length())) {
    case ObFileValueKind::FILE_VALUE_EMPTY:
      if (empty_is_null) {
        saw_null_ = true;
      } else {
        saw_text_ = true;
      }
      break;
    case ObFileValueKind::FILE_VALUE_BOOL:
      saw_bool_ = true;
      break;
    case ObFileValueKind::FILE_VALUE_INTEGER:
      saw_int_ = true;
      break;
    case ObFileValueKind::FILE_VALUE_DOUBLE:
      saw_double_ = true;
      break;
    case ObFileValueKind::FILE_VALUE_DATE:
      saw_date_ = true;
      break;
    case ObFileValueKind::FILE_VALUE_DATETIME:
      saw_datetime_ = true;
      break;
    default:
      saw_text_ = true;
      break;
  }
}

void ObFileColumnInfer::resolve(ObObjType &type, const char *&type_name, int64_t &type_name_len,
                                bool &nullable) const
{
  nullable = saw_null_ || 0 == value_count_;
  if (saw_text_) {
    type = ObVarcharType;
    type_name = "VARCHAR";
    type_name_len = 7;
  } else if (saw_double_) {
    type = ObDoubleType;
    type_name = "DOUBLE";
    type_name_len = 6;
  } else if (saw_int_) {
    type = ObIntType;
    type_name = "BIGINT";
    type_name_len = 6;
  } else if (saw_bool_) {
    // MySQL has no dedicated boolean type: BOOLEAN is a tinyint, but DESCRIBE
    // must still print BOOLEAN for a column inferred from true/false values.
    type = ObTinyIntType;
    type_name = "BOOLEAN";
    type_name_len = 7;
  } else if (saw_datetime_) {
    type = ObDateTimeType;
    type_name = "DATETIME";
    type_name_len = 8;
  } else if (saw_date_) {
    type = ObDateType;
    type_name = "DATE";
    type_name_len = 4;
  } else {
    // all NULL, or no value at all: keep it a nullable string
    type = ObVarcharType;
    type_name = "VARCHAR";
    type_name_len = 7;
  }
}

void ObFileColumnInfer::reset()
{
  saw_null_ = false;
  saw_int_ = false;
  saw_double_ = false;
  saw_bool_ = false;
  saw_date_ = false;
  saw_datetime_ = false;
  saw_text_ = false;
  value_count_ = 0;
}

} // namespace sql
} // namespace oceanbase
