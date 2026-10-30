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

#include "ob_sparse_daat_iter.h"

#ifndef OB_FTS_EXISTENCE_ONLY_DAAT
#define OB_FTS_EXISTENCE_ONLY_DAAT 1
#endif

#ifndef OB_FTS_DAAT_IMPLICIT_BOOL_SCORE
#define OB_FTS_DAAT_IMPLICIT_BOOL_SCORE 1
#endif

#ifndef OB_FTS_SMALL_EXISTENCE_MERGE
#define OB_FTS_SMALL_EXISTENCE_MERGE 1
#endif

namespace oceanbase
{
namespace storage
{

ObSRMergeCmp::ObSRMergeCmp()
  : cmp_func_(nullptr),
    iter_ids_(nullptr),
    is_inited_(false)
{
}

int ObSRMergeCmp::init(
    sql::ObDatumMeta id_meta,
    const ObFixedArray<const ObDatum *, ObIAllocator> *iter_ids)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(iter_ids)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    iter_ids_ = iter_ids;
    common::ObDatumBasicFuncs *basic_funcs = ObDatumFuncs::get_basic_func(id_meta.type_, id_meta.cs_type_);
    cmp_func_ = basic_funcs->null_first_cmp_;
    if (OB_ISNULL(cmp_func_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

int ObSRMergeCmp::cmp(
    const ObSRMergeItem &l,
    const ObSRMergeItem &r,
    int64_t &cmp_ret)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else {
    int tmp_ret = 0;
    if (OB_FAIL(cmp_func_(
            get_id_datum(l.iter_idx_), get_id_datum(r.iter_idx_), tmp_ret, nullptr))) {
    } else {
      cmp_ret = tmp_ret;
    }
  }
  return ret;
}

ObSRDaaTIterImpl::ObSRDaaTIterImpl()
  : ObISparseRetrievalMergeIter(),
    iter_allocator_(nullptr),
    iter_param_(nullptr),
    dim_iters_(nullptr),
    merge_cmp_(),
    merge_heap_(nullptr),
    relevance_collector_(nullptr),
    iter_domain_ids_(),
    buffered_domain_ids_(),
    buffered_relevances_(),
    next_round_iter_idxes_(),
    next_round_cnt_(0),
    set_datum_func_(nullptr),
    filter_is_relevance_gt_zero_(false),
    use_small_existence_merge_(false)
{
}

int ObSRDaaTIterImpl::init(
    ObSparseRetrievalMergeParam &iter_param,
    ObIArray<ObISRDaaTDimIter *> &dim_iters,
    ObIAllocator &iter_allocator,
    ObSRDaaTRelevanceCollector &relevance_collector)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(iter_param.max_batch_size_ <= 0)) {
      ret = OB_ERR_UNEXPECTED;
  } else {
    iter_allocator_ = &iter_allocator;
    iter_param_ = &iter_param;
    dim_iters_ = &dim_iters;
    relevance_collector_ = &relevance_collector;
    const int64_t max_batch_size = OB_MAX(iter_param_->eval_ctx_->max_batch_size_, iter_param_->max_batch_size_);
    if (iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObUInt64Type) {
      set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_int;
    } else {
      set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_shallow;
    }
    if (OB_UNLIKELY(dim_iters.count() == 0)) {
    } else if (OB_NOT_NULL(iter_param_->dim_weights_) && dim_iters.count() != iter_param_->dim_weights_->count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (FALSE_IT(iter_domain_ids_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(iter_domain_ids_.init(dim_iters.count()))) {
    } else if (OB_FAIL(iter_domain_ids_.prepare_allocate(dim_iters.count()))) {
    } else if (FALSE_IT(next_round_iter_idxes_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(next_round_iter_idxes_.init(dim_iters.count()))) {
    } else if (OB_FAIL(next_round_iter_idxes_.prepare_allocate(dim_iters.count()))) {
    } else if (FALSE_IT(buffered_domain_ids_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(buffered_domain_ids_.init(max_batch_size))) {
    } else if (OB_FAIL(buffered_domain_ids_.prepare_allocate(max_batch_size))) {
    } else if (FALSE_IT(buffered_relevances_.set_allocator(iter_allocator_))) {
    } else if (OB_FAIL(buffered_relevances_.init(max_batch_size))) {
    } else if (OB_FAIL(buffered_relevances_.prepare_allocate(max_batch_size))) {
    } else if (OB_FAIL(merge_cmp_.init(iter_param_->id_proj_expr_->datum_meta_, &iter_domain_ids_))) {
    } else if (OB_FAIL(init_merge_heap(dim_iters_->count()))) {
    } else {
      for (int64_t i = 0; i < dim_iters.count(); ++i) {
        next_round_iter_idxes_[i] = i;
      }
      init_filter_shape();
      init_merge_strategy();
    }

    if (OB_SUCC(ret)) {
      next_round_cnt_ = dim_iters.count();
      input_row_cnt_ = 0;
      output_row_cnt_ = 0;
      is_inited_ = true;
    }
  }
  return ret;
}

void ObSRDaaTIterImpl::reset()
{
  if (OB_NOT_NULL(merge_heap_)) {
    merge_heap_->~ObSRMergeHeap();
    merge_heap_ = nullptr;
  }
  if (OB_NOT_NULL(relevance_collector_)) {
    relevance_collector_->reset();
    relevance_collector_ = nullptr;
  }
  iter_domain_ids_.reset();
  buffered_domain_ids_.reset();
  buffered_relevances_.reset();
  next_round_iter_idxes_.reset();
  next_round_cnt_ = 0;
  input_row_cnt_ = 0;
  output_row_cnt_ = 0;
  is_inited_ = false;
  use_small_existence_merge_ = false;
}

void ObSRDaaTIterImpl::reuse(const bool switch_tablet)
{
  if (OB_NOT_NULL(dim_iters_)) {
    if (OB_NOT_NULL(merge_heap_)) {
      merge_heap_->reuse();
      merge_heap_->open(dim_iters_->count());
    }
    next_round_cnt_ = dim_iters_->count();
    for (int64_t i = 0; i < next_round_cnt_; ++i) {
      next_round_iter_idxes_[i] = i;
    }
    init_merge_strategy();
  }
  if (OB_NOT_NULL(relevance_collector_)) {
    relevance_collector_->reuse();
  }
  input_row_cnt_ = 0;
  output_row_cnt_ = 0;
}

  int ObSRDaaTIterImpl::get_query_max_score(double &score)
  {
    int ret = OB_SUCCESS;
    score = 0.0;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
      ObISRDaaTDimIter *dim_iter = dim_iters_->at(i);
      double dim_score = 0.0;
      if (OB_ISNULL(dim_iter) || OB_ISNULL(iter_param_->dim_weights_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(dim_iter->get_dim_max_score(dim_score))) {
      } else {
        score += dim_score * iter_param_->field_boost_ * iter_param_->dim_weights_->at(i);
      }
    }

    return ret;
  }

int ObSRDaaTIterImpl::get_next_row()
{
  int ret = OB_SUCCESS;
  int64_t count = 0;
  if (OB_FAIL(get_next_rows(1, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else if (OB_UNLIKELY(count != 0)) {
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (OB_UNLIKELY(count != 1)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObSRDaaTIterImpl::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (iter_param_->limit_param_->is_valid() && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else {
      count = 0;
    }
  } else {
    count = 0;
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      if (OB_FAIL(do_one_merge_round(count))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        }
      }
    }

    if (OB_SUCC(ret) || OB_ITER_END == ret) {
      if (count > 0 && OB_FAIL(project_results(count))) {
      }
    }

  }
  return ret;
}

int ObSRDaaTIterImpl::pre_process()
{
  return OB_NOT_IMPLEMENT;
}

int ObSRDaaTIterImpl::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  bool need_project = true;
  double relevance = 0.0;
  const ObDatum *id_datum = nullptr;
  if (use_small_existence_merge_) {
    relevance = 1.0;
    if (OB_FAIL(next_small_existence_id(id_datum))) {
    } else if (OB_FAIL(process_collected_row(*id_datum, relevance))) {
    }
  } else if (OB_FAIL(fill_merge_heap())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  } else if (OB_FAIL(collect_dims_by_id(id_datum, relevance, need_project))) {
  }
  if (OB_SUCC(ret)) {
    if (need_project && OB_FAIL(filter_on_demand(count, relevance, need_project))) {
    } else if (need_project && OB_FAIL(cache_result(count, *id_datum, relevance))) {
      if (OB_ITER_END != ret) {
      }
    }
  }
  return ret;
}

void ObSRDaaTIterImpl::init_merge_strategy()
{
  use_small_existence_merge_ = false;
#if OB_FTS_SMALL_EXISTENCE_MERGE && OB_FTS_EXISTENCE_ONLY_DAAT
  // This is a small sorted-union operation, not ranked top-k retrieval.
  use_small_existence_merge_ = iter_param_->bool_relevance_only_
      && !iter_param_->need_pushdown_topk()
      && dim_iters_->count() > 0 && dim_iters_->count() <= 4;
#endif
}

int ObSRDaaTIterImpl::next_small_existence_id(const ObDatum *&id_datum)
{
  int ret = OB_SUCCESS;
  id_datum = nullptr;
  // Advance every cursor that produced the previous ID, including duplicates.
  // Other cursors retain their current head and its existing datum lifetime.
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t idx = next_round_iter_idxes_[i];
    ObISRDaaTDimIter *dim_iter = dim_iters_->at(idx);
    if (OB_ISNULL(dim_iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dim_iter->get_next_row())) {
      if (OB_ITER_END == ret) {
        iter_domain_ids_[idx] = nullptr;
        ret = OB_SUCCESS;
      }
    } else if (OB_FAIL(dim_iter->get_curr_id(iter_domain_ids_[idx]))) {
    } else if (OB_ISNULL(iter_domain_ids_[idx])) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  int64_t min_idx = -1;
  if (OB_SUCC(ret)) {
    next_round_cnt_ = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
      if (OB_NOT_NULL(iter_domain_ids_[i])) {
        int cmp_ret = 0;
        if (min_idx < 0) {
          min_idx = i;
          next_round_iter_idxes_[next_round_cnt_++] = i;
        } else if (OB_FAIL(merge_cmp_.cmp_ids(
                       *iter_domain_ids_[i], *iter_domain_ids_[min_idx], cmp_ret))) {
        } else if (cmp_ret < 0) {
          min_idx = i;
          next_round_cnt_ = 1;
          next_round_iter_idxes_[0] = i;
        } else if (0 == cmp_ret) {
          next_round_iter_idxes_[next_round_cnt_++] = i;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (min_idx < 0) {
        ret = OB_ITER_END;
      } else {
        id_datum = iter_domain_ids_[min_idx];
      }
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::fill_merge_heap()
{
  int ret = OB_SUCCESS;
  ObSRMergeItem item;
  bool skip_relevance_fetch = false;
#if OB_FTS_DAAT_IMPLICIT_BOOL_SCORE && OB_FTS_EXISTENCE_ONLY_DAAT
  skip_relevance_fetch = iter_param_->bool_relevance_only_
      && !iter_param_->need_pushdown_topk();
#endif
  if (skip_relevance_fetch) {
    // collect_dims_by_id() retains ID merging/deduplication and returns existence.
    // The heap comparator only reads IDs, so no per-posting score is needed.
    item.relevance_ = 1.0;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
    const int64_t iter_idx = next_round_iter_idxes_[i];
    ObISRDaaTDimIter *dim_iter = nullptr;
    if (OB_ISNULL(dim_iter = dim_iters_->at(iter_idx))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dim_iter->get_next_row())) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
        LOG_WARN("fail to try load next batch dimension data", K(ret));\
      } else {
        ret = OB_SUCCESS;
      }
    } else if (!skip_relevance_fetch && OB_FAIL(dim_iter->get_curr_score(item.relevance_))) {
    } else if (!skip_relevance_fetch && OB_NOT_NULL(iter_param_->dim_weights_) && FALSE_IT(item.relevance_ = item.relevance_ * iter_param_->field_boost_ * iter_param_->dim_weights_->at(iter_idx))) {
    } else if (OB_FAIL(dim_iter->get_curr_id(iter_domain_ids_[iter_idx]))) {
    } else if (FALSE_IT(item.iter_idx_ = iter_idx)) {
    } else if (OB_FAIL(merge_heap_->push(item))) {
    } else {
      LOG_DEBUG("push item to merge heap", K(ret), K(i), K(iter_idx), K(next_round_cnt_), K(item), K(iter_domain_ids_[iter_idx]));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (merge_heap_->empty()) {
    ret = OB_ITER_END;
  } else if (0 != next_round_cnt_ && OB_FAIL(merge_heap_->rebuild())) {
  } else {
    next_round_cnt_ = 0;
  }
  return ret;
}

int ObSRDaaTIterImpl::collect_dims_by_id(const ObDatum *&id_datum, double &relevance, bool &got_valid_id)
{
  int ret = OB_SUCCESS;

  const ObSRMergeItem *top_item = nullptr;
  bool curr_doc_end = false;
  bool skip_relevance_collection = false;
  int64_t iter_idx = 0;
  relevance = 0.0;
  got_valid_id = false;
#if OB_FTS_EXISTENCE_ONLY_DAAT
  skip_relevance_collection = iter_param_->bool_relevance_only_;
#endif

  while (OB_SUCC(ret) && !merge_heap_->empty() && !curr_doc_end) {
    if (merge_heap_->is_unique_champion()) {
      curr_doc_end = true;
    }
    if (OB_FAIL(merge_heap_->top(top_item))) {
    } else if (FALSE_IT(iter_idx = top_item->iter_idx_)) {
    } else if (!skip_relevance_collection
               && OB_FAIL(relevance_collector_->collect_one_dim(iter_idx, top_item->relevance_))) {
    } else if (OB_FAIL(merge_heap_->pop())) {
    } else {
      next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
    }
  }

  if (OB_SUCC(ret)) {
    id_datum = iter_domain_ids_[iter_idx];
    if (OB_ISNULL(id_datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      if (skip_relevance_collection) {
        // Every candidate has at least one matching token; the score is unobservable here.
        relevance = 1.0;
        got_valid_id = true;
      } else if (OB_FAIL(relevance_collector_->get_result(relevance, got_valid_id))) {
      }
      if (OB_SUCC(ret) && got_valid_id && OB_FAIL(process_collected_row(*id_datum, relevance))) {
      }
    }
  }

  return ret;
}

int ObSRDaaTIterImpl::process_collected_row(const ObDatum &id_datum, const double relevance)
{
  return OB_SUCCESS;
}

int ObSRDaaTIterImpl::filter_on_demand(const int64_t count, const double relevance, bool &need_project)
{
  int ret = OB_SUCCESS;
  ObExpr *filter_expr = iter_param_->filter_expr_;
  ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
  ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);

  if (!iter_param_->need_filter()) {
    need_project = true;
  } else if (eval_ctx->is_vectorized()) {
    if (OB_UNLIKELY(count >= eval_ctx->max_batch_size_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (filter_is_relevance_gt_zero_) {
      // The pushdown match filter only tests whether the matched document has a non-zero
      // relevance (`match_expr > 0` or `T_OP_BOOL(match_expr)`), and DaaT already summed
      // that relevance in `relevance`, so no expression has to be evaluated here.
      need_project = 0.0 != relevance;
    } else {
      guard.set_batch_idx(count);
      ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
      relevance_proj_datum.set_double(relevance);
      relevance_proj_expr->get_evaluated_flags(*eval_ctx).set(count);
      relevance_proj_expr->set_evaluated_projected(*eval_ctx);
      ObDatum *filter_res = nullptr;
      filter_expr->clear_evaluated_flag(*eval_ctx);
      if (OB_FAIL(filter_expr->eval(*eval_ctx, filter_res))) {
      } else {
        need_project = !(filter_res->is_null() || 0 == filter_res->get_int());
      }
    }
  } else if (OB_UNLIKELY(0 != count)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    guard.set_batch_idx(0);
    ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
    relevance_proj_datum.set_double(relevance);
    relevance_proj_expr->set_evaluated_projected(*eval_ctx);
  }
  return ret;
}

int ObSRDaaTIterImpl::cache_result(int64_t &count, const ObDatum &id_datum, const double relevance)
{
  int ret = OB_SUCCESS;
  const common::ObLimitParam *limit_param= iter_param_->limit_param_;
  const int64_t limit = limit_param->limit_;
  const int64_t offset = limit_param->offset_;
  ++input_row_cnt_;
  if (limit_param->is_valid() && input_row_cnt_ <= offset) {
    // TODO: Maybe we should not process offset logic here
    // don't need to project
  } else if (OB_UNLIKELY(count >= buffered_domain_ids_.count() || count >= buffered_relevances_.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    buffered_domain_ids_[count].from_datum(id_datum);
    buffered_relevances_[count] = relevance;
    ++count;
    ++output_row_cnt_;
    if (limit_param->is_valid() && output_row_cnt_ >= limit) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObSRDaaTIterImpl::project_results(const int64_t count)
{
  int ret = OB_SUCCESS;
  ObExpr *relevance_proj_expr = iter_param_->relevance_proj_expr_;
  ObExpr *id_proj_expr = iter_param_->id_proj_expr_;
  ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);

  if (eval_ctx->is_vectorized()) {
    sql::ObBitVector &id_evaluated_flags = id_proj_expr->get_evaluated_flags(*eval_ctx);
    for (int64_t i = 0; i < count; ++i) {
      guard.set_batch_idx(i);
      ObDatum &id_proj_datum = id_proj_expr->locate_datum_for_write(*eval_ctx);
      set_datum_func_(id_proj_datum, buffered_domain_ids_[i]);
      id_evaluated_flags.set(i);
      id_proj_expr->set_evaluated_projected(*eval_ctx);
    }
    if (iter_param_->need_project_relevance()) {
      sql::ObBitVector &relevance_evaluated_flags = relevance_proj_expr->get_evaluated_flags(*eval_ctx);
      for (int64_t i = 0; i < count; ++i) {
        guard.set_batch_idx(i);
        ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
        relevance_proj_datum.set_double(buffered_relevances_[i]);
        relevance_evaluated_flags.set(i);
        relevance_proj_expr->set_evaluated_projected(*eval_ctx);
      }
    }
  } else if (OB_UNLIKELY(1 != count)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    guard.set_batch_idx(0);
    ObDatum &id_proj_datum = id_proj_expr->locate_datum_for_write(*eval_ctx);
    set_datum_func_(id_proj_datum, buffered_domain_ids_[0]);
    id_proj_expr->set_evaluated_projected(*eval_ctx);
    ObDatum &relevance_proj_datum = relevance_proj_expr->locate_datum_for_write(*eval_ctx);
    relevance_proj_datum.set_double(buffered_relevances_[0]);
    relevance_proj_expr->set_evaluated_projected(*eval_ctx);
  }
  return ret;
}

namespace
{
// The pushdown match filter is normally `<match_expr> > 0`, sometimes wrapped as
// T_OP_BOOL(<lossless cast of match_expr>).  In both shapes the filter outcome only
// depends on the sign of the relevance value that DaaT has already collected, so the
// per-document expression tree walk can be replaced by a comparison on that value.
bool cast_preserves_zero(const ObExpr &expr)
{
  bool bret = false;
  switch (expr.datum_meta_.get_type()) {
    case ObDoubleType:
    case ObFloatType:
    case ObUDoubleType:
    case ObUFloatType:
    case ObNumberType:
    case ObUNumberType:
    case ObDecimalIntType: {
      bret = true;
      break;
    }
    default: {
      bret = false;
      break;
    }
  }
  return bret;
}

// returns true when `expr` evaluates to `relevance` unchanged
bool is_lossless_view_of(const ObExpr &expr, const ObExpr *relevance)
{
  bool bret = false;
  if (&expr == relevance) {
    bret = true;
  } else if (&expr != relevance && 1 == expr.arg_cnt_ && OB_NOT_NULL(expr.args_)
             && expr.args_[0] == relevance
             && (T_FUN_SYS_CAST == expr.type_ || T_FUN_SYS_DEFAULT == expr.type_)) {
    bret = cast_preserves_zero(expr);
  }
  return bret;
}
} // namespace

void ObSRDaaTIterImpl::init_filter_shape()
{
  filter_is_relevance_gt_zero_ = false;
  ObExpr *filter = iter_param_->filter_expr_;
  ObExpr *relevance = iter_param_->relevance_proj_expr_;
  ObEvalCtx *ctx = iter_param_->eval_ctx_;
  bool matched = false;
  int64_t argc = OB_NOT_NULL(filter) ? filter->arg_cnt_ : -1;
  if (OB_NOT_NULL(filter) && OB_NOT_NULL(relevance) && OB_NOT_NULL(ctx)
      && OB_NOT_NULL(filter->args_)) {
    if (T_OP_GT == filter->type_ && 2 == argc) {
      // <match_expr> > const_zero
      ObExpr *lhs = filter->args_[0];
      ObExpr *rhs = filter->args_[1];
      if (OB_NOT_NULL(lhs) && OB_NOT_NULL(rhs) && T_INT == rhs->type_
          && is_lossless_view_of(*lhs, relevance)) {
        ObDatum *rhs_datum = nullptr;
        if (OB_SUCCESS == rhs->eval(*ctx, rhs_datum) && OB_NOT_NULL(rhs_datum)
            && !rhs_datum->is_null() && 0 == rhs_datum->get_int()) {
          matched = true;
        }
      }
    } else if (T_OP_BOOL == filter->type_ && 1 == argc) {
      // T_OP_BOOL(<match_expr>) -- true iff the relevance value is non zero
      ObExpr *arg = filter->args_[0];
      if (OB_NOT_NULL(arg) && is_lossless_view_of(*arg, relevance)) {
        matched = true;
      }
    }
  }
  filter_is_relevance_gt_zero_ = matched;
}

int ObSRDaaTIterImpl::init_merge_heap(const int64_t count)
{
  int ret = OB_SUCCESS;
  ObSRSimpleMerger *simple_merge = nullptr;
  ObSRMergeHeap *loser_tree = nullptr;
  if (count <= ObSRSimpleMerger::USE_SIMPLE_MERGER_MAX_TABLE_CNT) {
    if (OB_ISNULL(simple_merge = OB_NEWx(ObSRSimpleMerger, iter_allocator_, merge_cmp_))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      merge_heap_ = simple_merge;
    }
  } else if (OB_ISNULL(loser_tree = OB_NEWx(ObSRLoserTree, iter_allocator_, merge_cmp_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    merge_heap_ = loser_tree;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(merge_heap_->init(count, count, *iter_allocator_))) {
  } else if (OB_FAIL(merge_heap_->open(count))) {
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
