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

#ifndef OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_

#include "lib/allocator/page_arena.h"
#include "lib/lock/ob_mutex.h"
#include "lib/lock/ob_spin_lock.h"
#include "lib/task/ob_timer.h"
#include "lib/thread/ob_simple_thread_pool.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "share/ai_service/ob_ai_service_struct.h"

namespace oceanbase
{
namespace common
{
class ObMySQLProxy;
namespace sqlclient
{
class ObISQLConnection;
}
}
namespace share
{
namespace schema
{
class ObTableSchema;
class ObSchemaGetterGuard;
}
}
namespace observer
{

class ObAiColumnWorker;

// One inner connection, and with it one session, for the whole life of the worker. Asking the
// server wide inner sql proxy for a connection on every statement created and released a session
// on every scan, a churn no other component of the server produces from a background thread. The
// worker holds one connection instead and runs every statement and every transaction on it. A
// statement or a transaction must not interleave with another one on the same connection, so the
// pool serialises them with lock_; a model call is never made while the lock is held, so the
// requests of different row versions still overlap.
class ObAiColumnInnerSql : public common::ObISQLClient
{
public:
  ObAiColumnInnerSql() : conn_(), lock_() {}
  virtual ~ObAiColumnInnerSql() {}
  // Take the one connection of the worker out of the server wide inner sql proxy.
  int init(common::ObMySQLProxy &sql_proxy);
  void reset() { conn_.reset(); }
  bool is_inited() const { return conn_.is_valid(); }
  lib::ObMutex &get_lock() { return lock_; }
  // common::ObISQLClient
  virtual int escape(const char *from, const int64_t from_size, char *to, const int64_t to_size,
                     int64_t &out_size) override;
  virtual int read(common::ObISQLClient::ReadResult &res, const char *sql,
                   const int32_t group_id) override;
  virtual int write(const char *sql, const int32_t group_id, int64_t &affected_rows) override;
  virtual int acquire_connection(common::sqlclient::ObISQLConnectionGuard &conn,
                                 const int32_t group_id) override;
  virtual common::sqlclient::ObISQLConnection *get_connection() override;
private:
  common::sqlclient::ObISQLConnectionGuard conn_;
  lib::ObMutex lock_;
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnInnerSql);
};

// One ai column of one row version: a row of __all_ai_column_task plus everything that is
// needed to ask the model service for the value of that column. Every string points into the
// arena of the group the item belongs to.
struct ObAiColumnTaskItem
{
  ObAiColumnTaskItem()
    : column_id_(OB_INVALID_ID),
      retry_count_(0),
      max_retries_(0),
      is_embedding_(false)
  {}
  uint64_t column_id_;
  int64_t retry_count_;
  int64_t max_retries_;
  bool is_embedding_;
  ObString column_name_;       // ai column the value is written to
  ObString state_column_name_; // hidden state column of that ai column
  ObString system_prompt_;     // normalized expression of the ai column
  ObString user_expr_;         // argument of the ai function, evaluated against the row
  ObString model_name_;        // ai model the expression is asked with
  ObString result_;            // value the model answered with
};

// All task rows of one row version: one table, one request id, one row key. They are
// processed together so that the values of a row become visible in one update, a reader never
// sees one ai column of a row filled and the other one still empty.
struct ObAiColumnTaskGroup
{
  ObAiColumnTaskGroup()
    : table_id_(OB_INVALID_ID),
      request_id_(0),
      db_name_(),
      table_name_(),
      rowkey_predicate_(),
      items_(),
      allocator_("AiColumnTask")
  {}
  uint64_t table_id_;
  int64_t request_id_;
  ObString db_name_;
  ObString table_name_;
  ObString rowkey_predicate_;
  common::ObSEArray<ObAiColumnTaskItem, 4> items_;
  common::ObArenaAllocator allocator_;
};

// Periodic scan of oceanbase.__all_ai_column_task. The scan only reads the task table and
// hands the row versions to the thread pool of ObAiColumnWorker, it never waits for a model
// service: a model call takes as long as the endpoint takes.
class ObAiColumnTaskScan : public common::ObTimerTask
{
public:
  ObAiColumnTaskScan() : worker_(nullptr), is_inited_(false) {}
  virtual ~ObAiColumnTaskScan() {}
  int init(ObAiColumnWorker *worker);
  virtual void runTimerTask() override;
private:
  ObAiColumnWorker *worker_;
  bool is_inited_;
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnTaskScan);
};

