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

#define USING_LOG_PREFIX SERVER

#include <stdlib.h>
#include <string.h>
#include "observer/ai_column/ob_ai_column_worker.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/profile/ob_trace_id.h"
#include "lib/string/ob_sql_string.h"
#include "query/api/query/ai/ob_ai_endpoint_resolver.h"
#include "share/ob_server_struct.h"
#include "share/rc/ob_server_runtime.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "sql/engine/expr/ob_expr_ai/ob_ai_func_utils.h"

namespace oceanbase
{
namespace observer
{

using namespace common;
using namespace share;
using namespace share::schema;

// Out of class definitions of the tuning constants of the worker. The timer and the thread pool
// take their argument by reference, which odr uses a static const member and needs a definition
// of it somewhere, otherwise the link fails on the symbol.
const int64_t ObAiColumnWorker::SCAN_INTERVAL_US;
const int64_t ObAiColumnWorker::SCAN_BATCH_SIZE;
const int64_t ObAiColumnWorker::MAX_QUEUED_GROUPS;
const int64_t ObAiColumnWorker::MIN_THREAD_COUNT;
const int64_t ObAiColumnWorker::MAX_THREAD_COUNT;
const int64_t ObAiColumnWorker::RETRY_BACKOFF_BASE_SEC;
const int64_t ObAiColumnWorker::RETRY_BACKOFF_MAX_SEC;

namespace
{

// The tasks of all ai columns of all tables, see the task table contract: one row per ai
// column of one row version, and every ai column of the same row version shares one
// request id.
const char *const AI_COLUMN_TASK_TABLE = "oceanbase.__all_ai_column_task";
const char *const AI_COMPLETE_FUNC_NAME = "ai_complete";
const char *const AI_EMBED_FUNC_NAME = "ai_embed";

// TEMPORARY (diagnosis of the session corruption, delete before commit): the mode selects how
// much of the worker runs, so that one build can be bisected from the outside without rebuilding.
//   "off"  the module is constructed, inited and started, but the scan timer is never scheduled
//   "scan" the scan reads the task table and stops there, nothing is queued into the pool
//   unset  the worker runs fully
const char *const AI_COLUMN_WORKER_MODE_ENV = "SEEKDB_AI_COLUMN_WORKER_MODE";

const char *ai_column_worker_mode()
{
  const char *mode = getenv(AI_COLUMN_WORKER_MODE_ENV);
  return nullptr == mode ? "" : mode;
}

bool worker_mode_is(const char *mode)
{
  return 0 == strcmp(ai_column_worker_mode(), mode);
}

// Task row state of a task whose last generation failed and that waits for its next try. A
// task that is waiting for its first generation stays 0, the dml side writes it that way.
const int64_t AI_COLUMN_TASK_STATE_FAILED = 3;
// The hidden state column of an ai column is 1 once its value is generated.
const int64_t AI_COLUMN_STATE_READY = 1;

// An eligible task row as the scan read it. The strings point into the arena of the scan.
struct ScannedTaskRow
{
  ScannedTaskRow()
    : table_id_(OB_INVALID_ID), column_id_(OB_INVALID_ID), request_id_(0), retry_count_(0),
      rowkey_predicate_()
  {}
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t request_id_;
  int64_t retry_count_;
  ObString rowkey_predicate_;
};

bool is_blank(const char c)
{
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c;
}

bool is_identifier_char(const char c)
{
  return ('_' == c) || ('$' == c) || (c >= '0' && c <= '9')
      || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Append an identifier the way the text of a statement quotes one.
int append_sql_identifier(ObSqlString &sql, const ObString &name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("`", 1))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < name.length(); ++i) {
      const char c = name.ptr()[i];
      if ('`' == c && OB_FAIL(sql.append("``", 2))) {
      } else if ('`' != c && OB_FAIL(sql.append(&c, 1))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append("`", 1))) {
    }
  }
  return ret;
}

// Append a qualified table name.
int append_sql_table(ObSqlString &sql, const ObString &database, const ObString &table)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_sql_identifier(sql, database))) {
  } else if (OB_FAIL(sql.append(".", 1))) {
  } else if (OB_FAIL(append_sql_identifier(sql, table))) {
  }
  return ret;
}

// Append a value as a single quoted sql literal. The value is what a model service answered, so
// it is arbitrary text: a quote or a back slash is escaped, and a control character that can
// appear in an answer keeps its meaning through the escape sequence of the dialect, so that the
// value of the column is exactly the answer of the model and never breaks the statement.
int append_sql_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'", 1))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      const char c = value.ptr()[i];
      const unsigned char uc = static_cast<unsigned char>(c);
      if ('\'' == c || '\\' == c) {
        // a doubled quote, respectively a doubled back slash, is the character itself
        if (OB_FAIL(sql.append(&c, 1))) {
        } else if (OB_FAIL(sql.append(&c, 1))) {
        }
      } else if ('\n' == c) {
        ret = sql.append("\\n", 2);
      } else if ('\r' == c) {
        ret = sql.append("\\r", 2);
      } else if ('\t' == c) {
        ret = sql.append("\\t", 2);
      } else if ('\0' == c) {
        ret = sql.append("\\0", 2);
      } else if (uc < 0x20) {
        // the remaining control characters have no escape in the dialect, a blank keeps the
        // statement valid
        ret = sql.append(" ", 1);
      } else {
        ret = sql.append(&c, 1);
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append("'", 1))) {
    }
  }
  return ret;
}

