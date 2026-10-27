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

#include "sql/engine/file_sql/ob_file_value_classify.h"

namespace oceanbase
{
namespace sql
{
namespace file_value
{

namespace
{

inline bool is_digit(const char c) { return c >= '0' && c <= '9'; }

bool equal_ignore_case(const char *str, const int64_t len, const char *word)
{
  bool ret = true;
  for (int64_t i = 0; ret && i < len; ++i) {
    char a = str[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    ret = (a == word[i]);
  }
  ret = ret && '\0' == word[len];
  return ret;
}

}  // namespace

bool is_bool(const char *str, const int64_t len)
{
  // true/false, case insensitive
  return (4 == len && equal_ignore_case(str, len, "true"))
      || (5 == len && equal_ignore_case(str, len, "false"));
}

bool is_integer(const char *str, const int64_t len)
{
  bool ret = len > 0 && nullptr != str;
  int64_t idx = 0;
  if (ret && ('+' == str[idx] || '-' == str[idx])) {
    ++idx;
  }
  // Reject the sign-only case and anything past 19 significant digits, which
  // cannot fit into an int64.
  ret = ret && idx < len && len - idx <= 19;
  for (; ret && idx < len; ++idx) {
    ret = is_digit(str[idx]);
  }
  return ret;
}

bool is_double(const char *str, const int64_t len)
{
  bool ret = len > 0 && nullptr != str;
  int64_t digits = 0;
  int64_t idx = 0;
  bool has_dot = false;
  bool has_exp = false;
  if (ret && ('+' == str[idx] || '-' == str[idx])) {
    ++idx;
  }
  for (; ret && idx < len; ++idx) {
    const char c = str[idx];
    if (is_digit(c)) {
      ++digits;
    } else if ('.' == c && !has_dot && !has_exp) {
      has_dot = true;
    } else if (('e' == c || 'E' == c) && !has_exp && digits > 0) {
      has_exp = true;
      digits = 0;  // the exponent needs at least one digit after it
      if (idx + 1 < len && ('+' == str[idx + 1] || '-' == str[idx + 1])) {
        ++idx;
      }
    } else {
      ret = false;
    }
  }
  // "12" is an integer, not a double: a double needs a dot or an exponent.
  ret = ret && digits > 0 && (has_dot || has_exp);
  return ret;
}

bool is_date(const char *str, const int64_t len)
{
  bool ret = 10 == len && nullptr != str;
  for (int64_t i = 0; ret && i < 10; ++i) {
    ret = (4 == i || 7 == i) ? ('-' == str[i]) : is_digit(str[i]);
  }
  if (ret) {
    const int month = (str[5] - '0') * 10 + (str[6] - '0');
    const int day = (str[8] - '0') * 10 + (str[9] - '0');
    ret = month >= 1 && month <= 12 && day >= 1 && day <= 31;
  }
  return ret;
}

bool is_datetime(const char *str, const int64_t len)
{
  // YYYY-MM-DD HH:MM:SS or YYYY-MM-DDTHH:MM:SS
  bool ret = 19 == len && nullptr != str && is_date(str, 10);
  if (ret) {
    ret = (' ' == str[10] || 'T' == str[10] || 't' == str[10]);
  }
  for (int64_t i = 11; ret && i < 19; ++i) {
    ret = (13 == i || 16 == i) ? (':' == str[i]) : is_digit(str[i]);
  }
  if (ret) {
    const int hour = (str[11] - '0') * 10 + (str[12] - '0');
    const int minute = (str[14] - '0') * 10 + (str[15] - '0');
    const int second = (str[17] - '0') * 10 + (str[18] - '0');
    ret = hour <= 23 && minute <= 59 && second <= 59;
  }
  return ret;
}

ObFileValueKind classify(const char *str, const int64_t len)
{
  ObFileValueKind kind = ObFileValueKind::FILE_VALUE_TEXT;
  if (0 == len || nullptr == str) {
    kind = ObFileValueKind::FILE_VALUE_EMPTY;
  } else if (is_bool(str, len)) {
    kind = ObFileValueKind::FILE_VALUE_BOOL;
  } else if (is_integer(str, len)) {
    kind = ObFileValueKind::FILE_VALUE_INTEGER;
  } else if (is_double(str, len)) {
    kind = ObFileValueKind::FILE_VALUE_DOUBLE;
  } else if (is_datetime(str, len)) {
    kind = ObFileValueKind::FILE_VALUE_DATETIME;
  } else if (is_date(str, len)) {
    kind = ObFileValueKind::FILE_VALUE_DATE;
  }
  return kind;
}

}  // namespace file_value
} // namespace sql
} // namespace oceanbase
