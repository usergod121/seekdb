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

#define USING_LOG_PREFIX SQL_RESV
#include <stdio.h>
#include <string.h>
#include "sql/resolver/ob_ai_column_access.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "lib/string/ob_sql_string.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "share/ob_errno.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_schema_struct.h"
#include "share/schema/ob_table_schema.h"
#include "share/system_variable/ob_sys_var_class_type.h"
#include "sql/session/ob_sql_session_info.h"

namespace oceanbase
{
namespace sql
{

using namespace common;
using namespace share;
using namespace share::schema;

namespace
{

// The hidden state column of an ai column is 1 once the value of that column of a row is
// generated. Every other value means that the row still owes the value, including the null the
// column of a row that was never generated holds.
const int64_t AI_COLUMN_STATE_READY = 1;
const int64_t AI_COLUMN_STATE_PENDING = 0;
// The retry predicate addresses the generation state of the retried ai column through this
// unquoted name, a backquoted `status` is the physical column of the table named that way.
const char *const STATUS_PSEUDO_COLUMN_NAME = "status";
const int64_t STATUS_PSEUDO_COLUMN_NAME_LENGTH = 6;

bool is_identifier_char(const char c)
{
  return ('_' == c) || ('$' == c) || (c >= '0' && c <= '9')
      || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_blank(const char c)
{
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c;
}

bool is_identifier_start(const char c)
{
  return ('_' == c) || ('$' == c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_status_name(const char *text, const int64_t length)
{
  bool is_status = length == STATUS_PSEUDO_COLUMN_NAME_LENGTH;
  for (int64_t i = 0; is_status && i < length; ++i) {
    const char lower = (text[i] >= 'A' && text[i] <= 'Z')
                       ? static_cast<char>(text[i] + ('a' - 'A')) : text[i];
    is_status = lower == STATUS_PSEUDO_COLUMN_NAME[i];
  }
  return is_status;
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

int append_sql_table(ObSqlString &sql, const ObString &database, const ObString &table)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(append_sql_identifier(sql, database))) {
  } else if (OB_FAIL(sql.append(".", 1))) {
  } else if (OB_FAIL(append_sql_identifier(sql, table))) {
  }
  return ret;
}

// The generation state of the retried ai column as the value the predicate compares with.
int append_status_expression(ObSqlString &sql, const ObString &state_column_name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("(case when "))) {
  } else if (OB_FAIL(append_sql_identifier(sql, state_column_name))) {
  } else if (OB_FAIL(sql.append_fmt(" = %ld then 'READY' else 'FAILED' end)",
                                    AI_COLUMN_STATE_READY))) {
  }
  return ret;
}

// The text of the retry predicate with the STATUS pseudo column replaced by the generation
// state of the retried ai column. Only an unquoted identifier is the pseudo column: a quoted
// one names a column of the table, and the text of a literal or of a comment is never a name.
int rewrite_status_pseudo_column(const ObString &predicate,
                                 const ObString &state_column_name,
                                 ObSqlString &rewritten)
{
  int ret = OB_SUCCESS;
  const char *text = predicate.ptr();
  const int64_t length = predicate.length();
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < length) {
    const char c = text[pos];
    int64_t end = pos + 1;
    bool is_pseudo_column = false;
    if ('\'' == c || '"' == c) {
      // a literal keeps its text: a doubled quote and a back slash escape stay inside it
      end = pos + 1;
      while (end < length) {
        if ('\\' == text[end] && end + 1 < length) {
          end += 2;
        } else if (c == text[end] && end + 1 < length && c == text[end + 1]) {
          end += 2;
        } else if (c == text[end]) {
          ++end;
          break;
        } else {
          ++end;
        }
      }
    } else if ('`' == c) {
      // a quoted identifier names the physical column of the table
      end = pos + 1;
      while (end < length) {
        if ('`' == text[end] && end + 1 < length && '`' == text[end + 1]) {
          end += 2;
        } else if ('`' == text[end]) {
          ++end;
          break;
        } else {
          ++end;
        }
      }
    } else if ('-' == c && pos + 1 < length && '-' == text[pos + 1]) {
      end = pos + 2;
      while (end < length && '\n' != text[end]) {
        ++end;
      }
    } else if ('#' == c) {
      while (end < length && '\n' != text[end]) {
        ++end;
      }
    } else if ('/' == c && pos + 1 < length && '*' == text[pos + 1]) {
      end = pos + 2;
      while (end + 1 < length && !('/' == text[end] && '*' == text[end + 1])) {
        ++end;
      }
      end = (end + 1 < length) ? end + 2 : length;
    } else if (is_identifier_start(c)) {
      while (end < length && is_identifier_char(text[end])) {
        ++end;
      }
      is_pseudo_column = is_status_name(text + pos, end - pos);
    }
    if (OB_FAIL(ret)) {
    } else if (is_pseudo_column) {
      if (OB_FAIL(append_status_expression(rewritten, state_column_name))) {
      }
    } else if (OB_FAIL(rewritten.append(text + pos, end - pos))) {
    }
    pos = end;
  }
  return ret;
}

// The text of the WHERE clause the parser captured starts with its own keyword; the statement
// the retry runs spells its own WHERE, so the keyword is dropped here.
ObString skip_where_keyword(const ObString &where_clause)
{
  const char *text = where_clause.ptr();
  const int64_t length = where_clause.length();
  int64_t pos = 0;
  while (pos < length && is_blank(text[pos])) {
    ++pos;
  }
  if (pos + 5 <= length
      && (pos + 5 == length || !is_identifier_char(text[pos + 5]))
      && (0 == strncmp(text + pos, "where", 5) || 0 == strncmp(text + pos, "WHERE", 5))) {
    pos += 5;
  }
  while (pos < length && is_blank(text[pos])) {
    ++pos;
  }
  return ObString(static_cast<int32_t>(length - pos), text + pos);
}

int get_database_name(const ObTableSchema &table_schema, ObString &database_name)
{
  int ret = OB_SUCCESS;
  ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  ObSchemaGetterGuard schema_guard;
  const ObDatabaseSchema *database_schema = NULL;
  if (OB_ISNULL(schema_service)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the schema service is not available", K(ret));
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(schema_guard))) {
    LOG_WARN("fail to get the schema guard", K(ret));
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema.get_database_id(),
                                                      database_schema))) {
    LOG_WARN("fail to get the database of the table", K(ret),
             K(table_schema.get_database_id()));
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the database of the table is not found", K(ret),
             K(table_schema.get_database_id()));
  } else {
    database_name = database_schema->get_database_name_str();
  }
  return ret;
}

