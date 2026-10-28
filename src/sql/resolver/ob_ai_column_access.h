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

#ifndef OCEANBASE_SQL_OB_AI_COLUMN_ACCESS_H_
#define OCEANBASE_SQL_OB_AI_COLUMN_ACCESS_H_

#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace share
{
namespace schema
{
class ObColumnSchemaV2;
class ObTableSchema;
}
}
namespace sql
{
class ObSQLSessionInfo;

// The generated values of the ai columns of a table and the generation state of those values
// live next to each other: the value is the column the user declared, the state is the hidden
// state column of that ai column, and the generation tasks are rows of
// oceanbase.__all_ai_column_task that the worker of the ai columns consumes.
//
// The state is a state of the column of the table, not of a row: a statement reads the column
// as a whole, so it is complete only when every row of the table has its value, which is the
// condition `ai_column_strict_read` turns into an error.
class ObAiColumnAccess
{
public:
  // Fails the statement when it reads an ai column whose generation is not finished for every
  // row of the table. The read of a column is column wide: the statement fails even when none
  // of the rows it selects is the unfinished one.
  static int check_readable(const ObSQLSessionInfo &session,
                            const share::schema::ObTableSchema &table_schema,
                            const share::schema::ObColumnSchemaV2 &column);
  // ALTER TABLE <table> RETRY AI COLUMN <column> [WHERE <predicate>]: asks for a new
  // generation of one ai column for the rows the predicate selects. The value of the column is
  // dropped, the state of the column of those rows goes back to pending, and one task row of
  // the column replaces every task row those rows had. The tasks are what the worker consumes,
  // no model service is asked while the statement runs.
  static int retry(const uint64_t table_id,
                   const common::ObString &column_name,
                   const common::ObString &predicate,
                   int64_t &retried_rows);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_AI_COLUMN_ACCESS_H_
