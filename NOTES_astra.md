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