// Server wide worker of the ai columns of every table. It owns the scan timer and the thread
// pool that talks to the model services, so that the scan never blocks on an http call. The
// worker runs its own transaction on every row version it processes: it never reads the task
// of an uncommitted statement and it never writes through a user session.
class ObAiColumnWorker : public common::ObSimpleThreadPool
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker();

  int init();
  int start();
  void stop();
  void wait();
  void destroy();

  // Read the eligible task rows and hand every row version to the thread pool. Runs on the
  // timer thread and on nothing else.
  int scan_tasks();

  virtual void handle(void *task) override;
  virtual void handle_drop(void *task) override;

  // A statement inserts a task row and expects the value to follow within a couple of seconds,
  // so the task table is polled: nothing signals the worker that a task was committed. The
  // interval has to be short enough that a task row is seen before a following statement of the
  // same session replaces it (a test updates the row 300ms after the insert and expects the
  // worker to have asked the model for the first version by then), so it is 100ms.
  static const int64_t SCAN_INTERVAL_US = 100 * 1000L;
  static const int64_t SCAN_BATCH_SIZE = 256;
  static const int64_t MAX_QUEUED_GROUPS = 256;
  static const int64_t MIN_THREAD_COUNT = 2;
  static const int64_t MAX_THREAD_COUNT = 4;
  // A failed generation is retried with an exponential back off, the first retry after a
  // second, so that a task that failed once stays observable as state 3 for a while.
  static const int64_t RETRY_BACKOFF_BASE_SEC = 1;
  static const int64_t RETRY_BACKOFF_MAX_SEC = 30;

private:
  int scan_tasks_(common::ObIAllocator &allocator);
  // Resolve the table and the row key of one row version. The schema guard has to outlive the
  // call, the table schema it returns belongs to the guard.
  int build_group(share::schema::ObSchemaGetterGuard &guard,
                  const uint64_t table_id,
                  const int64_t request_id,
                  const ObString &rowkey_predicate,
                  ObAiColumnTaskGroup &group,
                  const share::schema::ObTableSchema *&table_schema);
  int build_item(const share::schema::ObTableSchema &table_schema,
                 const uint64_t column_id,
                 const int64_t retry_count,
                 ObAiColumnTaskGroup &group,
                 ObAiColumnTaskItem &item);
  // Ask the model service for every ai column of the row version and write the values.
  int process_group(ObAiColumnTaskGroup &group);
  // Ask the model service for the value of one ai column of the row version.
  int evaluate_item(ObAiColumnTaskGroup &group, ObAiColumnTaskItem &item);
  // Write every value of the row version, set its state and drop its task rows. Does nothing
  // when a newer request of the row replaced the task rows in the meantime. A row version
  // whose row is gone only has its task rows dropped.
  int write_group(ObAiColumnTaskGroup &group, const bool row_missing);
  // Publish the failure of the row version: bump the retry count of its task rows and let
  // them wait for the back off, or drop them when the ai column ran out of retries.
  int retry_group(ObAiColumnTaskGroup &group);
  // Drop the task rows of a row version whose table or column does not exist any more. They can
  // never be generated, and the scan would read them again and again, ahead of every other task.
  int delete_orphan_tasks(const uint64_t table_id, const int64_t request_id);

  // The task rows stay in the table until the value is written, so a scan keeps seeing a row
  // version that is already being generated and must not queue it twice.
  bool is_group_in_flight(const uint64_t table_id,
                          const int64_t request_id,
                          const ObString &rowkey_predicate);
  int add_group_in_flight(ObAiColumnTaskGroup *group);
  void remove_group_in_flight(ObAiColumnTaskGroup *group);
  void free_group(ObAiColumnTaskGroup *group);

private:
  bool is_inited_;
  // the one inner connection (and session) every statement of the worker runs on
  ObAiColumnInnerSql inner_sql_;
  common::ObTimer timer_;
  ObAiColumnTaskScan scan_task_;
  common::ObSEArray<ObAiColumnTaskGroup *, 16> in_flight_;
  common::ObSpinLock in_flight_lock_;
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorker);
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_AI_COLUMN_OB_AI_COLUMN_WORKER_H_
