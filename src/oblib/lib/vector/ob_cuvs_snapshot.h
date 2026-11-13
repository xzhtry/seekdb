/**
 * Copyright (c) 2025 OceanBase.
 *
 * Snapshot container for the GPU (cuVS / CAGRA) vector index.
 *
 * This header deliberately has no OceanBase dependency: it only talks to
 * std::iostream, so the container format can be exercised by a standalone
 * unit test (see docs/gpu-vector-index-hipvs-cuvs/validation/
 * test_fast_snapshot.cpp) without a full seekdb build.
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
 *   OBCUVS04 (fast path)
 *     ids | vectors | graph
 *   Only the knn graph is exported from the built index, so the dataset is
 *   stored exactly once and no cuVS blob travels through the filesystem.
 *   The restored GPU index is re-created from (graph, dataset) with
 *   cuvsCagraIndexFromArgs(); it holds the same graph and the same dataset
 *   values as the built one, so search results -- and therefore recall --
 *   are unchanged.
 *
 * OBCUVS04 additionally stores the two big blocks in a *lossless* compressed
 * form, because the snapshot is the dominant cost of the timed build window
 * (it is the only thing the window writes to the disk at full size):
 *
 *   - the dataset: the benchmark vectors are float32 whose components are
 *     exact integers in [-128, 127] (SIFT is an int8 dataset stored in
 *     float32), so one byte per component is enough. A dataset that is not
 *     integer valued keeps the plain float32 encoding.
 *   - the knn graph: the neighbour ids of a 250k index fit in 24 bits, so
 *     three bytes per neighbour are enough.
 *
 * Both encodings are detected when the container is written, recorded in the
 * header and therefore self-describing: the reader never has to guess. They
 * are bit-exact round trips, so the restored index is the same index and the
 * recall cannot change.
 */

#ifndef OB_CUVS_SNAPSHOT_H
#define OB_CUVS_SNAPSHOT_H

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>
#include <vector>

namespace ob_cuvs_snapshot {

// Baseline container magic.
constexpr char kMagicBase[8] = {'O', 'B', 'C', 'U', 'V', 'S', '0', '2'};
// Fast container magic. The payload layout changed when the lossless codecs
// were added, so the magic was bumped: an OBCUVS03 container must never be
// read by an OBCUVS04 reader.
constexpr char kMagicFast[8] = {'O', 'B', 'C', 'U', 'V', 'S', '0', '4'};
// Length of both magics.
constexpr size_t kMagicSize = 8;

// Bytes per dataset component in the payload.
enum VectorCodec {
  VEC_F32 = 4,  // plain float32
  VEC_I16 = 2,  // exact integers in [-32768, 32767]
  VEC_I8  = 1   // exact integers in [-128, 127]
};

// Bytes per neighbour id in the payload.
enum GraphCodec {
  GRAPH_U32 = 4,  // plain uint32
  GRAPH_U24 = 3   // neighbour ids below 2^24
};

// Header of the OBCUVS04 container, written right after the magic.
struct FastHeader {
  int32_t dim = 0;           // vector dimension
  int32_t vec_codec = VEC_F32;   // bytes per dataset component in the payload
  int64_t n = 0;                // number of vectors
  int64_t graph_degree = 0;     // neighbours per node in the knn graph
  int32_t metric = 0;           // cuvsDistanceType the index was built with
  int32_t graph_codec = GRAPH_U32;  // bytes per neighbour id in the payload
};

// Size of the header on the wire: 4 + 4 + 8 + 8 + 4 + 4.
constexpr size_t kFastHeaderBytes = 32;

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

// ---------------------------------------------------------------------------
// Lossless codecs
// ---------------------------------------------------------------------------

// Picks the narrowest integer codec that can hold every component exactly.
// A component is representable when it is a finite integer; -0.0 is excluded
// because it would come back as +0.0.
//
// The scan runs inside the timed build window over n*dim components, so it
// avoids the libm calls: the range test rejects NaN and every value that no
// integer codec could hold, and the cast is then safe.
inline int pick_vector_codec(const float *vectors, size_t count)
{
  if (vectors == nullptr || count == 0) { return VEC_F32; }
  float lo = 0.0f;
  float hi = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    const float x = vectors[i];
    if (!(x >= -32768.0f && x <= 32767.0f)) { return VEC_F32; }
    const int32_t iv = static_cast<int32_t>(x);
    if (static_cast<float>(iv) != x) { return VEC_F32; }
    if (x == 0.0f && std::signbit(x)) { return VEC_F32; }
    if (i == 0) {
      lo = x;
      hi = x;
    } else {
      if (x < lo) { lo = x; }
      if (x > hi) { hi = x; }
    }
  }
  if (lo >= -128.0f && hi <= 127.0f) { return VEC_I8; }
  return VEC_I16;
}

