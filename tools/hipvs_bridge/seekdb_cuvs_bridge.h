#ifndef SEEKDB_CUVS_BRIDGE_H
#define SEEKDB_CUVS_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot: build a CAGRA index over base (n x dim, row-major f32) on the GPU and
 * search queries (nq x dim), writing top-k uint32 neighbor ids to out_ids
 * (length nq*topk). Returns 0 on success, non-zero on error. */
int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids);

/* Build-once / search-many (used by seekdb's ob_vsag_adaptor GPU path).
 *  - seekdb_cuvs_build: builds a CAGRA index on the GPU from base (n x dim,
 *    row-major f32) and returns an opaque handle (NULL on error). cuVS copies
 *    the dataset to the device, so `base` may be freed after this returns.
 *  - seekdb_cuvs_search: runs top-k search for nq queries (row-major f32, dim
 *    cols), writing uint32 row offsets to out_ids[nq*topk] and (if non-NULL) L2
 *    distances to out_dist[nq*topk]. Returns 0 on success.
 *  - seekdb_cuvs_free: releases the handle; callers must finish all operations
 *    on that handle before freeing or serializing it.
 * Search scratch is private to each handle. With default reuse enabled,
 * concurrent searches of one handle are serialized through a per-handle mutex.
 * SEEKDB_CUVS_REUSE=0 before build/load restores per-call allocation.
 * Official CAGRA AUTO search selection is enabled by default.
 * SEEKDB_CUVS_AUTO_SEARCH=0 before build/load restores the C API's own
 * freshly initialized algorithm default, independently of scratch reuse.
 * OB_CUVS_BALANCED_BUILD=0 restores the preceding n60/I256/itopk512 defaults;
 * individual SEEKDB_CUVS_* overrides remain available for runtime A/B tests.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);
int seekdb_cuvs_serialize(void* handle, const char* filename);
void* seekdb_cuvs_deserialize(const char* filename, long n, long dim);
#ifdef __cplusplus
}
#endif
#endif
