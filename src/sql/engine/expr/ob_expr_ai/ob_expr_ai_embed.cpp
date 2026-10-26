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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

namespace
{

bool is_json_number_type(const ObJsonNodeType type)
{
  return ObJsonNodeType::J_INT == type
         || ObJsonNodeType::J_UINT == type
         || ObJsonNodeType::J_DOUBLE == type
         || ObJsonNodeType::J_DECIMAL == type
         || ObJsonNodeType::J_OFLOAT == type
         || ObJsonNodeType::J_ODOUBLE == type
         || ObJsonNodeType::J_ODECIMAL == type
         || ObJsonNodeType::J_OINT == type
         || ObJsonNodeType::J_OLONG == type;
}

// The embedding endpoint has to answer with a non empty json array of numbers. When the
// caller asked for a dimension the vector length has to match it as well.
int check_embedding_vector(ObIAllocator &allocator,
                           const ObString &result,
                           const int64_t expect_dim)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *j_base = NULL;
  if (OB_FAIL(ObJsonBaseFactory::get_json_base(&allocator, result,
                                               ObJsonInType::JSON_TREE,
                                               ObJsonInType::JSON_TREE, j_base))) {
  } else if (OB_ISNULL(j_base) || ObJsonNodeType::J_ARRAY != j_base->json_type()) {
    ret = OB_INVALID_DATA;
  } else {
    ObJsonArray *vector = static_cast<ObJsonArray *>(j_base);
    const int64_t count = vector->element_count();
    if (count <= 0) {
      ret = OB_INVALID_DATA;
    } else if (expect_dim > 0 && expect_dim != count) {
      ret = OB_INVALID_DATA;
      LOG_USER_ERROR(OB_INVALID_DATA, "ai_embed, the returned vector length does not match dimension");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        ObJsonNode *node = vector->get_value(i);
        if (OB_ISNULL(node)) {
          ret = OB_INVALID_DATA;
        } else if (!is_json_number_type(node->json_type())) {
          ret = OB_INVALID_DATA;
        }
      }
    }
  }
  return ret;
}

} // anonymous namespace

ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null() || arg_content->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or content is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(check_embedding_vector(temp_allocator, result, dim))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip, const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));

  // A single request carries many rows, but not the whole batch: providers limit both
  // the number of inputs and the response size (one vector is ~4*dim bytes).
  const int64_t MAX_INPUTS_PER_REQUEST = 32;

  ObDatum *results = expr.locate_batch_datums(ctx);
  ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
  ObBitVector &my_skip = expr.get_pvt_skip(ctx);
  my_skip.deep_copy(skip, batch_size);

  if (OB_ISNULL(results)) {
    ret = OB_ERR_UNEXPECTED;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < expr.arg_cnt_; ++i) {
    if (OB_FAIL(expr.args_[i]->eval_batch(ctx, my_skip, batch_size))) {
    }
  }

  ObDatum *model_datums = NULL;
  ObDatum *content_datums = NULL;
  ObDatum *dim_datums = NULL;
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(model_datums = expr.args_[0]->locate_batch_datums(ctx))
             || OB_ISNULL(content_datums = expr.args_[1]->locate_batch_datums(ctx))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (expr.arg_cnt_ > 2 && OB_NOT_NULL(expr.args_[2])) {
    dim_datums = expr.args_[2]->locate_batch_datums(ctx);
  }

  // Collect the rows that really need a vector. Rows filtered out by the query, rows
  // whose input is NULL or empty keep a NULL result and are not sent to the provider.
  ObArray<int64_t> row_ids;
  ObArray<ObString> contents;
  ObString model_id;
  int64_t dim = 0;
  for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; ++j) {
    ObString content;
    if (my_skip.at(j) || eval_flags.at(j)) {
      continue;
    } else if (model_datums[j].is_null() || content_datums[j].is_null()) {
      results[j].set_null();
      eval_flags.set(j);
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(ctx.exec_ctx_, temp_allocator,
                       content_datums[j], expr.args_[1]->datum_meta_,
                       expr.args_[1]->obj_meta_.has_lob_header(), content))) {
    } else if (content.empty()) {
      results[j].set_null();
      eval_flags.set(j);
    } else {
      if (model_id.empty()) {
        model_id = model_datums[j].get_string();
        if (OB_NOT_NULL(dim_datums) && !dim_datums[j].is_null()) {
          dim = dim_datums[j].get_int();
        }
      }
      if (OB_FAIL(row_ids.push_back(j))) {
      } else if (OB_FAIL(contents.push_back(content))) {
      }
    }
  }

  ObAIFuncExprInfo *info = NULL;
  share::ObAiModelEndpointInfo resolved_endpoint;
  query::ObIAiEndpointResolver *endpoint_resolver = NULL;
  if (OB_FAIL(ret) || contents.empty()) {
  } else if (model_id.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (dim < 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
  } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
  } else if (OB_ISNULL(endpoint_resolver =
             ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(model_id, temp_allocator,
                                                              resolved_endpoint))) {
  } else {
    ObJsonObject *config = NULL;
    if (dim > 0) {
      ObJsonInt *dim_json = NULL;
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_SUCC(ret)) {
      ObAIFuncModel model(temp_allocator, *info, resolved_endpoint);
      for (int64_t start = 0; OB_SUCC(ret) && start < contents.count();
           start += MAX_INPUTS_PER_REQUEST) {
        const int64_t left = contents.count() - start;
        const int64_t end = (left > MAX_INPUTS_PER_REQUEST) ? start + MAX_INPUTS_PER_REQUEST
                                                            : contents.count();
        ObArray<ObString> chunk_contents;
        ObArray<ObString> chunk_results;
        for (int64_t i = start; OB_SUCC(ret) && i < end; ++i) {
          if (OB_FAIL(chunk_contents.push_back(contents.at(i)))) {
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(model.call_dense_embedding_vector_v2(chunk_contents, config,
                                                                chunk_results))) {
        } else if (chunk_results.count() != chunk_contents.count()) {
          // the provider must answer one vector per input
          ret = OB_ERR_UNEXPECTED;
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < chunk_results.count(); ++i) {
            const int64_t row = row_ids.at(start + i);
            ObString &vector_str = chunk_results.at(i);
            ObDatum &res_datum = results[row];
            ObTextStringDatumResult text_result(expr.datum_meta_.type_, &expr, &ctx, &res_datum);
            if (OB_FAIL(check_embedding_vector(temp_allocator, vector_str, dim))) {
            } else if (OB_FAIL(text_result.init_with_batch_idx(vector_str.length(), row))) {
            } else if (OB_FAIL(text_result.append(vector_str))) {
            } else {
              text_result.set_result();
              eval_flags.set(row);
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   ObObj const_data;
  //   bool got_data = false;
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
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    // Rows scanned together are embedded with a single provider request.
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