// Picks the narrowest neighbour id codec. Ids are row numbers, so a 250k
// index only needs 24 bits.
inline int pick_graph_codec(const unsigned int *graph, size_t count)
{
  if (graph == nullptr || count == 0) { return GRAPH_U32; }
  const uint32_t limit = 1u << 24;
  for (size_t i = 0; i < count; ++i) {
    if (graph[i] >= limit) { return GRAPH_U32; }
  }
  return GRAPH_U24;
}

inline void encode_vectors(const float *vectors, size_t count, int codec,
                           std::vector<char> &out)
{
  out.resize(count * static_cast<size_t>(codec));
  char *p = out.data();
  if (codec == VEC_I8) {
    for (size_t i = 0; i < count; ++i) {
      *p++ = static_cast<char>(static_cast<int8_t>(static_cast<int32_t>(vectors[i])));
    }
  } else if (codec == VEC_I16) {
    for (size_t i = 0; i < count; ++i) {
      const int16_t s = static_cast<int16_t>(static_cast<int32_t>(vectors[i]));
      std::memcpy(p, &s, sizeof(s));
      p += sizeof(s);
    }
  } else {
    std::memcpy(p, vectors, count * sizeof(float));
  }
}

inline void decode_vectors(const char *in, size_t count, int codec, float *out)
{
  const char *p = in;
  if (codec == VEC_I8) {
    for (size_t i = 0; i < count; ++i) {
      out[i] = static_cast<float>(static_cast<int32_t>(static_cast<int8_t>(*p++)));
    }
  } else if (codec == VEC_I16) {
    for (size_t i = 0; i < count; ++i) {
      int16_t s = 0;
      std::memcpy(&s, p, sizeof(s));
      p += sizeof(s);
      out[i] = static_cast<float>(static_cast<int32_t>(s));
    }
  } else {
    std::memcpy(out, in, count * sizeof(float));
  }
}

inline void encode_graph(const unsigned int *graph, size_t count, int codec,
                         std::vector<char> &out)
{
  out.resize(count * static_cast<size_t>(codec));
  char *p = out.data();
  if (codec == GRAPH_U24) {
    for (size_t i = 0; i < count; ++i) {
      const uint32_t v = graph[i];
      *p++ = static_cast<char>(v & 0xFFu);
      *p++ = static_cast<char>((v >> 8) & 0xFFu);
      *p++ = static_cast<char>((v >> 16) & 0xFFu);
    }
  } else {
    std::memcpy(p, graph, count * sizeof(unsigned int));
  }
}

inline void decode_graph(const char *in, size_t count, int codec, unsigned int *out)
{
  const char *p = in;
  if (codec == GRAPH_U24) {
    for (size_t i = 0; i < count; ++i) {
      const uint32_t v = static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16);
      p += 3;
      out[i] = v;
    }
  } else {
    std::memcpy(out, in, count * sizeof(unsigned int));
  }
}

// ---------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------

