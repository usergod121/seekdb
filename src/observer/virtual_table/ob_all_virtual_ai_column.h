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

#ifndef OCEANBASE_OBSERVER_VIRTUAL_TABLE_ALL_VIRTUAL_AI_COLUMN_
#define OCEANBASE_OBSERVER_VIRTUAL_TABLE_ALL_VIRTUAL_AI_COLUMN_

#include "lib/container/ob_se_array.h"
#include "share/ai_service/ob_ai_service_struct.h"
#include "observer/virtual_table/ob_virtual_table_iterator.h"

namespace oceanbase
{
namespace observer
{
// Every ai column of the resolved table schemas, together with the ai column definition
// stored in the column schema and the ids of its hidden columns.
class ObAllVirtualAiColumn : public common::ObVirtualTableIterator
{
  enum COLUMN_ID {
    TABLE_ID = common::OB_APP_MIN_COLUMN_ID,
    COLUMN_ID,
    COLUMN_NAME,
    EXPR,
    NORMALIZED_EXPR,
    SYNC_MODE,
    MAX_RETRIES,
    STATE_COLUMN_ID,
    REQUEST_COLUMN_ID,
    RESULT_SPEC_COLUMN_ID,
  };
public:
  ObAllVirtualAiColumn();
  virtual ~ObAllVirtualAiColumn();
  virtual int inner_open() override;
  virtual int inner_get_next_row(common::ObNewRow *&row) override;
  virtual void reset() override;
private:
  struct AiColumnItem
  {
    AiColumnItem() { reset(); }
    void reset()
    {
      spec_.reset();
      table_id_ = common::OB_INVALID_ID;
      column_id_ = common::OB_INVALID_ID;
      column_name_.reset();
      state_column_id_ = common::OB_INVALID_ID;
      request_column_id_ = common::OB_INVALID_ID;
      result_spec_column_id_ = common::OB_INVALID_ID;
    }
    share::ObAiColumnSpec spec_;
    uint64_t table_id_;
    uint64_t column_id_;
    common::ObString column_name_;
    uint64_t state_column_id_;
    uint64_t request_column_id_;
    uint64_t result_spec_column_id_;
  };
  int collect_ai_columns_();
  int add_ai_column_(const share::schema::ObTableSchema &table_schema,
                     const share::schema::ObColumnSchemaV2 &ai_column);
  int get_hidden_column_id_(const share::schema::ObTableSchema &table_schema,
                            const share::ObAiColumnSpec::HiddenColumnType type,
                            const common::ObString &ai_column_name,
                            uint64_t &column_id);
private:
  common::ObSEArray<AiColumnItem, 8> items_;
  int64_t item_idx_;
  DISALLOW_COPY_AND_ASSIGN(ObAllVirtualAiColumn);
};
} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_VIRTUAL_TABLE_ALL_VIRTUAL_AI_COLUMN_
