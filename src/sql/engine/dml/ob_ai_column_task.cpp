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

#define USING_LOG_PREFIX SQL_DML
#include "sql/engine/dml/ob_ai_column_task.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "lib/utility/utility.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/dml/ob_table_modify_op.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_sql_context.h"
#include "sql/session/ob_sql_session_info.h"
#include "storage/ob_table_dml_param.h"
#include "storage/ob_value_row_iterator.h"
#include "storage/tx/ob_trans_service.h"
#include "storage/tx_storage/ob_access_service.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;
using namespace oceanbase::storage;
using namespace oceanbase::transaction;

namespace oceanbase
{
namespace sql
{

namespace
{
// A dml row is filled in the order the storage layer writes rows: the rowkey columns first,
// then the remaining columns in column id order. The task table has its two timestamps at the
// lowest column ids, which is why they sit between the rowkey and the task state here.
const char *const TASK_COLUMN_NAMES[] = {
  "table_id", "column_id", "rowkey_predicate",
  "gmt_create", "gmt_modified",
  "request_id", "state", "retry_count", "eligible_ts"
};
// a delete of task rows only has to know their rowkey
const int64_t TASK_ROWKEY_COLUMN_COUNT = 3;
const int64_t TASK_COLUMN_COUNT = 9;
const int64_t TASK_TABLE_ID = 0;
const int64_t TASK_COLUMN_ID = 1;
const int64_t TASK_ROWKEY_PREDICATE = 2;
const int64_t TASK_GMT_CREATE = 3;
const int64_t TASK_GMT_MODIFIED = 4;
const int64_t TASK_REQUEST_ID = 5;
const int64_t TASK_STATE = 6;
const int64_t TASK_RETRY_COUNT = 7;
const int64_t TASK_ELIGIBLE_TS = 8;
// The task rows join the transaction of the user statement, so they get the same budget the
// sql layer allows its other inner writes of one statement.
const int64_t AI_COLUMN_TASK_TIMEOUT_US = 10 * 1000 * 1000;

// Everything one write of task rows needs: the transaction of the user statement, the task
// table and the dml param of the task columns the write touches.
struct TaskTableWrite
{
  TaskTableWrite()
    : tx_(NULL),
      access_service_(NULL),
      task_table_(NULL),
      affected_rows_(0)
  {}
  ObTxDesc *tx_;
  ObAccessService *access_service_;
  const ObTableSchema *task_table_;
  ObDMLBaseParam dml_param_;
  ObStoreCtxGuard store_ctx_guard_;
  ObSEArray<uint64_t, TASK_COLUMN_COUNT> column_ids_;
  int64_t affected_rows_;
};

int get_task_table_schema(ObTableModifyOp &dml_op, const ObTableSchema *&task_table)
{
  int ret = OB_SUCCESS;
  const ObSqlCtx *sql_ctx = NULL;
  ObSchemaGetterGuard *schema_guard = NULL;
  task_table = NULL;
  if (OB_ISNULL(sql_ctx = dml_op.get_exec_ctx().get_sql_ctx())
      || OB_ISNULL(schema_guard = sql_ctx->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the schema guard of the statement is not available", K(ret));
  } else if (OB_FAIL(schema_guard->get_table_schema(share::OB_ALL_AI_COLUMN_TASK_TID, task_table))) {
    LOG_WARN("fail to get the ai column task table schema", K(ret));
  } else if (OB_ISNULL(task_table)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the ai column task table is not found", K(ret));
  }
  return ret;
}

// Resolves everything a write of task rows needs and acquires the write context of the task
// tablet, so the write itself only has to build its rows and call the access service. The
// first column_count entries of TASK_COLUMN_NAMES are the columns the write touches, and they
// are the only ones its dml param knows about.
int prepare_task_table_write(ObTableModifyOp &dml_op,
                             ObIAllocator &allocator,
                             const int64_t column_count,
                             ObTableDMLParam &table_dml_param,
                             TaskTableWrite &write)
{
  int ret = OB_SUCCESS;
  const int64_t timeout_us = ObTimeUtility::current_time() + AI_COLUMN_TASK_TIMEOUT_US;
  ObSQLSessionInfo *session = NULL;
  ObTransService *trans_service = share::server_service<transaction::ObTransService>();
  if (OB_ISNULL(session = dml_op.get_exec_ctx().get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the session of the statement is not available", K(ret));
  } else if (OB_ISNULL(write.tx_ = session->get_tx_desc())) {
    // the tasks are part of the transaction of the user statement, there is no other one they
    // could be written into
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the statement has no transaction", K(ret));
  } else if (OB_ISNULL(write.access_service_ = share::server_service<storage::ObAccessService>())
             || OB_ISNULL(trans_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the storage services are not available", K(ret), KP(write.access_service_),
             KP(trans_service));
  } else if (OB_FAIL(get_task_table_schema(dml_op, write.task_table_))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
    const ObColumnSchemaV2 *column = NULL;
    if (OB_ISNULL(column = write.task_table_->get_column_schema(TASK_COLUMN_NAMES[i]))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the ai column task table misses a column", K(ret), K(TASK_COLUMN_NAMES[i]));
    } else if (OB_FAIL(write.column_ids_.push_back(column->get_column_id()))) {
      LOG_WARN("fail to push back the task column id", K(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(table_dml_param.convert(write.task_table_,
                                             write.task_table_->get_schema_version(),
                                             write.column_ids_))) {
    LOG_WARN("fail to build the dml param of the task table", K(ret));
  } else if (OB_FAIL(trans_service->get_read_snapshot(
               *write.tx_, ObTxIsolationLevel::RC, timeout_us, write.dml_param_.snapshot_))) {
    LOG_WARN("fail to get a snapshot of the transaction", K(ret));
  } else {
    write.dml_param_.table_param_ = &table_dml_param;
    write.dml_param_.branch_id_ = 0;
    write.dml_param_.schema_version_ = write.task_table_->get_schema_version();
    write.dml_param_.timeout_ = timeout_us;
    write.dml_param_.dml_allocator_ = &allocator;
    write.dml_param_.store_ctx_guard_ = &write.store_ctx_guard_;
    // A task row is put rather than inserted, and a removal writes the rowkey before deleting
    // it, so one operation may modify the same task row twice. That is exactly the case an
    // insert up write is allowed to do (ob_concurrent_control.cpp, the insert up case), and it
    // is the flag the lob writer sets for its own writes of one row
    // (ob_lob_persistent_adaptor.cpp).
    write.dml_param_.write_flag_.set_is_insert_up();
    if (OB_FAIL(write.access_service_->get_write_store_ctx_guard(
          timeout_us, *write.tx_, write.dml_param_.snapshot_, write.dml_param_.branch_id_,
          write.dml_param_.write_flag_, write.store_ctx_guard_))) {
      LOG_WARN("fail to get a write store context of the task table", K(ret));
    }
  }
  return ret;
}

// Queues one task row per ai column of the row version the caller describes. The rows are put
// instead of inserted: a row version of the same row may still have its task row, and
// replacing it with a fresh request is exactly what a new row version needs.
int write_task_rows(ObTableModifyOp &dml_op,
                    const uint64_t table_id,
                    const ObIArray<const ObColumnSchemaV2 *> &ai_columns,
                    const ObString &rowkey_predicate,
                    const int64_t request_id)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnTask", OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObTableDMLParam table_dml_param(allocator);
  TaskTableWrite write;
  ObValueRowIterator row_iter;
  ObDatumRow task_row;
  const int64_t now = ObTimeUtility::current_time();
  if (OB_FAIL(prepare_task_table_write(dml_op, allocator, TASK_COLUMN_COUNT, table_dml_param,
                                       write))) {
  } else if (OB_FAIL(row_iter.init())) {
  } else if (OB_FAIL(task_row.init(TASK_COLUMN_COUNT))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    const ObColumnSchemaV2 *ai_column = ai_columns.at(i);
    if (OB_ISNULL(ai_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the ai column schema is null", K(ret));
    } else {
      task_row.reuse();
      task_row.storage_datums_[TASK_TABLE_ID].set_int(static_cast<int64_t>(table_id));
      task_row.storage_datums_[TASK_COLUMN_ID].set_int(
          static_cast<int64_t>(ai_column->get_column_id()));
      task_row.storage_datums_[TASK_ROWKEY_PREDICATE].set_string(rowkey_predicate);
      task_row.storage_datums_[TASK_GMT_CREATE].set_timestamp(now);
      task_row.storage_datums_[TASK_GMT_MODIFIED].set_timestamp(now);
      task_row.storage_datums_[TASK_REQUEST_ID].set_int(request_id);
      task_row.storage_datums_[TASK_STATE].set_int(0);
      task_row.storage_datums_[TASK_RETRY_COUNT].set_int(0);
      task_row.storage_datums_[TASK_ELIGIBLE_TS].set_timestamp(now);
      if (OB_FAIL(row_iter.add_row(task_row))) {
        LOG_WARN("fail to add the task row", K(ret));
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(write.access_service_->put_rows(write.task_table_->get_tablet_id(),
                                                     *write.tx_, write.dml_param_,
                                                     write.column_ids_, &row_iter,
                                                     write.affected_rows_))) {
    LOG_WARN("fail to write the ai column task rows", K(ret), K(table_id), K(ai_columns.count()));
  }
  return ret;
}

// Removes the task rows of the given ai columns of one row version. The storage layer refuses
// to delete a row that is not there, and the task row of a row version may already be gone
// (the worker removes it once it wrote the value), so the rowkey is written first: what is put
// here is deleted right after.
int delete_task_rows(ObTableModifyOp &dml_op,
                     const uint64_t table_id,
                     const ObIArray<const ObColumnSchemaV2 *> &ai_columns,
                     const ObString &rowkey_predicate)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnTask", OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObTableDMLParam table_dml_param(allocator);
  TaskTableWrite write;
  ObValueRowIterator row_iter;
  ObDatumRow task_row;
  if (OB_FAIL(prepare_task_table_write(dml_op, allocator, TASK_ROWKEY_COLUMN_COUNT,
                                       table_dml_param, write))) {
  } else if (OB_FAIL(row_iter.init())) {
  } else if (OB_FAIL(task_row.init(TASK_ROWKEY_COLUMN_COUNT))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    const ObColumnSchemaV2 *ai_column = ai_columns.at(i);
    if (OB_ISNULL(ai_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the ai column schema is null", K(ret));
    } else {
      task_row.reuse();
      task_row.storage_datums_[TASK_TABLE_ID].set_int(static_cast<int64_t>(table_id));
      task_row.storage_datums_[TASK_COLUMN_ID].set_int(
          static_cast<int64_t>(ai_column->get_column_id()));
      task_row.storage_datums_[TASK_ROWKEY_PREDICATE].set_string(rowkey_predicate);
      if (OB_FAIL(row_iter.add_row(task_row))) {
        LOG_WARN("fail to add the task rowkey", K(ret));
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(write.access_service_->put_rows(write.task_table_->get_tablet_id(),
                                                     *write.tx_, write.dml_param_,
                                                     write.column_ids_, &row_iter,
                                                     write.affected_rows_))) {
  } else if (FALSE_IT(row_iter.rescan())) {
  } else if (OB_FAIL(write.access_service_->delete_rows(write.task_table_->get_tablet_id(),
                                                        *write.tx_, write.dml_param_,
                                                        write.column_ids_, &row_iter,
                                                        write.affected_rows_))) {
    LOG_WARN("fail to delete the ai column task rows", K(ret), K(table_id), K(ai_columns.count()));
  }
  return ret;
}

// Appends one "`name` = value" item of the rowkey predicate. Integers are written bare, every
// other value is written as a single quoted string, the way the worker parses it back.
int append_predicate_item(const ObColumnSchemaV2 &column, const ObDatum &datum, ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  const ObString &name = column.get_column_name_str();
  if (OB_UNLIKELY(datum.is_null())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("a rowkey column of the row is null", K(ret), K(name));
  } else if (OB_FAIL(sql.append_fmt("%s`%.*s` = ", 0 == sql.length() ? "" : " and ",
                                    name.length(), name.ptr()))) {
    LOG_WARN("fail to append the rowkey column", K(ret), K(name));
  } else if (ob_is_integer_type(column.get_data_type())) {
    if (OB_FAIL(sql.append_fmt("%ld", datum.get_int()))) {
      LOG_WARN("fail to append the rowkey value", K(ret));
    }
  } else if (OB_FAIL(sql.append_fmt("'%.*s'", datum.get_string().length(),
                                    datum.get_string().ptr()))) {
    LOG_WARN("fail to append the rowkey value", K(ret));
  }
  return ret;
}

// The ai columns an update invalidates. The value of an ai column is stale when the statement
// writes one of the columns its expression reads, or when it moves the row to a new rowkey (the
// value belongs to the row version, not to the row), or when it drops the value of the ai column
// itself (that is how ALTER TABLE ... RETRY AI COLUMN asks for a new one). Writing a value into
// the ai column does not invalidate it: the worker writes the value it generated with exactly
// such an update, and it would otherwise invalidate the value it just wrote.
int get_invalidated_columns(ObTableModifyOp &dml_op,
                            const ObTableSchema &user_table,
                            const ObUpdCtDef &upd_ctdef,
                            const ObIArray<const ObColumnSchemaV2 *> &ai_columns,
                            const ObIArray<uint64_t> &assigned_column_ids,
                            ObIArray<const ObColumnSchemaV2 *> &invalidated_columns)
{
  int ret = OB_SUCCESS;
  bool rowkey_assigned = false;
  for (int64_t i = 0; OB_SUCC(ret) && !rowkey_assigned && i < assigned_column_ids.count(); ++i) {
    if (OB_FAIL(user_table.get_rowkey_info().is_rowkey_column(assigned_column_ids.at(i),
                                                             rowkey_assigned))) {
      LOG_WARN("fail to check whether the assigned column is a rowkey column", K(ret));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    const ObColumnSchemaV2 *ai_column = ai_columns.at(i);
    share::ObAiColumnSpec spec;
    bool invalidated = rowkey_assigned;
    if (OB_ISNULL(ai_column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the ai column schema is null", K(ret));
    } else if (OB_FAIL(ObAiColumnSpec::load_from_ai_column(user_table, *ai_column, spec))) {
      LOG_WARN("fail to read the definition of the ai column", K(ret),
               K(ai_column->get_column_name_str()));
    }
    for (int64_t j = 0; OB_SUCC(ret) && !invalidated && j < assigned_column_ids.count(); ++j) {
      const ObColumnSchemaV2 *assigned_column = NULL;
      if (OB_ISNULL(assigned_column =
                    user_table.get_column_schema(assigned_column_ids.at(j)))) {
        // a hidden column of the row (a row version id, a state column of the ai column itself),
        // it is never read by the expression of an ai column
      } else {
        invalidated = spec.is_referenced_column(assigned_column->get_column_name_str());
      }
    }
    // the statement drops the value of the ai column: null is the only value an update ever
    // writes into an ai column, every other value is the one the worker generated
    for (int64_t j = 0; OB_SUCC(ret) && !invalidated && j < upd_ctdef.column_ids_.count(); ++j) {
      ObDatum *datum = NULL;
      if (upd_ctdef.column_ids_.at(j) != ai_column->get_column_id()) {
        // not the value of this ai column
      } else if (j >= upd_ctdef.new_row_.count() || OB_ISNULL(upd_ctdef.new_row_.at(j))) {
        // the dml row does not carry the value, there is nothing to judge
      } else if (OB_FAIL(upd_ctdef.new_row_.at(j)->eval(dml_op.get_eval_ctx(), datum))) {
        LOG_WARN("fail to evaluate the value assigned to an ai column", K(ret),
                 K(ai_column->get_column_name_str()));
      } else {
        invalidated = (NULL != datum && datum->is_null());
      }
    }
    if (OB_SUCC(ret) && invalidated
        && OB_FAIL(invalidated_columns.push_back(ai_column))) {
      LOG_WARN("fail to push back the invalidated column", K(ret));
    }
  }
  return ret;
}
} // anonymous namespace

int64_t ObAiColumnTask::next_request_id()
{
  // every row version of one row gets an id of its own, the ai columns of that row version
  // share it, and the worker groups the tasks of one row version by it
  static int64_t request_id = 0;
  return ATOMIC_FAA(&request_id, 1) + 1;
}

int ObAiColumnTask::get_ai_columns(const ObTableSchema &user_table,
                                   ObIArray<const ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  for (ObTableSchema::const_column_iterator iter = user_table.column_begin();
       OB_SUCC(ret) && iter != user_table.column_end(); ++iter) {
    const ObColumnSchemaV2 *column = *iter;
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column schema is null", K(ret));
    } else if (!column->is_ai_column()) {
      // an ordinary column, it has no value to generate
    } else if (OB_FAIL(ai_columns.push_back(column))) {
      LOG_WARN("fail to push back the ai column", K(ret));
    }
  }
  return ret;
}

int ObAiColumnTask::get_rowkey_columns(const ObTableSchema &user_table,
                                       ObIArray<const ObColumnSchemaV2 *> &rowkey_columns)
{
  int ret = OB_SUCCESS;
  ObSEArray<uint64_t, 4> column_ids;
  if (OB_FAIL(user_table.get_rowkey_info().get_column_ids(column_ids))) {
    LOG_WARN("fail to get the rowkey column ids", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
    const ObColumnSchemaV2 *column = NULL;
    if (OB_ISNULL(column = user_table.get_column_schema(column_ids.at(i)))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the rowkey column schema is not found", K(ret), K(column_ids.at(i)));
    } else if (OB_FAIL(rowkey_columns.push_back(column))) {
      LOG_WARN("fail to push back the rowkey column", K(ret));
    }
  }
  return ret;
}

int ObAiColumnTask::get_rowkey_row(ObTableModifyOp &dml_op,
                                   const ExprFixedArray &row,
                                   const ObIArray<const ObColumnSchemaV2 *> &rowkey_columns,
                                   ObDatumRow &rowkey_row)
{
  int ret = OB_SUCCESS;
  ObEvalCtx &eval_ctx = dml_op.get_eval_ctx();
  if (OB_UNLIKELY(rowkey_columns.empty())) {
    // without a rowkey there is no way to describe the row to the worker
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the table has no rowkey column", K(ret));
  } else if (OB_UNLIKELY(row.count() < rowkey_columns.count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the dml row does not carry the rowkey columns", K(ret), K(row.count()),
             K(rowkey_columns.count()));
  } else if (OB_FAIL(rowkey_row.init(rowkey_columns.count()))) {
    LOG_WARN("fail to init the rowkey row", K(ret), K(rowkey_columns.count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_columns.count(); ++i) {
    ObDatum *datum = NULL;
    if (OB_ISNULL(row.at(i))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the rowkey expression of the dml row is null", K(ret), K(i));
    } else if (OB_FAIL(row.at(i)->eval(eval_ctx, datum))) {
      LOG_WARN("fail to evaluate the rowkey column", K(ret), K(i));
    } else if (OB_ISNULL(datum)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the rowkey value is null", K(ret), K(i));
    } else {
      rowkey_row.storage_datums_[i].shallow_copy_from_datum(*datum);
    }
  }
  return ret;
}

int ObAiColumnTask::build_rowkey_predicate(
    const ObIArray<const ObColumnSchemaV2 *> &rowkey_columns,
    const ObDatumRow &rowkey_row,
    ObIAllocator &allocator,
    ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  char *buf = NULL;
  predicate.reset();
  if (OB_UNLIKELY(rowkey_columns.count() != rowkey_row.get_column_count())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the rowkey values do not match the rowkey columns", K(ret),
             K(rowkey_columns.count()), K(rowkey_row.get_column_count()));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_columns.count(); ++i) {
    const ObColumnSchemaV2 *column = rowkey_columns.at(i);
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the rowkey column schema is null", K(ret));
    } else if (OB_FAIL(append_predicate_item(*column, rowkey_row.storage_datums_[i], sql))) {
      LOG_WARN("fail to append the rowkey item", K(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(sql.length())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("fail to allocate the rowkey predicate", K(ret), K(sql.length()));
  } else {
    MEMCPY(buf, sql.ptr(), sql.length());
    predicate.assign_ptr(buf, static_cast<int32_t>(sql.length()));
  }
  return ret;
}

int ObAiColumnTask::insert_tasks(ObTableModifyOp &dml_op,
                                 const ObTableSchema &user_table,
                                 const ObIArray<const ObColumnSchemaV2 *> &rowkey_columns,
                                 const ObDatumRow &new_row,
                                 const int64_t request_id)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnTask", OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<const ObColumnSchemaV2 *, 4> ai_columns;
  ObString rowkey_predicate;
  if (OB_FAIL(get_ai_columns(user_table, ai_columns))) {
  } else if (ai_columns.empty()) {
    // the table has no ai column, so this row version has nothing to generate
  } else if (OB_FAIL(build_rowkey_predicate(rowkey_columns, new_row, allocator,
                                            rowkey_predicate))) {
    LOG_WARN("fail to build the rowkey predicate of the row", K(ret));
  } else if (OB_FAIL(write_task_rows(dml_op, user_table.get_table_id(), ai_columns,
                                      rowkey_predicate, request_id))) {
  }
  return ret;
}

int ObAiColumnTask::delete_tasks(ObTableModifyOp &dml_op,
                                 const ObTableSchema &user_table,
                                 const ObIArray<const ObColumnSchemaV2 *> &rowkey_columns,
                                 const ObDatumRow &old_row)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnTask", OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<const ObColumnSchemaV2 *, 4> ai_columns;
  ObString rowkey_predicate;
  if (OB_FAIL(get_ai_columns(user_table, ai_columns))) {
  } else if (ai_columns.empty()) {
    // a table without an ai column never had a task row
  } else if (OB_FAIL(build_rowkey_predicate(rowkey_columns, old_row, allocator,
                                            rowkey_predicate))) {
    LOG_WARN("fail to build the rowkey predicate of the row", K(ret));
  } else if (OB_FAIL(delete_task_rows(dml_op, user_table.get_table_id(), ai_columns,
                                      rowkey_predicate))) {
  }
  return ret;
}

int ObAiColumnTask::insert_row_tasks(ObTableModifyOp &dml_op,
                                     const ObTableSchema &user_table,
                                     const ObInsCtDef &ins_ctdef)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 4> rowkey_columns;
  ObDatumRow rowkey_row;
  if (OB_FAIL(get_rowkey_columns(user_table, rowkey_columns))) {
  } else if (OB_FAIL(get_rowkey_row(dml_op, ins_ctdef.new_row_, rowkey_columns, rowkey_row))) {
    LOG_WARN("fail to read the rowkey of the inserted row", K(ret));
  } else if (OB_FAIL(insert_tasks(dml_op, user_table, rowkey_columns, rowkey_row,
                                  next_request_id()))) {
  }
  return ret;
}

int ObAiColumnTask::update_row_tasks(ObTableModifyOp &dml_op,
                                     const ObTableSchema &user_table,
                                     const ObUpdCtDef &upd_ctdef)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnTask", OB_MALLOC_NORMAL_BLOCK_SIZE);
  ObSEArray<const ObColumnSchemaV2 *, 4> ai_columns;
  ObSEArray<const ObColumnSchemaV2 *, 4> invalidated_columns;
  ObSEArray<const ObColumnSchemaV2 *, 4> rowkey_columns;
  ObDatumRow old_rowkey_row;
  ObDatumRow new_rowkey_row;
  ObString old_predicate;
  ObString new_predicate;
  if (OB_FAIL(get_ai_columns(user_table, ai_columns))) {
  } else if (ai_columns.empty()) {
    // nothing to invalidate
  } else if (OB_FAIL(get_invalidated_columns(dml_op,
                                             user_table,
                                             upd_ctdef,
                                             ai_columns,
                                             upd_ctdef.dupd_ctdef_.updated_column_ids_,
                                             invalidated_columns))) {
  } else if (invalidated_columns.empty()) {
    // an update that writes no column the ai column reads leaves its value and its tasks alone
  } else if (OB_FAIL(get_rowkey_columns(user_table, rowkey_columns))) {
  } else if (OB_FAIL(get_rowkey_row(dml_op, upd_ctdef.old_row_, rowkey_columns,
                                    old_rowkey_row))) {
  } else if (OB_FAIL(get_rowkey_row(dml_op, upd_ctdef.new_row_, rowkey_columns,
                                    new_rowkey_row))) {
  } else if (OB_FAIL(build_rowkey_predicate(rowkey_columns, old_rowkey_row, allocator,
                                            old_predicate))) {
  } else if (OB_FAIL(build_rowkey_predicate(rowkey_columns, new_rowkey_row, allocator,
                                            new_predicate))) {
  } else {
    // The task rows are addressed by the rowkey of the row version they belong to. An update
    // that keeps the rowkey only replaces the tasks of the invalidated ai columns, which the
    // put of a fresh request already does. An update that moves the rowkey leaves the tasks of
    // the old row version behind, they belong to a row that no longer exists.
    if (0 != old_predicate.compare(new_predicate)
        && OB_FAIL(delete_task_rows(dml_op, user_table.get_table_id(), ai_columns,
                                    old_predicate))) {
      LOG_WARN("fail to drop the tasks of the old row version", K(ret));
    } else if (OB_FAIL(write_task_rows(dml_op, user_table.get_table_id(), invalidated_columns,
                                       new_predicate, next_request_id()))) {
    }
  }
  return ret;
}

int ObAiColumnTask::delete_row_tasks(ObTableModifyOp &dml_op,
                                     const ObTableSchema &user_table,
                                     const ObDelCtDef &del_ctdef)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 4> rowkey_columns;
  ObDatumRow rowkey_row;
  if (OB_FAIL(get_rowkey_columns(user_table, rowkey_columns))) {
  } else if (OB_FAIL(get_rowkey_row(dml_op, del_ctdef.old_row_, rowkey_columns, rowkey_row))) {
    LOG_WARN("fail to read the rowkey of the deleted row", K(ret));
  } else if (OB_FAIL(delete_tasks(dml_op, user_table, rowkey_columns, rowkey_row))) {
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
