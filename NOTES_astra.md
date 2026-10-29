# Astra FTS Performance Notes

## T5: DaaT existence-only merge

### Change

- `src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp`, `ObDASLegacyTRMergeIter::create_sparse_retrieval_iter`: enable existence-only collection only when the plan says relevance is boolean-only, the search is not BOOLEAN mode, and `minimum_should_match <= 1`.
- `src/storage/retrieval/ob_i_sparse_retrieval_iter.h`, `ObSparseRetrievalMergeParam`: carry the eligibility bit to DaaT.
- `src/storage/retrieval/ob_sparse_daat_iter.cpp`, `ObSRDaaTIterImpl::collect_dims_by_id`: continue popping all dimensions with the same document id, but skip per-dimension relevance collection and the per-document collector result when only match existence is observable. Emit a constant positive relevance so the existing `MATCH > 0` filter remains true. `OB_FTS_EXISTENCE_ONLY_DAAT` defaults to `1`; define it as `0` to restore the collector path.

### Hot-path evidence and expected impact

- The platform measurements imply `143.9 ns` (`query_cn`), `163 ns` (`query_beng`), and `169 ns` (`query_mixed`) per hit after subtracting the `0.0248 ms` `select1` floor, at `8001/11000/7332` hits.
- These queries use `ObDASLegacyTRMergeIter` and `ObSRDaaTIterImpl`. For each unique candidate, `collect_dims_by_id` invokes `collect_one_dim` once per matching token posting and `get_result` once per candidate. The fast path removes those collector calls and the associated score accumulation while retaining same-id cursor advancement and one output per document.
- Static estimate only: saving roughly `10-25 ns` per hit would project `query_cn` to `119-134 ns`, `query_beng` to `138-153 ns`, and `query_mixed` to `144-159 ns`. This does not establish the requested `120-135 ns` for all three; container measurements are needed to decide whether T5 has further headroom. No score gain is claimed from an unmeasured estimate.
- With the platform baselines and `f=8.48`, that saving range would add roughly `0.95-2.4` total score points if it holds across these three query metrics; this is a projection, not a result.

### Container verification required

1. Run the four platform MATCH queries and confirm hit counts remain `8001/11000/7332/20`.
2. Compare `query_cn`, `query_beng`, and `query_mixed` latency per hit before/after using `(avg_ms - 0.0248) * 1e6 / hits`; target is `120-135 ns` per hit, with repeated runs and noise reported.
3. Run with `OB_FTS_EXISTENCE_ONLY_DAAT=0`; it must restore the old collector path and the same hit counts.
4. Compare `SELECT id, MATCH(c) AGAINST('x') AS s FROM t WHERE MATCH(c) AGAINST('x') ORDER BY s DESC LIMIT 20`; explicit relevance projection/order must keep exact scoring and preserve result set and order.

### Risks and rollback

- The fast path depends on the plan's `bool_relevance_only` guarantee and `minimum_should_match <= 1`; it is explicitly disabled for BOOLEAN mode and higher minimum-match thresholds. A false-positive plan flag could otherwise change filter results.
- Roll back by defining `OB_FTS_EXISTENCE_ONLY_DAAT=0` or reverting this T5 commit. No compile or runtime validation was performed on this workstation.

## query_limit early stop

### Change

- `src/sql/rewrite/ob_transform_pre_process.cpp`, `ObTransformPreProcess::transform_one_stmt` / `preserve_order_for_fulltext_search`: mark the hidden MATCH order only when every parent is a pure, unfiltered, ungrouped `COUNT(*)` consumer (or the statement has no parent). A parent projection such as `SELECT id FROM (... LIMIT 20)` observes the produced rows and is not eligible.
- `src/sql/resolver/expr/ob_raw_expr.h` and `.cpp`, `ObMatchFunRawExpr`: carry the marker through expression copies and reset it with expression state.
- `src/sql/optimizer/ob_log_plan.cpp`, `match_score_is_observable`: treat only the marked hidden order as unobservable. A selected MATCH score, explicit `ORDER BY relevance`, or `FETCH ... WITH TIES` keeps exact relevance observable.
- `src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp` and `.h`, `ObDASLegacyTRMergeIter::inner_init` / `create_sparse_retrieval_iter`: route score-unobservable top-k from BMW/WAND to DaaT and give DaaT a `LIMIT + OFFSET` candidate cap. DaaT merges unique document IDs and its existing limit check stops advancing token cursors at that cap.
- `OB_FTS_APPROX_TOPK_EARLY_STOP` defaults to `1`. Build with `-DOB_FTS_APPROX_TOPK_EARLY_STOP=0` to restore the old relevance-observable decision and BMW/WAND route.

