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

#ifndef OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_DEF_H_
#define OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_DEF_H_

#include "lib/string/ob_string.h"
#include "lib/ob_define.h"
#include "common/object/ob_object.h"

namespace oceanbase
{
namespace sql
{

/// The file formats understood by the read_* table functions.
enum class ObFileFormat : int64_t
{
  FILE_FORMAT_INVALID = 0,
  FILE_FORMAT_CSV = 1,
  FILE_FORMAT_JSON = 2,
  FILE_FORMAT_NDJSON = 3,
  FILE_FORMAT_PARQUET = 4,
};

/// Options carried from the table function arguments to the file scanner.
struct ObFileScanOptions
{
  ObFileScanOptions()
      : format_(ObFileFormat::FILE_FORMAT_INVALID),
        delimiter_(','),
        quote_('"'),
        escape_('\\'),
        has_header_(true),
        sample_size_(-1),
        skip_rows_(0),
        path_(),
        null_str_()
  {}
  ObFileFormat format_;
  char delimiter_;               // CSV only
  char quote_;                   // CSV only
  char escape_;                  // CSV only
  bool has_header_;              // CSV only
  int64_t sample_size_;          // -1 means "read the whole file"
  int64_t skip_rows_;
  common::ObString path_;        // absolute path, already checked against secure_file_priv
  common::ObString null_str_;    // CSV text that means NULL
};

/// One inferred output column of a file table.
struct ObFileColumnDef
{
  ObFileColumnDef()
      : type_(ObNullType),
        nullable_(true),
        index_(-1),
        name_(),
        type_name_(),
        type_name_len_(0)
  {}
  ObObjType type_;               // type used to build the output datum
  bool nullable_;
  int64_t index_;                // position inside the file row
  common::ObString name_;        // column name, owned by the caller's allocator
  const char *type_name_;        // type name printed by DESCRIBE, e.g. "BIGINT"
  int64_t type_name_len_;
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_DEF_H_
