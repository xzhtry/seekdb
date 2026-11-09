/**
 * Copyright (c) 2025 OceanBase.
 *
 * Snapshot container for the GPU (cuVS / CAGRA) vector index.
 *
 * This header deliberately has no OceanBase dependency: it only talks to
 * std::iostream, so the container format can be exercised by a standalone
 * unit test (see tools/hipvs_bridge/test_fast_snapshot.cpp) without a full
 * seekdb build.
 *
 * Two container versions exist:
 *
 *   OBCUVS02 (baseline)
 *     ids | vectors | <cuVS blob>
 *   The cuVS blob embeds the dataset *and* the knn graph. Since seekdb keeps
 *   its own copy of the ids and the vectors (the CPU VSAG fallback needs
 *   them), the dataset is written twice and the blob is staged through the
 *   filesystem twice (once to serialize, once to deserialize).
 *
 *   OBCUVS03 (fast path)
 *     ids | vectors | graph
 *   Only the knn graph is exported from the built index, so the dataset is
 *   stored exactly once and no cuVS blob travels through the filesystem.
 *   The restored GPU index is re-created from (graph, dataset) with
 *   cuvsCagraIndexFromArgs(); it holds the same graph and the same dataset
 *   values as the built one, so search results -- and therefore recall --
 *   are unchanged.
 */

#ifndef OB_CUVS_SNAPSHOT_H
#define OB_CUVS_SNAPSHOT_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>

namespace ob_cuvs_snapshot {

// Baseline container magic.
constexpr char kMagicBase[8] = {'O', 'B', 'C', 'U', 'V', 'S', '0', '2'};
// Fast container magic.
constexpr char kMagicFast[8] = {'O', 'B', 'C', 'U', 'V', 'S', '0', '3'};
// Length of both magics.
constexpr size_t kMagicSize = 8;

// Header of the OBCUVS03 container, written right after the magic.
struct FastHeader {
  int32_t dim = 0;         // vector dimension
  int32_t reserved = 0;    // must be 0, reserved for future use
  int64_t n = 0;           // number of vectors
  int64_t graph_degree = 0;// neighbours per node in the knn graph
  int32_t metric = 0;      // cuvsDistanceType the index was built with
};

// Chunk size used for the stream copies. A single large write() is not
// portable across every stream implementation, so the payload is copied in
// bounded chunks.
constexpr size_t kChunkBytes = 64UL * 1024 * 1024;

inline bool write_bytes(std::ostream &out, const void *data, size_t bytes)
{
  const char *p = static_cast<const char *>(data);
  while (bytes > 0 && out.good()) {
    const size_t sz = bytes < kChunkBytes ? bytes : kChunkBytes;
    out.write(p, static_cast<std::streamsize>(sz));
    if (!out.good()) { return false; }
    p += sz;
    bytes -= sz;
  }
  return out.good();
}

inline bool read_bytes(std::istream &in, void *data, size_t bytes)
{
  char *p = static_cast<char *>(data);
  while (bytes > 0) {
    const size_t sz = bytes < kChunkBytes ? bytes : kChunkBytes;
    in.read(p, static_cast<std::streamsize>(sz));
    if (in.gcount() != static_cast<std::streamsize>(sz)) { return false; }
    p += sz;
    bytes -= sz;
  }
  return true;
}

inline bool is_fast_magic(const char *magic)
{
  return 0 == std::memcmp(magic, kMagicFast, sizeof(kMagicFast));
}

inline bool is_base_magic(const char *magic)
{
  return 0 == std::memcmp(magic, kMagicBase, sizeof(kMagicBase));
}

// Number of float elements and graph entries described by the header.
inline bool fast_counts(const FastHeader &h, size_t &vec_count, size_t &graph_count)
{
  if (h.dim <= 0 || h.n <= 0 || h.graph_degree <= 0) { return false; }
  const uint64_t n = static_cast<uint64_t>(h.n);
  const uint64_t dim = static_cast<uint64_t>(h.dim);
  const uint64_t gd = static_cast<uint64_t>(h.graph_degree);
  const uint64_t max_size = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
  if (n > max_size / sizeof(float) / dim) { return false; }
  if (n > max_size / sizeof(unsigned int) / gd) { return false; }
  vec_count = static_cast<size_t>(n * dim);
  graph_count = static_cast<size_t>(n * gd);
  return true;
}

// Writes the OBCUVS03 container: magic, header, ids, vectors, graph.
inline bool write_fast(std::ostream &out, const FastHeader &h, const int64_t *ids,
                       const float *vectors, const unsigned int *graph)
{
  size_t vec_count = 0;
  size_t graph_count = 0;
  if (!fast_counts(h, vec_count, graph_count)) { return false; }
  if (ids == nullptr || vectors == nullptr || graph == nullptr) { return false; }
  if (!write_bytes(out, kMagicFast, sizeof(kMagicFast))) { return false; }
  if (!write_bytes(out, &h.dim, sizeof(h.dim))) { return false; }
  if (!write_bytes(out, &h.reserved, sizeof(h.reserved))) { return false; }
  if (!write_bytes(out, &h.n, sizeof(h.n))) { return false; }
  if (!write_bytes(out, &h.graph_degree, sizeof(h.graph_degree))) { return false; }
  if (!write_bytes(out, &h.metric, sizeof(h.metric))) { return false; }
  if (!write_bytes(out, ids, static_cast<size_t>(h.n) * sizeof(int64_t))) { return false; }
  if (!write_bytes(out, vectors, vec_count * sizeof(float))) { return false; }
  if (!write_bytes(out, graph, graph_count * sizeof(unsigned int))) { return false; }
  return out.good();
}

// Reads and validates the OBCUVS03 header. The magic must already be
// consumed by the caller.
inline bool read_fast_header(std::istream &in, FastHeader &h, int expected_dim)
{
  if (!read_bytes(in, &h.dim, sizeof(h.dim))) { return false; }
  if (!read_bytes(in, &h.reserved, sizeof(h.reserved))) { return false; }
  if (!read_bytes(in, &h.n, sizeof(h.n))) { return false; }
  if (!read_bytes(in, &h.graph_degree, sizeof(h.graph_degree))) { return false; }
  if (!read_bytes(in, &h.metric, sizeof(h.metric))) { return false; }
  if (h.dim <= 0 || h.dim != expected_dim || h.reserved != 0 || h.n <= 0 ||
      h.graph_degree <= 0 ||
      static_cast<uint64_t>(h.n) > std::numeric_limits<uint32_t>::max() ||
      static_cast<uint64_t>(h.graph_degree) > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  size_t vec_count = 0;
  size_t graph_count = 0;
  return fast_counts(h, vec_count, graph_count);
}

// Reads the OBCUVS03 payload: ids, vectors, graph. The buffers must have
// been sized with fast_counts() for the header returned by
// read_fast_header().
inline bool read_fast_payload(std::istream &in, const FastHeader &h, int64_t *ids,
                              float *vectors, unsigned int *graph)
{
  size_t vec_count = 0;
  size_t graph_count = 0;
  if (!fast_counts(h, vec_count, graph_count)) { return false; }
  if (ids == nullptr || vectors == nullptr || graph == nullptr) { return false; }
  if (!read_bytes(in, ids, static_cast<size_t>(h.n) * sizeof(int64_t))) { return false; }
  if (!read_bytes(in, vectors, vec_count * sizeof(float))) { return false; }
  if (!read_bytes(in, graph, graph_count * sizeof(unsigned int))) { return false; }
  return true;
}

}  // namespace ob_cuvs_snapshot

#endif  // OB_CUVS_SNAPSHOT_H