// Writes the OBCUVS04 container: magic, header, ids, vectors, graph.
// The codecs are detected here and stored in the header, so the container
// describes itself and the reader never has to guess.
// When written_bytes is not null it receives the number of bytes written.
inline bool write_fast(std::ostream &out, const FastHeader &h, const int64_t *ids,
                       const float *vectors, const unsigned int *graph,
                       size_t *written_bytes = nullptr)
{
  size_t vec_count = 0;
  size_t graph_count = 0;
  if (!fast_counts(h, vec_count, graph_count)) { return false; }
  if (ids == nullptr || vectors == nullptr || graph == nullptr) { return false; }
  FastHeader out_h = h;
  out_h.vec_codec = pick_vector_codec(vectors, vec_count);
  out_h.graph_codec = pick_graph_codec(graph, graph_count);
  std::vector<char> vec_buf;
  std::vector<char> graph_buf;
  encode_vectors(vectors, vec_count, out_h.vec_codec, vec_buf);
  encode_graph(graph, graph_count, out_h.graph_codec, graph_buf);
  if (!write_bytes(out, kMagicFast, sizeof(kMagicFast))) { return false; }
  if (!write_bytes(out, &out_h.dim, sizeof(out_h.dim))) { return false; }
  if (!write_bytes(out, &out_h.vec_codec, sizeof(out_h.vec_codec))) { return false; }
  if (!write_bytes(out, &out_h.n, sizeof(out_h.n))) { return false; }
  if (!write_bytes(out, &out_h.graph_degree, sizeof(out_h.graph_degree))) { return false; }
  if (!write_bytes(out, &out_h.metric, sizeof(out_h.metric))) { return false; }
  if (!write_bytes(out, &out_h.graph_codec, sizeof(out_h.graph_codec))) { return false; }
  if (!write_bytes(out, ids, static_cast<size_t>(out_h.n) * sizeof(int64_t))) { return false; }
  if (!write_bytes(out, vec_buf.data(), vec_buf.size())) { return false; }
  if (!write_bytes(out, graph_buf.data(), graph_buf.size())) { return false; }
  if (written_bytes != nullptr) {
    *written_bytes = sizeof(kMagicFast) + kFastHeaderBytes +
                     static_cast<size_t>(out_h.n) * sizeof(int64_t) +
                     vec_buf.size() + graph_buf.size();
  }
  return out.good();
}

// Reads and validates the OBCUVS04 header. The magic must already be
// consumed by the caller.
inline bool read_fast_header(std::istream &in, FastHeader &h, int expected_dim)
{
  if (!read_bytes(in, &h.dim, sizeof(h.dim))) { return false; }
  if (!read_bytes(in, &h.vec_codec, sizeof(h.vec_codec))) { return false; }
  if (!read_bytes(in, &h.n, sizeof(h.n))) { return false; }
  if (!read_bytes(in, &h.graph_degree, sizeof(h.graph_degree))) { return false; }
  if (!read_bytes(in, &h.metric, sizeof(h.metric))) { return false; }
  if (!read_bytes(in, &h.graph_codec, sizeof(h.graph_codec))) { return false; }
  if (h.dim <= 0 || h.dim != expected_dim || h.n <= 0 || h.graph_degree <= 0) {
    return false;
  }
  if (h.vec_codec != VEC_I8 && h.vec_codec != VEC_I16 && h.vec_codec != VEC_F32) {
    return false;
  }
  if (h.graph_codec != GRAPH_U24 && h.graph_codec != GRAPH_U32) { return false; }
  if (static_cast<uint64_t>(h.n) > std::numeric_limits<uint32_t>::max() ||
      static_cast<uint64_t>(h.graph_degree) > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  size_t vec_count = 0;
  size_t graph_count = 0;
  return fast_counts(h, vec_count, graph_count);
}

// Reads the OBCUVS04 payload: ids, vectors, graph. The buffers must have
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
  std::vector<char> vec_buf(vec_count * static_cast<size_t>(h.vec_codec));
  if (!read_bytes(in, vec_buf.data(), vec_buf.size())) { return false; }
  decode_vectors(vec_buf.data(), vec_count, h.vec_codec, vectors);
  std::vector<char> graph_buf(graph_count * static_cast<size_t>(h.graph_codec));
  if (!read_bytes(in, graph_buf.data(), graph_buf.size())) { return false; }
  decode_graph(graph_buf.data(), graph_count, h.graph_codec, graph);
  return true;
}

}  // namespace ob_cuvs_snapshot

#endif  // OB_CUVS_SNAPSHOT_H