// Copy a string out of a scanned sql result, the buffer of the result is released long before
// the value of the ai column is written.
int copy_string(ObIAllocator &allocator, const ObString &src, ObString &dst)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  if (src.empty()) {
    dst.reset();
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(src.length())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, src.ptr(), src.length());
    dst.assign_ptr(buf, src.length());
  }
  return ret;
}

// Split the stored expression of an ai column into the ai model it is asked with and the
// expression that is evaluated against the row. An ai column is declared as
//   AI_COMPLETE('<ai model>', <expression of the value>)
// or
//   AI_EMBED('<ai model>', <content>, <dimension>)
// so the expression is one call of an ai function whose first argument is the model name and
// whose second argument is the value the model is asked with. The arguments are split on the
// commas that are neither nested in a call nor inside a string.
int parse_ai_column_expr(ObIAllocator &allocator,
                         const ObString &expr,
                         bool &is_embedding,
                         ObString &model_name,
                         ObString &content_expr)
{
  int ret = OB_SUCCESS;
  const ObString text = expr.trim();
  int64_t pos = 0;
  int64_t args_begin = -1;
  int64_t args_end = -1;
  ObString func_name;
  while (pos < text.length() && is_identifier_char(text.ptr()[pos])) {
    ++pos;
  }
  if (0 == pos) {
    ret = OB_NOT_SUPPORTED;
  } else {
    func_name.assign_ptr(text.ptr(), static_cast<int32_t>(pos));
  }
  while (OB_SUCC(ret) && pos < text.length() && is_blank(text.ptr()[pos])) {
    ++pos;
  }
  // the call has to be the whole expression, a value derived from a call would not be the
  // answer of the model service for the row
  if (OB_FAIL(ret)) {
  } else if (pos >= text.length() || '(' != text.ptr()[pos]) {
    ret = OB_NOT_SUPPORTED;
  } else {
    int64_t depth = 0;
    char quote = '\0';
    for (int64_t i = pos; OB_SUCC(ret) && i < text.length(); ++i) {
      const char c = text.ptr()[i];
      if ('\0' != quote) {
        if ('\\' == c) {
          ++i;
        } else if (quote == c) {
          quote = '\0';
        }
      } else if ('\'' == c || '"' == c || '`' == c) {
        quote = c;
      } else if ('(' == c) {
        if (0 == depth) {
          args_begin = i + 1;
        }
        ++depth;
      } else if (')' == c) {
        --depth;
        if (0 == depth) {
          args_end = i;
          break;
        }
      }
    }
    if (args_begin < 0 || args_end < args_begin) {
      ret = OB_NOT_SUPPORTED;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (0 != func_name.case_compare(AI_COMPLETE_FUNC_NAME)
             && 0 != func_name.case_compare(AI_EMBED_FUNC_NAME)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    int64_t depth = 0;
    char quote = '\0';
    int64_t arg_idx = 0;
    int64_t arg_begin = args_begin;
    ObString model_arg;
    for (int64_t i = args_begin; OB_SUCC(ret) && i <= args_end; ++i) {
      const char c = i == args_end ? ',' : text.ptr()[i];
      if ('\0' != quote) {
        if ('\\' == c) {
          ++i;
        } else if (quote == c) {
          quote = '\0';
        }
      } else if ('\'' == c || '"' == c || '`' == c) {
        quote = c;
      } else if ('(' == c) {
        ++depth;
      } else if (')' == c) {
        --depth;
      } else if (',' == c && 0 == depth) {
        const ObString arg(static_cast<int32_t>(i - arg_begin), text.ptr() + arg_begin);
        if (0 == arg_idx) {
          model_arg = arg.trim();
        } else if (1 == arg_idx) {
          content_expr = arg.trim();
        }
        ++arg_idx;
        arg_begin = i + 1;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (arg_idx < 2 || model_arg.length() < 2
               || ('\'' != model_arg.ptr()[0] && '"' != model_arg.ptr()[0])
               || model_arg.ptr()[0] != model_arg.ptr()[model_arg.length() - 1]) {
      // the model name is the only argument that has to be a string literal
      ret = OB_NOT_SUPPORTED;
    } else if (content_expr.empty()) {
      ret = OB_NOT_SUPPORTED;
    } else {
      char *buf = nullptr;
      if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(model_arg.length())))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        const char delimiter = model_arg.ptr()[0];
        int64_t len = 0;
        for (int64_t i = 1; i < model_arg.length() - 1; ++i) {
          char c = model_arg.ptr()[i];
          if ('\\' == c && i + 1 < model_arg.length() - 1) {
            c = model_arg.ptr()[++i];
          } else if (delimiter == c && i + 1 < model_arg.length() - 1
                     && delimiter == model_arg.ptr()[i + 1]) {
            ++i;
          }
          buf[len++] = c;
        }
        if (0 == len) {
          ret = OB_NOT_SUPPORTED;
        } else {
          model_name.assign_ptr(buf, static_cast<int32_t>(len));
          is_embedding = 0 == func_name.case_compare(AI_EMBED_FUNC_NAME);
        }
      }
    }
  }
  return ret;
}

// The task rows of the row version are only usable when no newer request of the row replaced
// them. The rows are read, not locked: the dml side takes the lock of the user row before it
// writes the task rows, so this transaction takes the same two locks in the same order (the
// user row first, the task rows last) and the two can not deadlock. A request that replaces
// the task rows between this read and the write of the values wins on its own, because its
// values are written after it and its own task rows are the ones that survive.
int task_rows_are_current(ObMySQLTransaction &trans,
                          const ObAiColumnTaskGroup &group,
                          bool &is_current)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t task_count = 0;
  is_current = false;
  if (OB_FAIL(sql.assign_fmt("SELECT column_id FROM %s WHERE table_id = %lu AND request_id = %ld",
                             AI_COLUMN_TASK_TABLE, group.table_id_, group.request_id_))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(trans.read(res, sql.ptr()))) {
        LOG_WARN("fail to read the task rows of a row version", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret)) {
          int tmp_ret = result->next();
          if (OB_ITER_END == tmp_ret) {
            break;
          } else if (OB_SUCCESS != tmp_ret) {
            ret = tmp_ret;
          } else {
            ++task_count;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    is_current = task_count == group.items_.count();
  }
  return ret;
}

// Drop every task row of the row version.
int delete_group_tasks(ObMySQLTransaction &trans, const ObAiColumnTaskGroup &group)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu AND request_id = %ld",
                             AI_COLUMN_TASK_TABLE, group.table_id_, group.request_id_))) {
  } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
    LOG_WARN("fail to delete the task rows of a row version", KR(ret), K(sql));
  }
  return ret;
}

