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
 *  - seekdb_cuvs_free: releases the handle.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);
int seekdb_cuvs_serialize(void* handle, const char* filename);
void* seekdb_cuvs_deserialize(const char* filename, long n, long dim);

/* ------------------------------------------------------------------ *
 * Fast snapshot path (build-window optimization).
 *
 * The cuVS file format embeds the dataset inside the index blob, so a
 * "persist the CAGRA graph" round trip writes and reads the whole dataset
 * twice (once inside the cuVS blob, once in seekdb's own snapshot header)
 * and stages the blob through the filesystem twice.
 *
 * Instead, export just the knn graph of a built index and re-create the
 * index from (graph, dataset) with cuvsCagraIndexFromArgs(). The restored
 * index holds exactly the same graph and the same dataset values as the
 * built one, so search results (and therefore recall) are unchanged.
 * ------------------------------------------------------------------ */

/* Copy the knn graph of a built CAGRA index into graph_out (n * graph_degree
 * uint32, row-major). *graph_degree_out receives the graph degree.
 * Returns 0 on success. */
int seekdb_cuvs_export_graph(void* handle, unsigned int* graph_out,
                             long n, long* graph_degree_out);

/* Re-create a CAGRA index from a host dataset (n x dim float32, row-major) and
 * a host knn graph (n x graph_degree uint32, row-major). The graph is uploaded
 * to the device and owned by the returned handle; the dataset is copied to the
 * device by cuVS. `metric` must be the cuvsDistanceType the index was built
 * with (see seekdb_cuvs_index_metric). Returns NULL on error. */
void* seekdb_cuvs_restore(const float* dataset, long n, long dim,
                          const unsigned int* graph, long graph_degree,
                          int metric);

/* 1 when the linked cuVS supports the fast snapshot path (verified by a
 * self-contained build/export/restore/search round trip at first call),
 * 0 otherwise. The result is cached for the lifetime of the process. */
int seekdb_cuvs_supports_fast_snapshot(void);

/* Distance metric (cuvsDistanceType) the given index was built with, so a
 * restored index uses the same metric as the built one. Returns -1 for a NULL
 * handle. */
int seekdb_cuvs_index_metric(void* handle);

/* Default distance metric (cuvsDistanceType) of a freshly created CAGRA
 * parameter set. */
int seekdb_cuvs_metric(void);
#ifdef __cplusplus
}
#endif
#endif