int get_strict_read(const ObSQLSessionInfo &session, bool &enabled)
{
  int ret = OB_SUCCESS;
  enabled = false;
  if (OB_FAIL(session.get_sys_variable(share::SYS_VAR_AI_COLUMN_STRICT_READ, enabled))) {
    LOG_WARN("fail to read the ai column strict read switch", K(ret));
  }
  return ret;
}

// The rows of one retry get request ids of their own, so that the worker groups them one row at a
// time: two rows that share a request id would look like one row version it can not match. The
// counter starts at the wall clock so it can not collide with the ids the dml of a statement hands
// out, which count up from one.
int64_t next_retry_request_id()
{
  static int64_t request_id = ObTimeUtility::current_time();
  return ATOMIC_FAA(&request_id, 1) + 1;
}

// One task row per row the retry selected, replacing whatever task row the row had. A row that is
// already pending with an empty value is not written by the update of the retry at all, so the
// tasks can not be left to the dml of that update. The rowkey predicate of a task row is built by
// the very function the dml of an update builds it with, on the rowkey values read back from the
// table, so the text of a retry task and the text of an update task are the same for one row.
int queue_retry_tasks(const ObTableSchema &table_schema,
                      const ObColumnSchemaV2 &ai_column,
                      const ObString &database_name,
                      const ObSqlString &predicate,
                      ObMySQLProxy &sql_proxy)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 4> rowkey_columns;
  ObSqlString select;
  const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
  if (OB_UNLIKELY(0 >= rowkey_info.get_size())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the table of the retried ai column has no rowkey", K(ret));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    const ObColumnSchemaV2 *rowkey_column = NULL;
    if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
      LOG_WARN("fail to read the rowkey column id", K(ret), K(i));
    } else if (OB_ISNULL(rowkey_column = table_schema.get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the rowkey column of the table is not found", K(ret), K(column_id));
    } else if (OB_FAIL(rowkey_columns.push_back(rowkey_column))) {
      LOG_WARN("fail to push back the rowkey column", K(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(select.append("SELECT "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_columns.count(); ++i) {
    if (i > 0 && OB_FAIL(select.append(", "))) {
    } else if (OB_FAIL(append_sql_identifier(select,
                                             rowkey_columns.at(i)->get_column_name_str()))) {
      LOG_WARN("fail to append the rowkey column", K(ret));
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(select.append(" FROM "))) {
  } else if (OB_FAIL(append_sql_table(select, database_name,
                                      table_schema.get_table_name_str()))) {
  } else if (0 < predicate.length() && OB_FAIL(select.append(" WHERE "))) {
  } else if (0 < predicate.length()
             && OB_FAIL(select.append(predicate.ptr(), predicate.length()))) {
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = NULL;
      if (OB_FAIL(sql_proxy.read(res, select.ptr()))) {
        LOG_WARN("fail to read the rowkeys of the retried rows", K(ret), K(select));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("the rowkeys of the retried rows are not readable", K(ret));
      } else {
        int tmp_ret = OB_SUCCESS;
        while (OB_SUCC(ret) && OB_SUCCESS == (tmp_ret = result->next())) {
          ObSqlString rowkey_predicate;
          ObSqlString insert;
          int64_t affected_rows = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_columns.count(); ++i) {
            const ObColumnSchemaV2 *rowkey_column = rowkey_columns.at(i);
            const ObString &name = rowkey_column->get_column_name_str();
            const char *separator = 0 == i ? "" : " and ";
            if (ob_is_integer_type(rowkey_column->get_data_type())) {
              int64_t value = 0;
              if (OB_FAIL(result->get_int(i, value))) {
                LOG_WARN("fail to read the rowkey value", K(ret), K(i));
              } else if (OB_FAIL(rowkey_predicate.append_fmt("%s`%.*s` = %ld", separator,
                                                             name.length(), name.ptr(), value))) {
                LOG_WARN("fail to append the rowkey predicate", K(ret));
              }
            } else {
              ObString value;
              if (OB_FAIL(result->get_varchar(i, value))) {
                LOG_WARN("fail to read the rowkey value", K(ret), K(i));
              } else if (OB_FAIL(rowkey_predicate.append_fmt("%s`%.*s` = '%.*s'", separator,
                                                             name.length(), name.ptr(),
                                                             value.length(), value.ptr()))) {
                LOG_WARN("fail to append the rowkey predicate", K(ret));
              }
            }
          }
          if (OB_FAIL(ret)) {
          } else if (OB_FAIL(insert.append_fmt(
                       "REPLACE INTO %s (table_id, column_id, rowkey_predicate, gmt_create,"
                       " gmt_modified, request_id, state, retry_count, eligible_ts)"
                       " VALUES (%lu, %lu, '%.*s', now(6), now(6), %ld, %ld, 0, now(6))",
                       "oceanbase.__all_ai_column_task",
                       table_schema.get_table_id(),
                       ai_column.get_column_id(),
                       rowkey_predicate.length(), rowkey_predicate.ptr(),
                       next_retry_request_id(),
                       AI_COLUMN_STATE_PENDING))) {
            LOG_WARN("fail to build the task row of a retried row", K(ret));
          } else if (OB_FAIL(sql_proxy.write(insert.ptr(), affected_rows))) {
            LOG_WARN("fail to write the task row of a retried row", K(ret), K(insert));
          }
        }
        if (OB_SUCC(ret) && OB_ITER_END != tmp_ret) {
          ret = tmp_ret;
          LOG_WARN("fail to walk the rowkeys of the retried rows", K(ret));
        }
      }
    }
  }
  return ret;
}

} // anonymous namespace

int ObAiColumnAccess::check_readable(const ObSQLSessionInfo &session,
                                     const ObTableSchema &table_schema,
                                     const ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  bool enabled = false;
  if (!column.is_ai_column() || session.is_inner()) {
    // only a user statement waits for a value that is still being generated, the inner
    // statements of the server are the ones that produce those values
  } else if (OB_FAIL(get_strict_read(session, enabled))) {
  } else if (!enabled) {
    // without the switch an unfinished value is simply read as it is
  } else {
    char name_buf[OB_MAX_COLUMN_NAME_LENGTH];
    ObString state_column_name;
    ObString database_name;
    ObSqlString sql;
    ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
    if (OB_FAIL(ObAiColumnSpec::build_hidden_column_name(
                  ObAiColumnSpec::HIDDEN_STATE_COLUMN, column.get_column_name_str(),
                  name_buf, sizeof(name_buf), state_column_name))) {
      LOG_WARN("fail to build the state column name of an ai column", K(ret),
               K(column.get_column_name_str()));
    } else if (OB_FAIL(get_database_name(table_schema, database_name))) {
    } else if (OB_ISNULL(sql_proxy)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("the sql proxy is not available", K(ret));
    } else if (OB_FAIL(sql.append("SELECT 1 FROM "))) {
    } else if (OB_FAIL(append_sql_table(sql, database_name,
                                        table_schema.get_table_name_str()))) {
    } else if (OB_FAIL(sql.append(" WHERE ifnull("))) {
    } else if (OB_FAIL(append_sql_identifier(sql, state_column_name))) {
    } else if (OB_FAIL(sql.append_fmt(", %ld) <> %ld LIMIT 1",
                                      AI_COLUMN_STATE_PENDING, AI_COLUMN_STATE_READY))) {
    } else {
      SMART_VAR(ObMySQLProxy::MySQLResult, res) {
        sqlclient::ObMySQLResult *result = NULL;
        int tmp_ret = OB_SUCCESS;
        if (OB_FAIL(sql_proxy->read(res, sql.ptr()))) {
          LOG_WARN("fail to read the state of an ai column", K(ret), K(sql));
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_SUCCESS != (tmp_ret = result->next()) && OB_ITER_END != tmp_ret) {
          ret = tmp_ret;
          LOG_WARN("fail to read the state of an ai column", K(ret), K(sql));
        } else if (OB_SUCCESS == tmp_ret) {
          // one row of the table still owes the value of this column, and a read of the
          // column reads the column of the whole table
          ret = OB_ERR_AI_COLUMN_UNFINISHED_TASK;
          LOG_USER_ERROR(OB_ERR_AI_COLUMN_UNFINISHED_TASK,
                         static_cast<int64_t>(table_schema.get_table_id()),
                         static_cast<int64_t>(column.get_column_id()));
        }
      }
    }
  }
  return ret;
}

int ObAiColumnAccess::retry(const uint64_t table_id,
                            const ObString &column_name,
                            const ObString &predicate,
                            int64_t &retried_rows)
{
  int ret = OB_SUCCESS;
  retried_rows = 0;
  ObMultiVersionSchemaService *schema_service = GCTX.schema_service_;
  ObMySQLProxy *sql_proxy = GCTX.sql_proxy_;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *table_schema = NULL;
  const ObColumnSchemaV2 *ai_column = NULL;
  if (OB_ISNULL(schema_service) || OB_ISNULL(sql_proxy)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("the server services are not available", K(ret));
  } else if (OB_FAIL(schema_service->get_runtime_schema_guard(schema_guard))) {
    LOG_WARN("fail to get the schema guard", K(ret));
  } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table_schema))) {
    LOG_WARN("fail to get the table of the retried ai column", K(ret), K(table_id));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("the table of the retried ai column is not found", K(ret), K(table_id));
  } else if (OB_ISNULL(ai_column = table_schema->get_column_schema(column_name))) {
    ret = OB_ERR_BAD_FIELD_ERROR;
    LOG_WARN("the retried column is not a column of the table", K(ret), K(column_name));
  } else if (!ai_column->is_ai_column()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "retrying a column that is not an AI column");
  } else {
    char name_buf[OB_MAX_COLUMN_NAME_LENGTH];
    ObString state_column_name;
    ObString database_name;
    ObSqlString rewritten;
    ObSqlString sql;
    // the parser hands over the WHERE clause of the statement, keyword included
    const ObString predicate_text = skip_where_keyword(predicate);
    if (OB_FAIL(ObAiColumnSpec::build_hidden_column_name(
                  ObAiColumnSpec::HIDDEN_STATE_COLUMN, ai_column->get_column_name_str(),
                  name_buf, sizeof(name_buf), state_column_name))) {
      LOG_WARN("fail to build the state column name of an ai column", K(ret));
    } else if (OB_FAIL(get_database_name(*table_schema, database_name))) {
    } else if (OB_FAIL(rewrite_status_pseudo_column(predicate_text, state_column_name,
                                                    rewritten))) {
      LOG_WARN("fail to rewrite the predicate of a retry", K(ret), K(predicate));
    } else if (OB_FAIL(sql.append("UPDATE "))) {
    } else if (OB_FAIL(append_sql_table(sql, database_name,
                                        table_schema->get_table_name_str()))) {
    } else if (OB_FAIL(sql.append(" SET "))) {
    } else if (OB_FAIL(append_sql_identifier(sql, ai_column->get_column_name_str()))) {
    } else if (OB_FAIL(sql.append(" = NULL, "))) {
    } else if (OB_FAIL(append_sql_identifier(sql, state_column_name))) {
    } else if (OB_FAIL(sql.append_fmt(" = %ld", AI_COLUMN_STATE_PENDING))) {
    } else if (!rewritten.empty() && OB_FAIL(sql.append(" WHERE "))) {
    } else if (!rewritten.empty() && OB_FAIL(sql.append(rewritten.ptr(), rewritten.length()))) {
    } else if (OB_FAIL(sql_proxy->write(sql.ptr(), retried_rows))) {
      LOG_WARN("fail to retry the rows of an ai column", K(ret), K(sql), K(column_name));
    } else if (OB_FAIL(queue_retry_tasks(*table_schema,
                                         *ai_column,
                                         database_name,
                                         rewritten,
                                         *sql_proxy))) {
      LOG_WARN("fail to queue the retry of an ai column", K(ret), K(column_name));
    } else {
      // the rows the predicate selected lost their value and their generation state went back
      // to pending, and every one of them has a task row again: the update above only writes the
      // rows it changes, so a row that was already pending with an empty value is untouched by
      // it and its task row is the one the queueing wrote
      LOG_INFO("retried the rows of an ai column", K(retried_rows), K(column_name), K(sql));
    }
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