// Write every value of the row version and mark the ai columns of the row as generated. One
// statement, so a reader sees all ai columns of the row filled or none of them.
int update_group_values(ObMySQLTransaction &trans, const ObAiColumnTaskGroup &group)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (group.items_.empty()) {
  } else if (OB_FAIL(sql.append("UPDATE "))) {
  } else if (OB_FAIL(append_sql_table(sql, group.db_name_, group.table_name_))) {
  } else if (OB_FAIL(sql.append(" SET "))) {
  }
  for (int64_t i = 0; !group.items_.empty() && OB_SUCC(ret) && i < group.items_.count(); ++i) {
    const ObAiColumnTaskItem &item = group.items_.at(i);
    if (i > 0 && OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_sql_identifier(sql, item.column_name_))) {
    } else if (OB_FAIL(sql.append(" = "))) {
    } else if (OB_FAIL(append_sql_literal(sql, item.result_))) {
    } else if (OB_FAIL(sql.append(", "))) {
    } else if (OB_FAIL(append_sql_identifier(sql, item.state_column_name_))) {
    } else if (OB_FAIL(sql.append_fmt(" = %ld", AI_COLUMN_STATE_READY))) {
    }
  }
  if (!group.items_.empty() && OB_SUCC(ret)) {
    if (OB_FAIL(sql.append(" WHERE "))) {
    } else if (OB_FAIL(sql.append(group.rowkey_predicate_))) {
    } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
      LOG_WARN("fail to write the values of a row version", KR(ret), K(sql));
    } else if (0 == affected_rows) {
      // the row of the row version is gone, the task rows are dropped, the value with them
      LOG_INFO("the row of an ai column request is gone", "table_id", group.table_id_,
               "request_id", group.request_id_, K(group.rowkey_predicate_));
    }
  }
  return ret;
}

} // anonymous namespace

// ------------------------------ ObAiColumnInnerSql ------------------------------

int ObAiColumnInnerSql::init(ObMySQLProxy &sql_proxy)
{
  int ret = OB_SUCCESS;
  if (conn_.is_valid()) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(sql_proxy.acquire_connection(conn_, 0 /*group_id*/))) {
    LOG_WARN("fail to take the inner connection of the ai column worker", KR(ret));
  } else if (!conn_.is_valid()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the inner connection of the ai column worker is invalid", KR(ret));
  } else {
    LOG_INFO("ai column worker took its inner connection");
  }
  return ret;
}

int ObAiColumnInnerSql::escape(const char *from, const int64_t from_size, char *to,
                               const int64_t to_size, int64_t &out_size)
{
  // the worker quotes the values it writes itself (append_sql_literal), it never asks a
  // connection to escape for it
  UNUSEDx(from, from_size, to, to_size);
  out_size = 0;
  return OB_NOT_SUPPORTED;
}

int ObAiColumnInnerSql::read(ObISQLClient::ReadResult &res, const char *sql,
                            const int32_t group_id)
{
  int ret = OB_SUCCESS;
  UNUSED(group_id);
  res.reset();
  if (OB_ISNULL(sql)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(conn_.get_ptr())) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(conn_->execute_read(ObString::make_string(sql), res))) {
  }
  return ret;
}

int ObAiColumnInnerSql::write(const char *sql, const int32_t group_id, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  UNUSED(group_id);
  if (OB_ISNULL(sql)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(conn_.get_ptr())) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(conn_->execute_write(ObString::make_string(sql), affected_rows))) {
  }
  return ret;
}

