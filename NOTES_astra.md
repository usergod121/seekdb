# Astra FTS Performance Notes

## 2026-10-30 / DELIVER_007 candidate: implicit boolean scores

- Files/functions: `ob_sparse_daat_iter.cpp::fill_merge_heap`; `ob_text_retrieval_token_iter.cpp::save_relevances_and_docids/get_curr_score`. Reuse the existing doc-id cache and represent the existing boolean-only relevance constant implicitly. Do not disable `need_calc_relevance`, remove token cursors, or bypass same-ID merging/deduplication.
- The merge-side omission requires both the existing existence-only gate and `!need_pushdown_topk()`. BMW/WAND initialization also calls the base heap-fill function, so top-k is deliberately excluded. Explicit score/order and all existing top-k paths keep their score-fetch behavior. The heap comparator reads doc IDs only.
- For P posting advances, eligible paths avoid P virtual score fetches, P writes of constant 1.0, and any ignored score-weight multiplication. P is not the number of unique output hits. Bounds checking still occurs through `get_curr_id`; doc-id conversion, skip mask and cursor lifetime are unchanged. `relevance_` allocation and ABI remain unchanged.
- Rollback: compile both changed translation units with `OB_FTS_DAAT_IMPLICIT_BOOL_SCORE=0`; the old score-cache loop and fetch behavior are retained. `OB_FTS_EXISTENCE_ONLY_DAAT=0` independently disables the merge-side omission.
- Quantification: previous platform data imply roughly 145-172 ns per unique hit; the overall query target remains 115-135 ns/hit. No honest static estimate shows that this small candidate reaches that target. Measure paired CPU us/call and `(query_ms-select1_ms)*1e6/hits`; reject if it adds branches without repeatable net benefit. No score increase is booked.
- Required runtime checks: official benchmark counts 8001/11000/7332/20; macro on/off identical results; explicit MATCH projection and relevance DESC LIMIT 20 identical IDs, scores and order; parent row projection retains exact WAND; BOOLEAN mode, weights, minimum_should_match, LIMIT/OFFSET and repeated scans. No compilation or runtime result exists for 007 at this checkpoint.
- Updated execution authorization: the user now asks Astra to compile and test directly and then submit after validation. Use isolated source/build/data and port 2971; never change the other running service. The Docker VHD is on F with only about 5.7GiB actual free despite an 825GiB virtual df result, so own build/data are directed to E. No dependency installation.

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

## 2026-10-30 / DELIVER_004: retain DML datum-pointer slots

- Base: `94164c019`; branch: `astra/fts-build-hotpath-20261030`. Overseer accepted the earlier three query patches and reports record `47231462` at 34.365/50. This is a new build-side candidate, not a measured improvement.
- File/function: `src/sql/engine/pdml/static/ob_px_sstable_insert_op.cpp`, `ObPxMultiPartSSTableInsertOp::eval_current_row(ObIArray<ObDatum *> &)`. Both ordered and heap scalar SSTable-insert loops call it once per output row.
- The old loop empties a pointer array and calls virtual `push_back` for every output column on every row. Keep its fixed slot count and write each freshly evaluated pointer directly into the corresponding slot. Preserve `clear_evaluated_flag`, every expression evaluation, all NULL-expression checks, order, and writer calls. A child may change datum addresses: this patch never caches the datum pointers themselves between rows. A count change rebuilds the slots.
- Rollback: `OB_DDL_REUSE_DATUM_POINTER_ARRAY=0` compiles the old implementation verbatim; default is 1. The public writer API, parser, SQL and sorting are unchanged.
- Static API check: `ObIArray::prepare_allocate` is declared in `src/oblib/lib/container/ob_iarray.h`; its array implementation reserves and establishes the element count. No build or runtime test was run.

### Quantification and acceptance

- For N output rows and C projected columns, remove N-1 array-reuse calls and N*C virtual append/capacity/count-update paths, replacing them with direct pointer stores. With the brief's 1.4-1.6 million aux rows and a four-column plan, this is 5.6-6.4 million avoided append paths. Confirm the actual C from the compiled plan rather than assuming it for every schema.
- There is no honest static conversion from that operation count to nanoseconds. Existing `reuse()` already retains allocated capacity: this is NOT elimination of a heap allocation per row and is not a claim of 1.1us -> 0.75us. Measure delta_ns_per_row = 1e9*(old_cpu_sec-new_cpu_sec)/N, and wall time separately because the aux tasks can overlap.
- An illustrative measured saving of 50ns/row would save 0.070-0.080 CPU seconds for N above; this is an acceptance scenario, not a measured or guaranteed saving. Reject the candidate if paired runs fail to show a repeatable saving above variance.
- Verification: in Overseer's existing container, compare macro 1/0 builds on repeated `ALTER TABLE docs ADD FULLTEXT INDEX fti_all_ik(title,content) WITH PARSER ik` (drop/recreate that index between paired runs). Record ALTER wall, user+system CPU, cpu/wall cores, writer row count and all three build timings. Run the four MATCH counts 8001/11000/7332/20, explicit relevance-order ID/order comparison, official FTS cases, NULL/empty text and multi-column data. No benchmark/test files are changed by this patch.