### Evidence and expected impact

- Platform record `47227738` reports `query_limit_avg_ms=1.8166`; `select1` is `0.0248 ms`, and LIMIT 1 and LIMIT 20 take the same time. The observed per-hit cost is about `144 ns`, consistent with scanning far more candidates than the requested 20.
- The benchmark query is `SELECT COUNT(*) FROM (SELECT id FROM fts_large_bench.docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t;`. The hidden relevance order selects BMW/WAND, whose pruning loses effectiveness on the benchmark's near-equal scores. DaaT can stop after 20 unique document IDs.
- If the current `144 ns` per-hit estimate holds, reducing more than 10,000 candidate advances to 20 removes about `1.44 ms` of variable scan work. This is an estimate, not a measured result; DaaT setup and fixed query work remain.
- Platform score target: `query_limit_avg_ms <= 0.765 ms`, corresponding to about 60% improvement for this metric using `f=8.48`. If reached with other categories unchanged, the total score gain is about 4.6 points. The local report score is not an acceptance measure.

### Container verification required

1. Build in the existing container with the default macro and run the platform `fts_large_bench` benchmark. Confirm hits remain `query_cn=8001`, `query_beng=11000`, `query_mixed=7332`, and `query_limit=20`; compare repeated `query_limit_avg_ms` runs against the `<=0.765 ms` target and calculate score with the platform formula.
2. Run the query above with LIMIT 1 and LIMIT 20. LIMIT 20 must return count 20, and its latency should materially exceed neither LIMIT 1 nor a 20-candidate DaaT walk.
3. Compare the result IDs and order for `SELECT id, MATCH(c) AGAINST('x') AS s FROM t WHERE MATCH(c) AGAINST('x') ORDER BY s DESC LIMIT 20` with the macro enabled and disabled. The set and relevance order must match; this query must remain on BMW/WAND.
4. Run `SELECT id FROM (SELECT id FROM fts_large_bench.docs WHERE MATCH(content) AGAINST('倒排索引 tokenizer') LIMIT 20) t`. The inner retrieval must remain on BMW/WAND because the parent projects its rows; it must not use the approximate DaaT early stop.
5. Repeat the benchmark with `OB_FTS_APPROX_TOPK_EARLY_STOP=0`. It should restore the prior query plan/behavior, including the prior `query_limit` latency range, while preserving all four hit counts.

### Risks and rollback

- When relevance is not returned or explicitly ordered, LIMIT results may be a different subset because DaaT stops in document-ID merge order instead of ranking all matches. This is permitted only for the injected hidden order; explicit score consumers and `WITH TIES` retain BMW/WAND.
- The score-observability check depends on the implicit-order marker surviving optimizer expression copies, and the row-order gate depends on the parent statement chain remaining available during preprocessing. Exercise the count-only wrapper and row-projecting parent cases in the container.
- Roll back by building with `OB_FTS_APPROX_TOPK_EARLY_STOP=0`, or revert this single commit. No build or runtime validation has been performed on this workstation.

## Negative results and excluded directions

- Raising the aux-fill SQL `parallel(N)` from 1 to 2/4/8 produced ALTER wall times `3.95/3.96/3.95/4.28 s` and core counts `1.66/1.78/1.78/1.84`; all four hit counts were correct, with no useful gain on one partition. Do not revisit `get_real_parallelism` or the hint path.
- DDL task status refresh accounted for 79% of DDL-stage time. Interval-based refresh reduced stage time from `10.7 s` to `2.7 s` but did not change wall time because it overlapped data build. DDL orchestration, status publication, schema retries, and client polling are not targets.
- Runtime schema guard reads took `83-330 us`; `OB_SCHEMA_EAGAIN` and full schema refresh were not observed. Schema refresh work is not justified.
- Removing the hidden relevance order made the plan cleaner but ran 3x slower by losing the DaaT path. Setting `need_calc_relevance=false` broke semantics (hits changed to `4000/0/...`). Skipping `ObDASTextRetrievalIter` aggregation returned correct hits with zero gain; its `do_doc_cnt_agg()` was not called in the measured path.
- Client DDL polling changed the platform score from `26.410` to `26.168`, a regression. Do not revisit it.
- Build-side aux fill was measured at roughly 24% tokenization and `1.1 us/row` DML for about 1.4-1.6 million rows, using only 1.4-1.7 cores. It remains a separate priority; this commit changes only query_limit.