int ObAiColumnInnerSql::acquire_connection(ObISQLConnectionGuard &conn, const int32_t group_id)
{
  int ret = OB_SUCCESS;
  UNUSED(group_id);
  // hand out the one connection of the worker, so a transaction runs on it as well
  if (!conn_.is_valid()) {
    ret = OB_NOT_INIT;
  } else {
    conn = conn_;
  }
  return ret;
}

ObISQLConnection *ObAiColumnInnerSql::get_connection()
{
  return conn_.get_ptr();
}

// ------------------------------ ObAiColumnTaskScan ------------------------------

int ObAiColumnTaskScan::init(ObAiColumnWorker *worker)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(worker)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    worker_ = worker;
    // the scan issues one statement, a blocking model call never runs on the timer thread
    disable_timeout_check();
    is_inited_ = true;
  }
  return ret;
}

void ObAiColumnTaskScan::runTimerTask()
{
  ObCurTraceId::init(GCTX.self_addr());
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(worker_->scan_tasks())) {
    LOG_WARN("fail to scan the ai column tasks", KR(ret));
  }
}

// ------------------------------ ObAiColumnWorker ------------------------------

ObAiColumnWorker::ObAiColumnWorker()
  : ObSimpleThreadPool(),
    is_inited_(false),
    timer_(),
    scan_task_(),
    in_flight_(),
    in_flight_lock_()
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
  destroy();
}

int ObAiColumnWorker::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(timer_.init("AiColumnWorker", ObMemAttr("AiColWorker")))) {
    LOG_WARN("fail to init the ai column worker timer", KR(ret));
  } else if (OB_FAIL(scan_task_.init(this))) {
    LOG_WARN("fail to init the ai column task scan", KR(ret));
  } else if (OB_FAIL(ObSimpleThreadPool::init(MIN_THREAD_COUNT, MAX_QUEUED_GROUPS,
                                              "AiColumnWorker"))) {
    LOG_WARN("fail to init the ai column worker pool", KR(ret));
  } else {
    is_inited_ = true;
    LOG_INFO("ai column worker is inited");
  }
  if (OB_FAIL(ret)) {
    timer_.destroy();
  }
  return ret;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql_proxy)) {
    // the inner sql proxy is published before the modules start, a null here is a hard error
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the inner sql proxy is not available", KR(ret));
  } else if (OB_FAIL(inner_sql_.init(*sql_proxy))) {
    LOG_WARN("fail to take the inner connection of the ai column worker", KR(ret));
  } else if (worker_mode_is("off")) {
    // TEMPORARY (diagnosis): the module stays registered, inited and started, the scan never runs
    LOG_INFO("ai column worker is started without the scan timer for diagnosis");
  } else if (OB_FAIL(ObSimpleThreadPool::set_adaptive_thread(MIN_THREAD_COUNT, MAX_THREAD_COUNT))) {
    LOG_WARN("fail to set the thread count of the ai column worker", KR(ret));
  } else if (get_thread_count() <= 0 && !try_expand_one(MIN_THREAD_COUNT)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("fail to start the ai column worker threads", KR(ret));
  } else if (worker_mode_is("timernopool")) {
    // TEMPORARY (diagnosis): the scan runs on the timer thread, the pool stays out of it
    LOG_INFO("ai column worker runs the scan only, for diagnosis");
  } else if (OB_FAIL(timer_.schedule(scan_task_, SCAN_INTERVAL_US, true /*repeat*/,
                                     true /*immediate*/))) {
    LOG_WARN("fail to schedule the ai column task scan", KR(ret));
  } else {
    LOG_INFO("ai column worker is started", "scan_interval_us", SCAN_INTERVAL_US,
             "mode", ai_column_worker_mode());
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  if (is_inited_) {
    // the scan stops first, nothing is queued into a pool that is going away
    timer_.stop();
    ObSimpleThreadPool::stop();
    LOG_INFO("ai column worker is stopped");
  }
}

void ObAiColumnWorker::wait()
{
  if (is_inited_) {
    timer_.wait();
    ObSimpleThreadPool::wait();
    LOG_INFO("ai column worker finished waiting");
  }
}

void ObAiColumnWorker::destroy()
{
  if (is_inited_) {
    ObSimpleThreadPool::stop();
    ObSimpleThreadPool::wait();
    ObSimpleThreadPool::destroy();
    timer_.destroy();
    is_inited_ = false;
    LOG_INFO("ai column worker is destroyed");
  }
}

int ObAiColumnWorker::scan_tasks()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnScan");
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(scan_tasks_(allocator))) {
  }
  return ret;
}

