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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

namespace
{

// The provider answers with plain json numbers, an integer score such as 1 must not be
// reinterpreted as a double, so read every number through its real json type.
int read_json_int(const ObJsonNode *node, int64_t &value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObJsonNodeType::J_INT == node->json_type()) {
    value = static_cast<const ObJsonInt *>(node)->value();
  } else if (ObJsonNodeType::J_UINT == node->json_type()) {
    value = static_cast<int64_t>(static_cast<const ObJsonUint *>(node)->value());
  } else if (ObJsonNodeType::J_DOUBLE == node->json_type()) {
    value = static_cast<int64_t>(static_cast<const ObJsonDouble *>(node)->value());
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

int read_json_double(const ObJsonNode *node, double &value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObJsonNodeType::J_DOUBLE == node->json_type()) {
    value = static_cast<const ObJsonDouble *>(node)->value();
  } else if (ObJsonNodeType::J_INT == node->json_type()) {
    value = static_cast<double>(static_cast<const ObJsonInt *>(node)->value());
  } else if (ObJsonNodeType::J_UINT == node->json_type()) {
    value = static_cast<double>(static_cast<const ObJsonUint *>(node)->value());
  } else {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

struct RerankResultItem
{
  double score;
  int64_t index;
  ObJsonObject *node;
};

bool rerank_item_before(const RerankResultItem &lhs, const RerankResultItem &rhs)
{
  // higher score first, equal scores keep the original document order
  if (lhs.score != rhs.score) {
    return lhs.score > rhs.score;
  }
  return lhs.index < rhs.index;
}

// Rebuild the json array in a deterministic order: relevance score descending and, for
// equal scores, the document index ascending.
int sort_rerank_result_array(ObIAllocator &allocator, ObJsonArray *array, ObJsonArray *&sorted)
{
  int ret = OB_SUCCESS;
  ObArray<RerankResultItem> items;
  ObJsonArray *out = NULL;
  if (OB_ISNULL(array)) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < array->element_count(); ++i) {
    ObJsonObject *obj = static_cast<ObJsonObject *>(array->get_value(i));
    RerankResultItem item;
    item.node = obj;
    item.score = 0.0;
    item.index = 0;
    if (OB_ISNULL(obj)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(read_json_double(obj->get_value(ObString("relevance_score")), item.score))) {
    } else if (OB_FAIL(read_json_int(obj->get_value(ObString("index")), item.index))) {
    } else if (OB_FAIL(items.push_back(item))) {
    }
  }
  // insertion sort over a small result set, keeps equal scores stable
  for (int64_t i = 1; OB_SUCC(ret) && i < items.count(); ++i) {
    RerankResultItem key = items.at(i);
    int64_t j = i - 1;
    while (j >= 0 && rerank_item_before(key, items.at(j))) {
      items.at(j + 1) = items.at(j);
      --j;
    }
    items.at(j + 1) = key;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, out))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < items.count(); ++i) {
      if (OB_FAIL(out->append(items.at(i).node))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted = out;
  }
  return ret;
}

} // anonymous namespace

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    int64_t batch_size = 20; // max batch size for rerank
    bool is_null_result = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      // A NULL or empty doc_key means "the documents are plain strings": the fourth
      // argument is often written as a literal `null`, which arrives as a NULL datum
      // rather than a missing argument.
      const bool has_doc_key = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null()
                               && !arg_doc_key->get_string().empty();
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }
    
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } 

    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null()
               && !arg_doc_key->get_string().empty()) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(eval_ai_rerank_with_doc_key(expr, ctx, temp_allocator, model_id,
                  query, document_array, doc_key, *info, *endpoint_info, res))) {
      }
    } else {
      if (OB_ISNULL(info)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
      } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, result_array))) {
      }

      if (OB_SUCC(ret)) {
        ObString score_key(SCORE_KEY);
        int64_t end_idx = 0;
        ObJsonArray *compact_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        ObJsonArray *batch_document_array = nullptr;
        for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i += batch_size) {
          end_idx = i + batch_size;
          if (end_idx > document_array->element_count()) {
            end_idx = document_array->element_count();
          }
          if (OB_FAIL(construct_batch_document_array(temp_allocator, document_array, i, end_idx, batch_document_array))) {
          } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, *endpoint_info, header_array, query, batch_document_array, batch_result_array))) {
          } else if (OB_FAIL(batch_result_add_base(temp_allocator, batch_result_array, i))) {
          } else if (OB_FAIL(compact_json_array_by_key(temp_allocator, result_array, batch_result_array, score_key, compact_array))) {
          } else {
            result_array = compact_array;
          }
        }
      }
      if (OB_SUCC(ret)) {
        ObString raw_str;
        if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator, 
                                                ObString& model_id, ObString& query, ObJsonArray *document_array,
                                                ObString& doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info, ObDatum &res)
{
  INIT_SUCC(ret);
  ObJsonArray *doc_array = nullptr;
  ObJsonArray *result_array = nullptr;
  ObJsonArray *sorted_document_array = nullptr;
  ObArray<ObString> header_array;
  if (OB_FAIL(get_doc_array_from_documents_array_with_key(allocator, document_array, doc_key, doc_array))) {
  } else if (OB_FAIL(ObAIFuncUtils::get_header(allocator, info, endpoint_info, header_array))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    // Send the selected document texts in batches as well, a single request with every
    // document would hit the provider request limit on large inputs.
    const int64_t batch_size = 20;
    const int64_t doc_count = doc_array->element_count();
    ObString score_key(SCORE_KEY);
    ObJsonArray *batch_document_array = nullptr;
    ObJsonArray *batch_result_array = nullptr;
    ObJsonArray *compact_array = nullptr;
    for (int64_t i = 0; OB_SUCC(ret) && i < doc_count; i += batch_size) {
      const int64_t end_idx = (i + batch_size > doc_count) ? doc_count : (i + batch_size);
      if (OB_FAIL(construct_batch_document_array(allocator, doc_array, i, end_idx, batch_document_array))) {
      } else if (OB_FAIL(inner_eval_ai_rerank(allocator, info, endpoint_info, header_array,
                                              query, batch_document_array, batch_result_array))) {
      } else if (OB_FAIL(batch_result_add_base(allocator, batch_result_array, i))) {
      } else if (OB_FAIL(compact_json_array_by_key(allocator, result_array, batch_result_array,
                                                   score_key, compact_array))) {
      } else {
        result_array = compact_array;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sort_document_array_by_model_result(allocator, document_array,
                                                         result_array, sorted_document_array))) {
  }
  if (OB_SUCC(ret)) {
    ObString raw_str;
    if (OB_FAIL(sorted_document_array->get_raw_binary(raw_str, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (ObJsonNodeType::J_STRING == doc_value_node->json_type()) {
            if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
            }
          } else if (ObJsonNodeType::J_OBJECT == doc_value_node->json_type()
                     || ObJsonNodeType::J_ARRAY == doc_value_node->json_type()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else {
            // numeric and boolean values are accepted as well, they are rendered as text
            ObStringBuffer value_buf(&allocator);
            ObJsonString *value_str = NULL;
            ObString value_text;
            if (OB_FAIL(doc_value_node->print(value_buf, 0))) {
            } else if (FALSE_IT(value_text = value_buf.string())) {
            } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_string(allocator,
                           value_text, value_str))) {
            } else if (OB_FAIL(doc_array_obj->append(value_str))) {
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator, ObJsonArray *document_array, ObJsonArray *model_result_array, ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted_document_array_obj = nullptr;
  const int64_t doc_count = (NULL == document_array) ? 0 : document_array->element_count();
  if (OB_ISNULL(document_array) || OB_ISNULL(model_result_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_document_array_obj))) {
  } else {
    // model_result_array is already ordered by score, map each result back to its
    // document through the global index and carry both the score and the index over.
    const int64_t count = model_result_array->element_count();
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonObject *model_result_obj = static_cast<ObJsonObject *>(model_result_array->get_value(i));
      int64_t index = 0;
      double score = 0.0;
      if (OB_ISNULL(model_result_obj)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(read_json_int(model_result_obj->get_value(ObString(INDEX_KEY)), index))) {
      } else if (OB_FAIL(read_json_double(model_result_obj->get_value(ObString(SCORE_KEY)), score))) {
      } else if (index < 0 || index >= doc_count) {
        // the model referred to a document that was not sent, drop the entry
        LOG_WARN("rerank result index out of range", K(index), K(doc_count));
        ret = OB_SUCCESS;
      } else {
        ObJsonObject *origin_obj = static_cast<ObJsonObject *>(document_array->get_value(index));
        ObJsonObject *new_obj = nullptr;
        ObJsonInt *index_json = nullptr;
        ObJsonDouble *score_json = nullptr;
        if (OB_ISNULL(origin_obj)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(origin_obj->clone(&allocator, true)))) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, index, index_json))) {
        } else if (OB_ISNULL(score_json = OB_NEWx(ObJsonDouble, &allocator, score))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(new_obj->add(ObString(INDEX_KEY), index_json))) {
        } else if (OB_FAIL(new_obj->add(ObString(MODEL_SCORE_KEY), score_json))) {
        } else if (OB_FAIL(sorted_document_array_obj->append(new_obj))) {
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = sorted_document_array_obj;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator, 
                                          const ObAIFuncExprInfo &info, 
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array, 
                                          ObString &query, 
                                          ObJsonArray *document_array, 
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator, ObJsonArray *array, int64_t start_idx)
{
  INIT_SUCC(ret);
  int64_t count = array->element_count();
  ObString index_key(INDEX_KEY);
  for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
    ObJsonObject *obj = static_cast<ObJsonObject *>(array->get_value(i));
    int64_t local_idx = 0;
    if (OB_ISNULL(obj)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(read_json_int(obj->get_value(index_key), local_idx))) {
    } else if (local_idx < 0) {
      ret = OB_INVALID_DATA;
    } else {
      // shift the batch local index to the global document position
      ObJsonInt *index_node = static_cast<ObJsonInt *>(obj->get_value(index_key));
      if (OB_NOT_NULL(index_node) && ObJsonNodeType::J_INT == index_node->json_type()) {
        index_node->set_value(local_idx + start_idx);
      } else {
        ObJsonInt *new_index = NULL;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, local_idx + start_idx, new_index))) {
        } else if (OB_FAIL(obj->add(index_key, new_index))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::compact_json_array_by_key(ObIAllocator &allocator, ObJsonArray *array1, ObJsonArray *array2, ObString &score_key, ObJsonArray *&compact_array)
{
  INIT_SUCC(ret);
  UNUSED(score_key);
  ObJsonArray *merged = NULL;
  ObJsonArray *sorted = NULL;
  // Merge every batch result and order the whole set once: score descending and, for
  // equal scores, the original document index ascending. The previous incremental merge
  // only compared the head of each batch and produced an order that depended on how the
  // documents happened to be split into batches.
  if (OB_ISNULL(array1) || OB_ISNULL(array2)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, merged))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < array1->element_count(); ++i) {
      if (OB_FAIL(merged->append(array1->get_value(i)))) {
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < array2->element_count(); ++i) {
      if (OB_FAIL(merged->append(array2->get_value(i)))) {
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sort_rerank_result_array(allocator, merged, sorted))) {
  } else {
    compact_array = sorted;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
