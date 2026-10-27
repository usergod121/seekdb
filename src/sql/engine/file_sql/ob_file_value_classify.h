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

#ifndef OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_VALUE_CLASSIFY_H_
#define OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_VALUE_CLASSIFY_H_

#include <cstdint>

namespace oceanbase
{
namespace sql
{

/// What a raw text field looks like.  Deliberately dependency free (no oblib,
/// no allocation) so it can be unit tested on its own and reused by every text
/// reader.
enum class ObFileValueKind : int64_t
{
  FILE_VALUE_TEXT = 0,
  FILE_VALUE_EMPTY = 1,
  FILE_VALUE_BOOL = 2,
  FILE_VALUE_INTEGER = 3,
  FILE_VALUE_DOUBLE = 4,
  FILE_VALUE_DATE = 5,
  FILE_VALUE_DATETIME = 6,
};

namespace file_value
{

/// Classify a raw field.  `len` bytes starting at `str`, no NUL terminator
/// required.  All predicates below are strict: padded text, sign-only values,
/// out of range dates and trailing garbage are all TEXT.
ObFileValueKind classify(const char *str, const int64_t len);

bool is_bool(const char *str, const int64_t len);
bool is_integer(const char *str, const int64_t len);   // fits in int64
bool is_double(const char *str, const int64_t len);    // needs '.' or exponent
bool is_date(const char *str, const int64_t len);      // YYYY-MM-DD
bool is_datetime(const char *str, const int64_t len);  // YYYY-MM-DD[ T]HH:MM:SS

}  // namespace file_value
}  // namespace sql
}  // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_VALUE_CLASSIFY_H_