// Read the task rows that may run now and hand every row version to the thread pool. The
// scan is the only place that touches the task table for reading, and it never waits for a
// model service: the model calls happen on the pool.
int ObAiColumnWorker::scan_tasks_(ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObSEArray<ScannedTaskRow, 32> rows;
  if (!inner_sql_.is_inited()) {
    // the inner connection could not be taken at start, try again on the next scan
    if (OB_FAIL(inner_sql_.init(*GCTX.sql_proxy_))) {
    }
  } else if (OB_FAIL(sql.assign_fmt(
               "SELECT table_id, column_id, request_id, retry_count, rowkey_predicate"
               " FROM %s WHERE eligible_ts <= now(6)"
               " ORDER BY table_id, request_id, column_id LIMIT %ld",
               AI_COLUMN_TASK_TABLE, SCAN_BATCH_SIZE))) {
  } else {
    lib::ObMutexGuard guard(inner_sql_.get_lock());
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(inner_sql_.read(res, sql.ptr(), 0))) {
        if (REACH_TIME_INTERVAL(10 * 1000 * 1000L)) {
          LOG_WARN("fail to read the ai column tasks", KR(ret), K(sql));
        }
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret)) {
          int64_t table_id = 0;
          int64_t column_id = 0;
          int64_t request_id = 0;
          int64_t retry_count = 0;
          ObString rowkey_predicate;
          ScannedTaskRow row;
          int tmp_ret = result->next();
          if (OB_ITER_END == tmp_ret) {
            break;
          } else if (OB_SUCCESS != tmp_ret) {
            ret = tmp_ret;
            LOG_WARN("fail to get the next ai column task", KR(ret));
          } else if (OB_FAIL(result->get_int("table_id", table_id))) {
          } else if (OB_FAIL(result->get_int("column_id", column_id))) {
          } else if (OB_FAIL(result->get_int("request_id", request_id))) {
          } else if (OB_FAIL(result->get_int("retry_count", retry_count))) {
          } else if (OB_FAIL(result->get_varchar("rowkey_predicate", rowkey_predicate))) {
          } else if (FALSE_IT(row.table_id_ = static_cast<uint64_t>(table_id))) {
          } else if (FALSE_IT(row.column_id_ = static_cast<uint64_t>(column_id))) {
          } else if (FALSE_IT(row.request_id_ = request_id)) {
          } else if (FALSE_IT(row.retry_count_ = retry_count)) {
          } else if (OB_FAIL(copy_string(allocator, rowkey_predicate, row.rowkey_predicate_))) {
          } else if (OB_FAIL(rows.push_back(row))) {
          }
        }
      }
    }
  }
  // the scanned rows are ordered by table, request and column, so the rows of one row version
  // are adjacent and the request ids of two row versions never mix
  if (worker_mode_is("scan")) {
    // TEMPORARY (diagnosis): the read above ran, nothing is handed to the pool
    return ret;
  }
  // One schema guard for the whole scan. The table schema build_group() resolves belongs to the
  // guard, so it has to outlive every build_item() of that row version.
  ObSchemaGetterGuard schema_guard;
  ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!schema_service->is_runtime_schema_ready()) {
    ret = OB_EAGAIN;
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(schema_guard))) {
    if (REACH_TIME_INTERVAL(10 * 1000 * 1000L)) {
      LOG_WARN("fail to get the runtime schema guard of the ai column worker", KR(ret));
    }
  }
  int64_t row_idx = 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
    const ScannedTaskRow &first = rows.at(i);
    if (i > 0) {
      const ScannedTaskRow &prev = rows.at(i - 1);
      if (prev.table_id_ == first.table_id_ && prev.request_id_ == first.request_id_
          && 0 == prev.rowkey_predicate_.compare(first.rowkey_predicate_)) {
        continue;
      }
    }
    row_idx = i;
    if (is_group_in_flight(first.table_id_, first.request_id_, first.rowkey_predicate_)) {
      continue;
    }
    ObAiColumnTaskGroup *group = nullptr;
    const ObTableSchema *table_schema = nullptr;
    void *buf = nullptr;
    bool broken = false;
    if (OB_ISNULL(buf = ob_malloc(sizeof(ObAiColumnTaskGroup), ObMemAttr("AiColGroup")))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(group = new (buf) ObAiColumnTaskGroup())) {
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(build_group(schema_guard, first.table_id_, first.request_id_,
                                   first.rowkey_predicate_, *group, table_schema))) {
      // The task rows can never be generated when the table of the task is gone or the column is
      // not an ai column any more, and a row that stays in the table is scanned again on every
      // scan: the scan reads the oldest rows first, so a pile of such rows would starve every
      // other ai column of the server. Those rows are dropped here; a failure that may be
      // transient leaves them for the next scan.
      const int build_ret = ret;
      if (OB_TABLE_NOT_EXIST == build_ret || OB_NOT_SUPPORTED == build_ret
          || OB_ERR_BAD_FIELD_ERROR == build_ret) {
        int del_ret = delete_orphan_tasks(first.table_id_, first.request_id_);
        if (OB_SUCCESS != del_ret && REACH_TIME_INTERVAL(10 * 1000 * 1000L)) {
          LOG_WARN("fail to drop the task rows of a table that is gone", K(del_ret),
                   "table_id", first.table_id_, "request_id", first.request_id_);
        }
      }
      if (REACH_TIME_INTERVAL(10 * 1000 * 1000L)) {
        LOG_WARN("fail to build the ai column task group", K(build_ret),
                 "table_id", first.table_id_, "request_id", first.request_id_);
      }
      free_group(group);
      group = nullptr;
      ret = OB_SUCCESS;
    }
    for (; OB_SUCC(ret) && !broken && nullptr != group && row_idx < rows.count(); ++row_idx) {
      const ScannedTaskRow &row = rows.at(row_idx);
      ObAiColumnTaskItem item;
      if (row.table_id_ != first.table_id_ || row.request_id_ != first.request_id_
          || 0 != row.rowkey_predicate_.compare(first.rowkey_predicate_)) {
        // the row version ends here
        break;
      } else if (OB_FAIL(build_item(*table_schema, row.column_id_, row.retry_count_,
                                    *group, item))) {
        // an ai column of the task can not be generated any more: the failure is published on
        // its task row below instead of being retried forever
        LOG_WARN("fail to build the ai column task item", KR(ret),
                 "table_id", row.table_id_, "column_id", row.column_id_);
        ObAiColumnTaskItem failed_item;
        failed_item.column_id_ = row.column_id_;
        failed_item.retry_count_ = row.retry_count_;
        failed_item.max_retries_ = 0;
        if (OB_FAIL(group->items_.push_back(failed_item))) {
        } else {
          broken = true;
        }
      }
    }
    if (OB_FAIL(ret)) {
      free_group(group);
      group = nullptr;
    } else if (OB_NOT_NULL(group)) {
      if (broken) {
        int tmp_ret = retry_group(*group);
        if (OB_SUCCESS != tmp_ret && REACH_TIME_INTERVAL(10 * 1000 * 1000L)) {
          LOG_WARN("fail to publish the failure of an ai column task group", K(tmp_ret));
        }
        free_group(group);
        group = nullptr;
      } else if (OB_FAIL(add_group_in_flight(group))) {
        free_group(group);
        group = nullptr;
      } else if (OB_FAIL(push(group))) {
        LOG_WARN("fail to push an ai column task group", KR(ret),
                 "table_id", group->table_id_, "request_id", group->request_id_);
        remove_group_in_flight(group);
        free_group(group);
        group = nullptr;
        ret = OB_SUCCESS;
      }
    }
  }
  return ret;
}

int ObAiColumnWorker::build_group(ObSchemaGetterGuard &guard,
                                  const uint64_t table_id,
                                  const int64_t request_id,
                                  const ObString &rowkey_predicate,
                                  ObAiColumnTaskGroup &group,
                                  const ObTableSchema *&table_schema)
{
  int ret = OB_SUCCESS;
  const ObDatabaseSchema *database_schema = nullptr;
  if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(),
                                               database_schema))) {
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(copy_string(group.allocator_, database_schema->get_database_name_str(),
                                 group.db_name_))) {
  } else if (OB_FAIL(copy_string(group.allocator_, table_schema->get_table_name_str(),
                                 group.table_name_))) {
  } else if (OB_FAIL(copy_string(group.allocator_, rowkey_predicate,
                                 group.rowkey_predicate_))) {
  } else {
    group.table_id_ = table_id;
    group.request_id_ = request_id;
  }
  return ret;
}

int ObAiColumnWorker::build_item(const ObTableSchema &table_schema,
                                 const uint64_t column_id,
                                 const int64_t retry_count,
                                 ObAiColumnTaskGroup &group,
                                 ObAiColumnTaskItem &item)
{
  int ret = OB_SUCCESS;
  const ObColumnSchemaV2 *ai_column = table_schema.get_column_schema(column_id);
  share::ObAiColumnSpec spec;
  char name_buf[OB_MAX_COLUMN_NAME_LENGTH];
  ObString state_column_name;
  if (OB_ISNULL(ai_column)) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (!ai_column->is_ai_column()) {
    // the task outlived the ai column it was made for
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(share::ObAiColumnSpec::load_from_ai_column(table_schema, *ai_column,
                                                               spec))) {
    LOG_WARN("fail to read the definition of the ai column", KR(ret),
             K(ai_column->get_column_name_str()));
  } else if (OB_FAIL(share::ObAiColumnSpec::build_hidden_column_name(
               share::ObAiColumnSpec::HIDDEN_STATE_COLUMN,
               ai_column->get_column_name_str(), name_buf, sizeof(name_buf),
               state_column_name))) {
  } else if (OB_FAIL(copy_string(group.allocator_, ai_column->get_column_name_str(),
                                 item.column_name_))) {
  } else if (OB_FAIL(copy_string(group.allocator_, state_column_name,
                                 item.state_column_name_))) {
  } else if (OB_FAIL(copy_string(group.allocator_, spec.get_normalized_expr(),
                                 item.system_prompt_))) {
  } else if (OB_FAIL(parse_ai_column_expr(group.allocator_, spec.get_expr(),
                                          item.is_embedding_, item.model_name_,
                                          item.user_expr_))) {
    LOG_WARN("an ai column expression is not a call of an ai function", KR(ret),
             K(spec.get_expr()));
  } else {
    item.column_id_ = column_id;
    item.retry_count_ = retry_count;
    item.max_retries_ = spec.get_max_retries();
    if (OB_FAIL(group.items_.push_back(item))) {
    }
  }
  return ret;
}