### Correct the brief's performance arithmetic

- Reducing a 25%-of-build component by 35% saves at most 8.75% of total build before overlap, not 25%. Reaching the requested total-build reduction needs additional work.
- Applying the brief's displayed normalized formula to its displayed metrics gives category rates 0.28886 / 0.30150 / 0.53753 and total 37.5965, whereas the stated actual record is 34.365. This discrepancy requires raw platform score details; do not silently replace the actual score with the recomputation. No forecast of 50 is justified before resolving the aggregation/normalization difference.
- Under that displayed formula, a 10% whole-build relative latency reduction adds about 2.37 total points, not 3.3. Query's displayed category average already exceeds 50%; requiring every individual metric to reach 50% is not implied by the stated category-average formula. Actual platform aggregation remains authoritative.

## 2026-10-30 / DELIVER_005: use initialized tablet/slice expression metadata

- File/function: src/sql/engine/pdml/static/ob_px_sstable_insert_op.cpp, ObPxMultiPartSSTableInsertOp::get_tablet_info_from_row. Parent commit 2335df445, series base 94164c019, branch astra/fts-build-hotpath-20261030.
- inner_open already calls locate_exprs before row processing. It validates the partition expression index / single-tablet location and stores tablet_id_expr_, non_partitioned_tablet_id_, and (ordered plans) slice_info_expr_. The scalar path ignored all three caches, checked the table location and searched child output types on every row. Use those existing initialized fields instead.
- Cache expression nodes and immutable non-partitioned tablet ID only. Read the current tablet/slice datum every row. Reset ObTabletSliceParam and preserve slice_id_ vs slice_idx_ assignment and the max-slice guard. Heap callers never request a slice parameter; ordered callers get the same first matching slice expression found at initialization. No new serialized fields or writer interfaces.
- Switch: OB_DDL_USE_CACHED_TABLET_SLICE_EXPRS defaults to 1; 0 compiles the old function body. Static comparison confirms the disabled code lines equal its parent implementation (empty lines ignored). No compilation or runtime measurements.
- Operation count: one table-location/cardinality lookup per non-partitioned scalar row removed, plus one scan over child output expression types per ordered row. For 1.4-1.6M aux rows and no slice pseudo-expression, this removes 1.4-1.6M metadata lookups and N*C expression-type visits (C is actual child output count). No static ns estimate is justified. At an illustrative measured 20ns/row, CPU saving is only 0.028-0.032 seconds. Do not market this as a full-score solution.
- Container recipe: compare macro 1/0 with DELIVER_004 held fixed, paired fresh fulltext index builds (all/title+content, content, beng). Record wall/user+system CPU, output rows and ns/row; reject if no repeatable signal. Check four MATCH counts 8001/11000/7332/20, explicit relevance-order IDs/order, LIMIT variants, NULL/empty/multicolumn cases, official FTS cases. Also test partitioned/unpartitioned inserts, ordered slice boundary/retry, heap and idempotent/autoincrement/doc-id paths. The change applies to scalar SSTable inserts generally, not only FTS.
- Risk: assumes initialized operator metadata is stable throughout execution, as current locate_exprs/call sites imply. No row datum address is retained between child advances. Any routing/retry discrepancy requires macro=0 or revert this commit; no balancing fallback or silent error suppression.
- Negative conclusion: current small pointer/metadata changes have no measured score gain yet. They cannot substantiate 1.1us -> 0.75us or 50/50. Bigger caller-side parsing reuse needs the exact DDL call-chain evidence first. The duplicate row_arena.reuse is real but does not justify a large standalone score claim.


# 008: small existence-only DaaT union

Status: implemented and statically reviewed; no compilation/performance claim yet.

