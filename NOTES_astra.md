# AMD CAGRA audit and candidates (2026-10-30)

Base: verified niter=60 commit `8e61b873d4d7315cec911d929e953315328da821`,
itself a direct descendant of frozen V5 `634888d9b037153fa143d2c913ba4b25503e1d62`.
Remote bridge and seekdb compilation and the first proxy experiments are now
complete (2026-10-30). No candidate is formally scored or submitted; detailed
measurements are in analysis/astar_audit/GPU_MEASUREMENTS_20261030.md outside
this source tree. The historical sections below retain their original scope.

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

Historical pre-GPU risk: fixed-version hipVS header/runtime semantics and pthread
linking had not yet been compiled. The authorized remote build and the final
lifecycle checks below now cover these items. Official dataset recall and
explicit GPU allocation/copy fault injection remain unverified; do not submit
automatically or replace the existing pending evaluation.

## 003: official AUTO search selection with exact legacy opt-out

Files: tools/hipvs_bridge/seekdb_cuvs_bridge.c and .h. Functions: scratch params
creation, build/deserialize initialization, seekdb_cuvs_search.

The frozen /opt/hipvs C API was inspected at runtime: search params initialize
algo=0 (SINGLE_CTA), itopk=64, width=1. The wrapper keeps its established width4
and itopk floor512 but now explicitly selects the fixed official AUTO algorithm.
No search kernel, input vector precision, graph format or build default changes.
AUTO is a supported C API parameter; it can select MULTI_CTA for nq=1 while
retaining library heuristics for larger batches.

OB_CUVS_AUTO_SEARCH=1 is the compile default. SEEKDB_CUVS_AUTO_SEARCH=0 before
build/load restores the SearchParams object's captured original algorithm, not
a guessed enum default. This switch is independent of SEEKDB_CUVS_REUSE=0.
Both flags off restore the n60 wrapper's transient allocation/search-mode route.
Revert this commit to remove AUTO selection entirely.

Measured on a disposable authorized W7900D/ROCm7.2.4 instance, 250000x256
uniform proxy data (NOT official Hidden250K/SIFT250K):
- Same graph, 512 direct single-row calls: default 3.663/3.711ms vs AUTO
  .666/.667ms for top10/top100. Recall .9285/.8645 vs .9326/.8709; IDs valid.
- Fresh-index 4096 individual SQL queries: 133.65/131.00 QPS vs
  228.09/220.02 QPS (~1.69x geometric). Recall .9254/.8651 vs .9303/.8723.
- Same stored index, recovery before each run, default/AUTO/AUTO/default:
  default top10 130.73/126.46, top100 127.00/119.44 QPS;
  AUTO top10 219.99/219.06, top100 205.88/201.46 QPS. All k IDs legal/unique.
  Restore phase logs confirm deserialization, not graph rebuild.
- AUTO recall varies slightly across repeated process runs (.9290-.9295 at10,
  .8718-.8720 at100); approximate multi-CTA search is not claimed bitwise
  repeatable. Compare integrity, distance correctness and recall gates; do not
  mistake permitted algorithm nondeterminism for scratch corruption.

Final code validation completed remotely on the fixed ROCm7.2.4/hipVS image:
- Official build.sh succeeded for the default candidate; an independent
  OB_CUVS_AUTO_SEARCH=0 binary also compiled with the same flags.
- On the same 250000x256 serialized graph, runtime AUTO=0 with REUSE=0/1 and
  macro0 with runtime AUTO=1 match the original bridge IDs/distances exactly
  through 10/100/10, nq1->4, k1000 failure, then successful k10 recovery.
- Default AUTO works with REUSE=0/1 through the same sequence (including
  successful k1000), returning unique legal IDs, finite sorted distances,
  and distances matching direct float32 L2 within 2e-4 relative/absolute.
- Two threads on one handle and two threads across independent AUTO/legacy
  handles pass integrity/distance checks. No shared result scratch is used.
- Two fresh build/search/serialize/free/restore cycles on an independent
  8192x64 clustered proxy also pass. This is lifecycle coverage, not a claim
  that official SIFT/Hidden recall gates have been certified.
- Evidence: analysis/astar_runtime/auto_lifecycle.json, samegraph_results.jsonl.
  Timing inside lifecycle checks includes CPU validation, so it is not used
  for the speed claim. No explicit GPU allocation/copy fault injection yet.
The proxy speed is not a formal score. Official best remains 7.247/20; query
alone with build1.270x caps near9.046. Do not replace pending n48 record47231435.

Build attribution/negative results: n100 DDL+first query50.61s, build host
44.591s; n60 34.77s, build28.829s. add calls250000 but only19 capacity
growths; add body .24-.32s, snapshot write phases .69-.86s. These do not
support a dominant repeated-allocation or 60% fixed-overhead theory. Default
IVF_PQ build7.208s but recall@100~.596 is unacceptable; default iterative
CAGRA build60.504s is slower. These alternatives are not part of this commit.