// Ask the model service for the value of one ai column of the row version. The request
// carries the expression of the ai column as the system prompt and the value of that
// expression for the row as the user message.
int ObAiColumnWorker::evaluate_item(ObAiColumnTaskGroup &group, ObAiColumnTaskItem &item)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString content;
  if (OB_FAIL(sql.append("SELECT ", 7))) {
  } else if (OB_FAIL(sql.append(item.user_expr_))) {
  } else if (OB_FAIL(sql.append(" FROM ", 6))) {
  } else if (OB_FAIL(append_sql_table(sql, group.db_name_, group.table_name_))) {
  } else if (OB_FAIL(sql.append(" WHERE ", 7))) {
  } else if (OB_FAIL(sql.append(group.rowkey_predicate_))) {
  } else {
    // the read is the only statement of the evaluation, the model call below runs without the
    // connection lock so that the requests of different row versions overlap
    lib::ObMutexGuard guard(inner_sql_.get_lock());
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      ObString value;
      int tmp_ret = OB_SUCCESS;
      if (OB_FAIL(inner_sql_.read(res, sql.ptr(), 0))) {
        LOG_WARN("fail to evaluate the value to ask the model with", KR(ret), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ITER_END == (tmp_ret = result->next())) {
        // the row of the row version is gone, there is nothing left to generate
        ret = OB_ENTRY_NOT_EXIST;
      } else if (OB_SUCCESS != tmp_ret) {
        ret = tmp_ret;
      } else if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), value))) {
        LOG_WARN("fail to get the value to ask the model with", KR(ret), K(sql));
      } else if (OB_FAIL(copy_string(group.allocator_, value, content))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (content.empty()) {
    // the expression of the ai column has no value for this row, the model service is never
    // asked with an empty message
    ret = OB_INVALID_DATA;
  } else {
    query::ObIAiEndpointResolver *endpoint_resolver =
        share::server_service<query::ObIAiEndpointResolver>();
    share::ObAiModelEndpointInfo endpoint_info;
    ObAIFuncExprInfo *info = nullptr;
    if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the ai endpoint resolver is not available", KR(ret));
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                 item.model_name_, group.allocator_, endpoint_info, false /*check_access*/))) {
      LOG_WARN("fail to resolve the ai model endpoint of an ai column", KR(ret),
               K(item.model_name_));
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(group.allocator_, item.model_name_,
                                                       info))) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObAIFuncModel model(group.allocator_, *info, endpoint_info);
      ObString system_prompt = item.system_prompt_;
      if (item.is_embedding_) {
        if (OB_FAIL(model.call_dense_embedding(content, nullptr, item.result_))) {
          LOG_WARN("fail to ask the model service for an embedding", KR(ret), K(item.model_name_));
        }
      } else if (OB_FAIL(model.call_completion(system_prompt, content, nullptr, item.result_))) {
        LOG_WARN("fail to ask the model service for a completion", KR(ret), K(item.model_name_));
      }
    }
  }
  return ret;
}

// Write the generated values of the row version, mark its ai columns as generated and drop
// its task rows in one transaction, so the row version becomes visible as a whole.
int ObAiColumnWorker::write_group(ObAiColumnTaskGroup &group, const bool row_missing)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  bool is_current = false;
  // the whole transaction runs on the one connection of the worker, so it is serialised against
  // the other statements of the pool with the connection lock
  lib::ObMutexGuard guard(inner_sql_.get_lock());
  if (group.items_.empty()) {
  } else if (OB_FAIL(trans.start(&inner_sql_))) {
    LOG_WARN("fail to start the transaction of an ai column row version", KR(ret));
  } else if (OB_FAIL(task_rows_are_current(trans, group, is_current))) {
  } else if (!is_current) {
    // a newer request of the row replaced these task rows while the model was asked, its
    // values win and this answer is thrown away
    LOG_INFO("drop the values of a replaced ai column request", "table_id", group.table_id_,
             "request_id", group.request_id_);
  } else if (!row_missing && OB_FAIL(update_group_values(trans, group))) {
  } else if (OB_FAIL(delete_group_tasks(trans, group))) {
  }
  if (trans.is_started()) {
    int tmp_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to end the transaction of an ai column row version", KR(ret), K(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
  return ret;
}

// Publish the failure of a row version: its task rows wait for the back off before they are
// tried again, and a task row whose ai column ran out of retries is dropped, the value of that
// column stays null. The failure is never reported to a client.
int ObAiColumnWorker::delete_orphan_tasks(const uint64_t table_id, const int64_t request_id)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  // one statement on the connection of the worker, so the transaction of another item can not
  // interleave with it
  lib::ObMutexGuard guard(inner_sql_.get_lock());
  if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu AND request_id = %ld",
                             AI_COLUMN_TASK_TABLE, table_id, request_id))) {
    LOG_WARN("fail to build the statement that drops an orphan task", KR(ret));
  } else if (OB_FAIL(inner_sql_.write(sql.ptr(), 0, affected_rows))) {
    LOG_WARN("fail to drop the orphan task rows of an ai column", KR(ret), K(sql));
  } else {
    LOG_INFO("dropped the task rows of a row version whose table is gone", K(table_id),
             K(request_id), K(affected_rows));
  }
  return ret;
}