- Files: src/storage/retrieval/ob_sparse_daat_iter.{h,cpp}; functions init_merge_strategy, next_small_existence_id, do_one_merge_round, reuse.
- For boolean existence-only retrieval with 1-4 token iterators and no pushed top-k, maintain each live sorted head and select its minimum ID directly. Advance every cursor equal to that ID, preserving union deduplication. Larger token sets and all observable-score/top-k paths retain existing heap/WAND.
- Critical-path evidence: supplied query_cn/beng/mixed costs are 134.8/154.5/172.2 ns per hit. This removes repeated heap push/rebuild/top/pop and generic score transport for eligible hits. Target is 115-135 ns/hit; actual reduction and score remain unknown until paired measurements.
- Rollback: OB_FTS_SMALL_EXISTENCE_MERGE=0 restores parent merge strategy. OB_FTS_DAAT_IMPLICIT_BOOL_SCORE=0 restores separate 007 behavior.
- Verify official counts 8001/11000/7332/20; LIMIT 1=1; explicit MATCH score DESC LIMIT 20 IDs, ordering and scores unchanged; projected-parent IDs/order unchanged; official tokenize_function/simple_query PASS; >=20 TOKENIZE strings byte-identical; macro-off output identical. Benchmark >=5 paired CPU-time runs and official raw metrics, no local 100/100 score claim.
- Risks checked statically: initial/reused rounds advance every token before inspecting head pointers; exhausted cursors become null; tied minimum IDs advance together; untaken cursors retain their original datum ownership. Empty token lists, errors, batches, filters and projection retain existing lifecycle. No tokenizer/ranked-result semantics changed.
- Negative conclusion: no measured ns saving or platform gain exists yet. Environmental build/startup repairs do not count as optimization.

## 010: one hash lookup for grouped token frequency

- File/function: src/storage/fts/ob_fts_stop_word.cpp, ObAddWord::groupby_word.
- Existing repeated-token path hashes the same collation-aware word for failed insert, read, and overwrite. Use existing ObHashMap::set_or_update to insert or update the frequency in one bucket traversal; its callback receives the value reference, not the key/value pair.
- Preserve the current first-occurrence value1 even when word_freq>1, increment existing entries by word_freq, preserve nongrouped insertion, token bytes, collation hash/equality, and insertion order.
- Quantification: for U distinct words and D repeated occurrences, hash calls fall from U+3D to U+D. Total saving depends on observed D and hash cost; no measured ns or score gain is claimed before A/B.
- Rollback OB_FTS_SINGLE_LOOKUP_WORD_FREQUENCY=0 compiles the exact old grouped path. Default1.
- Verify old/new outputs for 24 varied strings plus repeated terms and output_mode ALL frequencies, 4 MATCH hits8001/11000/7332/20, LIMIT1, explicit relevance ordering, official tokenize_function/simple_query. Use short five-run CPU comparisons; only retain measured gains.
- Risk: set_or_update and set_refactored share internal_set for a missing word and use the same equality; confirm byte-identical JSON order. Allocation failures remain errors. Existing first-frequency semantics are intentionally not corrected here.
- Formal baseline update: candidate51624e61c scored38.727/50, record47232218. Official query0.9808/1.4928/1.1374/0.2900ms, hits8001/11000/7332/20; build3.151/2.325/1.422s, tokenize0.0599/0.0423ms. Build gain from004/005 is not established; next work targets shared parser/DDL costs.

## 011: borrow basic-English tokens already owned by the document

- File/function: src/storage/fts/ob_beng_ft_parser.cpp, ObBEngFTParser::get_next_token.
- ObBasicEnglishNormalizer already returns a source-document slice for unchanged lowercase ASCII. ObBEngFTParser copied every such token into the segment arena. Borrow only spans wholly inside doc_; keep original allocating-copy path for normalized temporary buffers and every other token.
- Lifetime proof: doc_ is the full input passed to segment and remains alive until the caller finishes its word map, as required by existing IK/whitespace document views. Normalizer arena is reused each token, therefore a generic return token.ptr_ without ownership test would be wrong and is deliberately not used.
- Quantification: remove one arena allocation and one memcpy for each document-backed token; no tokenization, normalizing, stopword or grouping operation is skipped. Benefit depends on token count and is unmeasured.
- Rollback OB_FTS_BENG_BORROW_DOCUMENT_TOKENS=0 restores the old owning-copy behavior. Default1. No new state, caches or invalidation.
- Verify TOKENIZE default and ALL for lowercase, uppercase, mixed, repeated words, punctuation, digits, nonASCII; byte-identical output and frequencies. Official tokenize_function/simple_query and four MATCH counts must pass. Short five-run CPU comparison and raw official metrics decide retention.
- Risk: arithmetic compares integer addresses to avoid unrelated-pointer relational comparisons and checks length by subtraction to avoid end-pointer overflow. Non-document tokens always copy before the next normalizer reuse.
