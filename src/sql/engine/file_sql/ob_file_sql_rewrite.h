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

#ifndef OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_REWRITE_H_
#define OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_REWRITE_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"

namespace oceanbase
{
namespace sql
{

class ObSQLSessionInfo;

/// Turns the "file as a table" constructs into plain SQL the existing engine
/// already understands:
///
///   read_csv('f.csv'[, opt = val ...])   -> a derived table of typed literals
///   read_json/read_ndjson('f.jsonl')     -> a derived table of typed literals
///   read_parquet('f.parquet')            -> a derived table of typed literals
///   glob('dir/*')                        -> a derived table with one `file` column
///   describe select .. from read_csv(..) -> the inferred column metadata
///   copy (query) to 'f' (format csv ..)  -> query ... into outfile 'f'
///
/// The rewrite happens on the statement text, before it is parsed, so it does
/// not disturb the parser, the resolver, the optimizer or the code generator,
/// and every downstream capability (aggregates, sort, join, describe, export)
/// keeps working unchanged.
///
/// The file itself is read once per statement on the query thread; the session
/// variable enable_file_sql gates the whole feature (off by default).
class ObFileSqlRewriter
{
public:
  /// Cheap test of whether the statement mentions a file table function.
  static bool need_rewrite(const common::ObString &sql);

  /// Rewrite `sql` into `rewritten`.  `alloc` must outlive the statement.
  /// Returns OB_NOT_SUPPORTED (1235) when the feature is disabled and
  /// OB_ERR_PARSE_SQL (1064) when a file table argument is not valid.
  static int rewrite(ObSQLSessionInfo &session,
                     common::ObIAllocator &alloc,
                     const common::ObString &sql,
                     common::ObString &rewritten);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_FILE_SQL_OB_FILE_SQL_REWRITE_H_
