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
#include "observer/virtual_table/ob_all_virtual_ai_column.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"

using namespace oceanbase::common;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

namespace oceanbase
{
namespace observer
{

ObAllVirtualAiColumn::ObAllVirtualAiColumn()
    : ObVirtualTableIterator(),
      items_(),
      item_idx_(0)
{
}

ObAllVirtualAiColumn::~ObAllVirtualAiColumn()
{
}

void ObAllVirtualAiColumn::reset()
{
  items_.reset();
  item_idx_ = 0;
  ObVirtualTableIterator::reset();
}

int ObAllVirtualAiColumn::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(NULL == schema_guard_)) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "schema guard is null", K(ret));
  } else if (OB_FAIL(collect_ai_columns_())) {
    SERVER_LOG(WARN, "fail to collect the ai columns", K(ret));
  }
  return ret;
}

int ObAllVirtualAiColumn::collect_ai_columns_()
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObTableSchema *, 64> table_schemas;
  if (OB_FAIL(schema_guard_->get_table_schemas_in_runtime(table_schemas))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schemas.count(); ++i) {
    const ObTableSchema *table_schema = table_schemas.at(i);
    if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      SERVER_LOG(WARN, "table schema is null", K(ret));
    }
    for (ObTableSchema::const_column_iterator iter = table_schema->column_begin();
         OB_SUCC(ret) && iter != table_schema->column_end(); ++iter) {
      const ObColumnSchemaV2 *column = *iter;
      if (OB_ISNULL(column)) {
        ret = OB_ERR_UNEXPECTED;
        SERVER_LOG(WARN, "column schema is null", K(ret));
      } else if (!column->is_ai_column()) {
        // not an ai column
      } else if (OB_FAIL(add_ai_column_(*table_schema, *column))) {
      }
    }
  }
  return ret;
}

int ObAllVirtualAiColumn::get_hidden_column_id_(const ObTableSchema &table_schema,
                                                const ObAiColumnSpec::HiddenColumnType type,
                                                const ObString &ai_column_name,
                                                uint64_t &column_id)
{
  int ret = OB_SUCCESS;
  char name_buf[OB_MAX_COLUMN_NAME_LENGTH];
  ObString hidden_column_name;
  const ObColumnSchemaV2 *hidden_column = NULL;
  column_id = OB_INVALID_ID;
  if (OB_FAIL(ObAiColumnSpec::build_hidden_column_name(
                type, ai_column_name, name_buf, sizeof(name_buf), hidden_column_name))) {
  } else if (NULL == (hidden_column = table_schema.get_column_schema(hidden_column_name))) {
    // the hidden column is not materialized, leave the id unset
  } else {
    column_id = hidden_column->get_column_id();
  }
  return ret;
}

int ObAllVirtualAiColumn::add_ai_column_(const ObTableSchema &table_schema,
                                         const ObColumnSchemaV2 &ai_column)
{
  int ret = OB_SUCCESS;
  AiColumnItem item;
  if (OB_FAIL(ObAiColumnSpec::load_from_ai_column(table_schema, ai_column, item.spec_))) {
  } else {
    item.table_id_ = table_schema.get_table_id();
    item.column_id_ = ai_column.get_column_id();
    item.column_name_ = ai_column.get_column_name_str();
    if (OB_FAIL(get_hidden_column_id_(table_schema, ObAiColumnSpec::HIDDEN_STATE_COLUMN,
                                      item.column_name_, item.state_column_id_))) {
    } else if (OB_FAIL(get_hidden_column_id_(table_schema, ObAiColumnSpec::HIDDEN_REQUEST_COLUMN,
                                             item.column_name_, item.request_column_id_))) {
    } else if (OB_FAIL(get_hidden_column_id_(table_schema, ObAiColumnSpec::HIDDEN_RESULT_SPEC_COLUMN,
                                             item.column_name_, item.result_spec_column_id_))) {
    } else if (OB_FAIL(items_.push_back(item))) {
    }
  }
  return ret;
}

int ObAllVirtualAiColumn::inner_get_next_row(ObNewRow *&row)
{
  int ret = OB_SUCCESS;
  row = NULL;
  if (OB_ISNULL(allocator_) || OB_ISNULL(cur_row_.cells_)) {
    ret = OB_NOT_INIT;
    SERVER_LOG(WARN, "data member is not inited", K(ret), KP(allocator_), KP(cur_row_.cells_));
  } else if (item_idx_ >= items_.count()) {
    ret = OB_ITER_END;
    item_idx_ = 0;
  } else {
    const AiColumnItem &item = items_.at(item_idx_);
    const int64_t col_count = output_column_ids_.count();
    for (int64_t i = 0; OB_SUCC(ret) && i < col_count; ++i) {
      ObObj &cell = cur_row_.cells_[i];
      switch (output_column_ids_.at(i)) {
        case TABLE_ID: {
          cell.set_int(static_cast<int64_t>(item.table_id_));
          break;
        }
        case COLUMN_ID: {
          cell.set_int(static_cast<int64_t>(item.column_id_));
          break;
        }
        case COLUMN_NAME: {
          cell.set_varchar(item.column_name_);
          break;
        }
        case EXPR: {
          cell.set_varchar(item.spec_.get_expr());
          break;
        }
        case NORMALIZED_EXPR: {
          cell.set_varchar(item.spec_.get_normalized_expr());
          break;
        }
        case SYNC_MODE: {
          cell.set_varchar(item.spec_.get_sync_mode());
          break;
        }
        case MAX_RETRIES: {
          cell.set_int(item.spec_.get_max_retries());
          break;
        }
        case STATE_COLUMN_ID: {
          cell.set_int(static_cast<int64_t>(item.state_column_id_));
          break;
        }
        case REQUEST_COLUMN_ID: {
          cell.set_int(static_cast<int64_t>(item.request_column_id_));
          break;
        }
        case RESULT_SPEC_COLUMN_ID: {
          cell.set_int(static_cast<int64_t>(item.result_spec_column_id_));
          break;
        }
        default: {
          ret = OB_ERR_UNEXPECTED;
          SERVER_LOG(WARN, "invalid column id", K(ret), K(output_column_ids_.at(i)));
          break;
        }
      }
      if (OB_SUCC(ret) && cell.is_varchar_or_char()) {
        cell.set_collation_type(ObCharset::get_default_collation(ObCharset::get_default_charset()));
      }
    }
    if (OB_SUCC(ret)) {
      ++item_idx_;
      row = &cur_row_;
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase
