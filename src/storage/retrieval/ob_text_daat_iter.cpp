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

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

#ifndef OB_FTS_BATCH_EXISTENCE_OUTPUT
#define OB_FTS_BATCH_EXISTENCE_OUTPUT 1
#endif

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
  }
  return ret;
}

int ObTextDaaTIter::get_next_rows(const int64_t capacity, int64_t &count)
{
#if OB_FTS_BATCH_EXISTENCE_OUTPUT
  // This final class has no per-document processing hooks. Preserve the exact
  // sorted union and deep-copy IDs before advancing any posting cursor.
  if (IS_INIT && capacity > 0 && use_small_existence_merge_
      && !iter_param_->limit_param_->is_valid()
      && iter_param_->eval_ctx_->is_vectorized()
      && (!iter_param_->need_filter() || filter_is_relevance_gt_zero_)
      && iter_param_->id_proj_expr_->is_batch_result()
      && (!iter_param_->need_project_relevance()
          || iter_param_->relevance_proj_expr_->is_batch_result())) {
    int ret = OB_SUCCESS;
    count = 0;
    const int64_t safe_capacity = MIN(capacity, iter_param_->max_batch_size_);
    if (OB_UNLIKELY(safe_capacity > iter_param_->eval_ctx_->max_batch_size_
                   || safe_capacity > buffered_domain_ids_.count())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(pre_process())) {
    } else {
      const int64_t dimension_count = dim_iters_->count();
      ObTextRetrievalDaaTTokenIter *tokens[4];
      for (int64_t i = 0; i < dimension_count; ++i) {
        // init/reset of this final text iterator already require these types.
        tokens[i] = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i));
      }
      const bool integer_id = iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObUInt64Type;
      while (OB_SUCC(ret) && count < safe_capacity) {
        for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
          const int64_t index = next_round_iter_idxes_[i];
          ObTextRetrievalDaaTTokenIter *token = tokens[index];
          if (OB_FAIL(token->ObTextRetrievalDaaTTokenIter::get_next_row())) {
            if (OB_ITER_END == ret) {
              iter_domain_ids_[index] = nullptr;
              ret = OB_SUCCESS;
            }
          } else if (OB_FAIL(token->ObTextRetrievalDaaTTokenIter::get_curr_id(iter_domain_ids_[index]))) {
          } else if (OB_ISNULL(iter_domain_ids_[index])) {
            ret = OB_ERR_UNEXPECTED;
          }
        }
        int64_t minimum = -1;
        if (OB_SUCC(ret)) {
          next_round_cnt_ = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i < dimension_count; ++i) {
            const ObDatum *head = iter_domain_ids_[i];
            if (nullptr != head) {
              int comparison = 0;
              if (minimum < 0) {
                minimum = i;
                next_round_iter_idxes_[next_round_cnt_++] = i;
              } else {
                const ObDatum &other = *iter_domain_ids_[minimum];
                if (integer_id && !head->is_null() && !other.is_null()) {
                  const uint64_t a = head->get_uint();
                  const uint64_t b = other.get_uint();
                  comparison = (a > b) - (a < b);
                } else if (OB_FAIL(merge_cmp_.cmp_ids(*head, other, comparison))) {
                }
                if (OB_SUCC(ret) && comparison < 0) {
                  minimum = i;
                  next_round_cnt_ = 1;
                  next_round_iter_idxes_[0] = i;
                } else if (OB_SUCC(ret) && comparison == 0) {
                  next_round_iter_idxes_[next_round_cnt_++] = i;
                }
              }
            }
          }
        }
        if (OB_SUCC(ret)) {
          if (minimum < 0) {
            ret = OB_ITER_END;
          } else if (OB_FAIL(buffered_domain_ids_[count].from_datum(*iter_domain_ids_[minimum]))) {
          } else {
            ++count;
            ++input_row_cnt_;
            ++output_row_cnt_;
          }
        }
      }
      if ((OB_SUCCESS == ret || OB_ITER_END == ret) && count > 0) {
        sql::ObEvalCtx &ctx = *iter_param_->eval_ctx_;
        sql::ObExpr &id_expr = *iter_param_->id_proj_expr_;
        ObDatum *id_datums = id_expr.locate_datums_for_update(ctx, count);
        sql::ObBitVector &id_flags = id_expr.get_evaluated_flags(ctx);
        for (int64_t i = 0; i < count; ++i) {
          set_datum_func_(id_datums[i], buffered_domain_ids_[i]);
          id_flags.set(i);
        }
        id_expr.set_evaluated_projected(ctx);
        if (iter_param_->need_project_relevance()) {
          sql::ObExpr &relevance_expr = *iter_param_->relevance_proj_expr_;
          ObDatum *relevances = relevance_expr.locate_datums_for_update(ctx, count);
          sql::ObBitVector &flags = relevance_expr.get_evaluated_flags(ctx);
          for (int64_t i = 0; i < count; ++i) {
            relevances[i].set_double(1.0);
            flags.set(i);
          }
          relevance_expr.set_evaluated_projected(ctx);
        }
      }
    }
    return ret;
  }
#endif
  return ObSRDaaTIterImpl::get_next_rows(capacity, count);
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance() && !iter_param_->bool_relevance_only_) {
    // The only consumer of the estimated parameters is the BM25 expression, and a boolean relevance
    // never evaluates it (the token iterators skip score computation for that case), so the
    // estimate - which scans the skip index of the whole inverted index once per query - would be
    // work whose result is never read. There is no consumer of the estimated parameter here.
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
