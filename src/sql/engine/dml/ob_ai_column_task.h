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

#ifndef OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_TASK_H_
#define OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_TASK_H_

#include "lib/utility/ob_macro_utils.h"
#include "lib/container/ob_se_array.h"
#include "sql/engine/dml/ob_dml_ctx_define.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
class ObString;
}
namespace blocksstable
{
struct ObDatumRow;
}
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
class ObTableModifyOp;

// The generation tasks of the ai columns of one row: one row per ai column of the table, all
// of them carrying the request id of that row version. The task rows are written with the
// access service and the transaction descriptor of the user statement, so they are visible to
// that transaction before the commit and disappear again when it rolls back. Their rowkey is
// (table_id, column_id, rowkey_predicate), so every dml can address them from the row it is
// changing, without knowing the request id of an older row version.
class ObAiColumnTask
{
public:
  // Queues the tasks of the row version new_row belongs to, one per ai column of the table.
  // new_row holds the rowkey values of that row version in rowkey order.
  static int insert_tasks(ObTableModifyOp &dml_op,
                          const share::schema::ObTableSchema &user_table,
                          const common::ObIArray<const share::schema::ObColumnSchemaV2 *> &rowkey_columns,
                          const blocksstable::ObDatumRow &new_row,
                          const int64_t request_id);
  // Removes every task row of the row version old_row belongs to.
  static int delete_tasks(ObTableModifyOp &dml_op,
                          const share::schema::ObTableSchema &user_table,
                          const common::ObIArray<const share::schema::ObColumnSchemaV2 *> &rowkey_columns,
                          const blocksstable::ObDatumRow &old_row);
  // The whole insert path of a row: queues the tasks of the row version it writes.
  static int insert_row_tasks(ObTableModifyOp &dml_op,
                              const share::schema::ObTableSchema &user_table,
                              const ObInsCtDef &ins_ctdef);
  // The update and the delete path of a row: an update that invalidates an ai column replaces
  // the tasks of the row version, a delete removes them together with the row.
  static int update_row_tasks(ObTableModifyOp &dml_op,
                              const share::schema::ObTableSchema &user_table,
                              const ObUpdCtDef &upd_ctdef);
  static int delete_row_tasks(ObTableModifyOp &dml_op,
                              const share::schema::ObTableSchema &user_table,
                              const ObDelCtDef &del_ctdef);
  // The ai columns of a table in schema order, respectively its rowkey columns in rowkey order.
  static int get_ai_columns(const share::schema::ObTableSchema &user_table,
                            common::ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns);
  static int get_rowkey_columns(const share::schema::ObTableSchema &user_table,
                                common::ObIArray<const share::schema::ObColumnSchemaV2 *> &rowkey_columns);
  // Reads the rowkey of one dml row. Every dml row starts with the rowkey columns of its
  // table, in rowkey order, which is why the values are read by position.
  static int get_rowkey_row(ObTableModifyOp &dml_op,
                            const ExprFixedArray &row,
                            const common::ObIArray<const share::schema::ObColumnSchemaV2 *> &rowkey_columns,
                            blocksstable::ObDatumRow &rowkey_row);
  // Renders the rowkey of one row as the text the worker turns back into a scan range, e.g.
  // "`id` = 2": the name is backquoted, integers are written bare, any other value is single
  // quoted, and the columns of a composite rowkey are joined with " and ".
  static int build_rowkey_predicate(const common::ObIArray<const share::schema::ObColumnSchemaV2 *> &rowkey_columns,
                                    const blocksstable::ObDatumRow &rowkey_row,
                                    common::ObIAllocator &allocator,
                                    common::ObString &predicate);
  // A process wide counter: every row version of a row gets an id of its own, and the ai
  // columns of that row version share it, which is what the worker groups them by.
  static int64_t next_request_id();
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_DML_OB_AI_COLUMN_TASK_H_
