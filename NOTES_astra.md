# AMD CAGRA audit and candidates (2026-10-30)

Base: verified niter=60 commit `8e61b873d4d7315cec911d929e953315328da821`,
itself a direct descendant of frozen V5 `634888d9b037153fa143d2c913ba4b25503e1d62`.
These patches have not been compiled, executed, pushed, or submitted.

## 001: opt-in stage and vector-growth diagnostics

File: `src/oblib/lib/vector/ob_vsag_adaptor.cpp`.
Functions: `ob_cuvs_add`, `ob_cuvs_serialize`, `ob_cuvs_deserialize`.

- `SEEKDB_CUVS_PROFILE=1` before process startup enables diagnostics. Unset it
  for normal performance runs; the flag is cached per process. No GPU barriers
  are added. No search implementation or search parameter is changed.
- Per-entry insertion counters distinguish calls, actual capacity growths,
  estimated old-vector bytes copied on growth, mutex wait and insert body time.
  `std::vector::insert` already has amortized growth: a per-row call does not
  prove per-row reallocation, quadratic copying, or a 60% bottleneck.
- Snapshot diagnostics separate host build/join, cuVS graph file serialization,
  outer IDs/vectors stream writes, and graph stream writes. Restore diagnostics
  separate outer payload reads, temporary graph file writes, and cuVS reload.
- These are host wall-time intervals. Async GPU work may be charged to the
  subsequent synchronization or serialization; they are not individual kernel
  times. Restore `outer_vector_read_us` includes header parsing/allocation.
  Existing `build_us` includes graph serialization and must not be treated as
  GPU build time alone. Timed portions omit surrounding SQL/DDL work.

Expected direct speed benefit: none. Purpose: replace the unsupported fixed-cost
attribution with measured, reproducible phase data. Profiling per row has its own
clock overhead; use diagnostics for attribution, then unset PROFILE for scores.

Validation, in the existing authorized GPU environment only:
1. Build the bridge and seekdb with the official frozen commands.
2. Run fresh V5/n60 matched offline DDL + first legal query, twice with swapped
   order for each public/proxy dimension. Include first use in the DDL window;
   do not reuse an old graph or suppress the first-query work.
3. Use PROFILE=1 on a separate diagnostic process and collect `[cuVS][profile]`
   lines for every entry; do not sum parallel wall intervals as serial work.
4. Compare capacity growth count with call count. Do not prioritize reserve
   changes unless measured insert body time can explain meaningful total time.
5. PROFILE unset restores the non-diagnostic route. Revert this commit to remove
   the counters/branches entirely. Negative/failed observations belong in the
   audit report; no synthetic recall result proves the hidden-data gate.

## 002: isolated search scratch reuse candidate (uncompiled)

Files: `tools/hipvs_bridge/seekdb_cuvs_bridge.c`, `.h`, `build.sh`.
Functions: `seekdb_cuvs_search`, build/load/free lifecycle, scratch helpers.

- Keep the verified niter60 defaults, official cuVS search, width=4 and itopk
  floor=512. No PQ, alternate kernel, mixed precision, dataset omission, graph
  format change or lazy rebuild is introduced.
- Default `OB_CUVS_REUSE_SEARCH_SCRATCH=1`; `SEEKDB_CUVS_REUSE=0` before startup
  restores transient per-call device allocation/parameter-object lifetime. The
  compile macro can also be set to 0 in source. A full revert removes all changes.
- Build AND deserialize initialize the reuse flag and mutex; opaque handles
  returned by either route get independent scratch ownership. Capacity is byte
  based, checked for size overflow, and grows only when necessary. Allocate a new
  buffer before releasing the old one; capacity is changed only on success.
- Errors clear all scratch, including partial allocation, so retry starts clean.
  Free releases scratch and params before resources, then destroys the mutex.
  As before, destroy/serialize must not race active calls.
- Reuse-enabled searches of one handle are protected by a per-handle pthread
  mutex; separate indexes have no shared scratch. The legacy adaptor already
  holds its registry mutex, and the batch helper owns its temporary handle.
  The code uses the existing cuVS resources/stream and synchronous host result
  copies, not cross-stream sharing. It does not add a user stream API. Verify
  this behavior on frozen ROCm/hipVS; the Windows environment cannot prove it.
- build.sh supplies -pthread within the permitted build script. No dependencies
  are added and the official outer compile command remains unchanged.
- The fresh search object's default itopk is retained and applied each call, so
  100->10 (or 1000->10) does not retain a larger prior itopk. Width overrides keep
  exactly the existing behavior. Other search fields remain defaults.
- Result-copy failure now returns rc17 instead of silently reporting success.

Quantifiable expected operation reduction, NOT a runtime speed claim:
after one handle reaches stable query/result capacity, subsequent searches make
zero explicit cuvsRMMAlloc/Free calls and reuse one SearchParams object, versus
three allocs/frees and one create/destroy per call. For nq=1,k<=100,dim<=256 the
retained wrapper scratch is about 1.8KB per handle (allocator overhead excluded).
No claim is made about internal cuVS search allocations or GPU kernel speed.
RMM may already pool allocations; the actual saved latency may be small/zero.

Validation in the existing authorized GPU environment:
1. Compile official bridge+seekdb commands; no performance claim before this.
2. Compare REUSE=0 vs REUSE=1 with the SAME serialized graph, all top10/top100
   queries, ID arrays/distances and integrity, in 10/100/10/1000/10 sequences.
   Rebuild graph separately when measuring end-to-end build, never reuse a graph
   across formal rounds. First scratch allocation belongs to the first query.
3. Exercise build->search->free and build->serialize->free->deserialize->search,
   repeated create/drop, nq growth, k growth/shrink, allocation/copy failures,
   two indexes, and concurrent calls on one handle. Destroy only after joins.
4. Check SIFT/Hidden absolute recall, <=0.005 same-round delta, unique legal k
   IDs, GPU path and memory gates. Proxy data cannot certify Hidden recall.
5. Measure official nq4096 single-row SQL QPS in both top-k groups with PROFILE
   unset and swapped candidate/base order. Retained scratch is not a GPU-kernel
   acceleration; use the paired score, not the wrapper operation count.

Known remaining risk: fixed-version hipVS header/runtime semantics and pthread
linking have not been compiled here. A candidate is not a verified deliverable
until the above checks pass. Never submit automatically.