int ObAiColumnWorker::retry_group(ObAiColumnTaskGroup &group)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  // same as write_group: the transaction is serialised on the connection of the worker
  lib::ObMutexGuard guard(inner_sql_.get_lock());
  if (group.items_.empty()) {
  } else if (OB_FAIL(trans.start(&inner_sql_))) {
    LOG_WARN("fail to start the retry transaction of an ai column row version", KR(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < group.items_.count(); ++i) {
      const ObAiColumnTaskItem &item = group.items_.at(i);
      const int64_t retry_count = item.retry_count_ + 1;
      ObSqlString sql;
      int64_t affected_rows = 0;
      if (retry_count > item.max_retries_) {
        if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu AND column_id = %lu"
                                   " AND request_id = %ld",
                                   AI_COLUMN_TASK_TABLE, group.table_id_, item.column_id_,
                                   group.request_id_))) {
        }
      } else {
        // the first retry waits a second, every further retry twice as long as the one before
        int64_t backoff_sec = RETRY_BACKOFF_BASE_SEC;
        for (int64_t n = retry_count - 1; n > 1 && backoff_sec < RETRY_BACKOFF_MAX_SEC; --n) {
          backoff_sec = backoff_sec * 2 < RETRY_BACKOFF_MAX_SEC
                        ? backoff_sec * 2 : RETRY_BACKOFF_MAX_SEC;
        }
        if (OB_FAIL(sql.assign_fmt(
              "UPDATE %s SET state = %ld, retry_count = %ld,"
              " eligible_ts = date_add(now(6), interval %ld second)"
              " WHERE table_id = %lu AND column_id = %lu AND request_id = %ld",
              AI_COLUMN_TASK_TABLE, AI_COLUMN_TASK_STATE_FAILED, retry_count,
              backoff_sec, group.table_id_, item.column_id_, group.request_id_))) {
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(trans.write(sql.ptr(), affected_rows))) {
        LOG_WARN("fail to publish the failure of an ai column task", KR(ret), K(sql));
      }
    }
  }
  if (trans.is_started()) {
    int tmp_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("fail to end the retry transaction of an ai column row version", KR(ret), K(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
  return ret;
}

int ObAiColumnWorker::process_group(ObAiColumnTaskGroup &group)
{
  int ret = OB_SUCCESS;
  bool row_missing = false;
  for (int64_t i = 0; OB_SUCC(ret) && i < group.items_.count(); ++i) {
    if (OB_FAIL(evaluate_item(group, group.items_.at(i)))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        ret = OB_SUCCESS;
        row_missing = true;
        break;
      }
    }
  }
  if (OB_FAIL(ret)) {
    // a model service that does not answer is not an error of the statement that made the
    // task, the task row carries the failure instead
    int tmp_ret = retry_group(group);
    LOG_WARN("fail to generate the values of an ai column row version", KR(ret),
             "table_id", group.table_id_, "request_id", group.request_id_);
    ret = tmp_ret;
  } else if (OB_FAIL(write_group(group, row_missing))) {
    // the value of the row version could not be published, for instance because it does not
    // fit the column of the ai column. The task rows carry that failure too: without it they
    // would stay eligible and the worker would ask the model service again on every scan.
    const int write_ret = ret;
    LOG_WARN("fail to publish the values of an ai column row version", KR(ret),
             "table_id", group.table_id_, "request_id", group.request_id_);
    ret = retry_group(group);
    if (OB_SUCC(ret)) {
      ret = write_ret;
    }
  }
  return ret;
}

void ObAiColumnWorker::handle(void *task)
{
  int ret = OB_SUCCESS;
  ObAiColumnTaskGroup *group = static_cast<ObAiColumnTaskGroup *>(task);
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(group)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(process_group(*group))) {
    LOG_WARN("fail to process an ai column row version", KR(ret),
             "table_id", group->table_id_, "request_id", group->request_id_);
  }
  if (OB_NOT_NULL(group)) {
    // the row version is no longer in flight, a task that is still in the table is scanned
    // again and is retried from scratch
    remove_group_in_flight(group);
    free_group(group);
  }
}

void ObAiColumnWorker::handle_drop(void *task)
{
  ObAiColumnTaskGroup *group = static_cast<ObAiColumnTaskGroup *>(task);
  if (OB_NOT_NULL(group)) {
    LOG_INFO("drop an ai column row version that was never processed",
             "table_id", group->table_id_, "request_id", group->request_id_);
    remove_group_in_flight(group);
    free_group(group);
  }
}

bool ObAiColumnWorker::is_group_in_flight(const uint64_t table_id,
                                          const int64_t request_id,
                                          const ObString &rowkey_predicate)
{
  bool found = false;
  ObSpinLockGuard guard(in_flight_lock_);
  for (int64_t i = 0; !found && i < in_flight_.count(); ++i) {
    const ObAiColumnTaskGroup *group = in_flight_.at(i);
    found = OB_NOT_NULL(group) && group->table_id_ == table_id
        && group->request_id_ == request_id
        && 0 == group->rowkey_predicate_.compare(rowkey_predicate);
  }
  return found;
}

int ObAiColumnWorker::add_group_in_flight(ObAiColumnTaskGroup *group)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(group)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObSpinLockGuard guard(in_flight_lock_);
    if (OB_FAIL(in_flight_.push_back(group))) {
      LOG_WARN("fail to mark an ai column row version as in flight", KR(ret));
    }
  }
  return ret;
}

void ObAiColumnWorker::remove_group_in_flight(ObAiColumnTaskGroup *group)
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(in_flight_lock_);
  for (int64_t i = 0; OB_SUCC(ret) && i < in_flight_.count(); ++i) {
    if (in_flight_.at(i) == group) {
      ret = in_flight_.remove(i);
    }
  }
}

void ObAiColumnWorker::free_group(ObAiColumnTaskGroup *group)
{
  if (OB_NOT_NULL(group)) {
    group->~ObAiColumnTaskGroup();
    ob_free(group);
  }
}

} // namespace observer
} // namespace oceanbase
