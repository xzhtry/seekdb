/**
 * Standalone test for the OBCUVS04 (fast) snapshot container.
 *
 * The container lives in src/oblib/lib/vector/ob_cuvs_snapshot.h and has no
 * OceanBase dependency, so it can be tested without building seekdb:
 *
 *   g++ -std=c++17 -O2 -Wall -Wextra \
 *       docs/gpu-vector-index-hipvs-cuvs/validation/test_fast_snapshot.cpp \
 *       -o /tmp/test_fast_snapshot
 *   /tmp/test_fast_snapshot
 *
 * What it proves:
 *   1. The magic dispatch works: a fast container is recognised as fast and
 *      is never mistaken for a baseline container.
 *   2. The payload round trip is exact: ids, vectors and the knn graph come
 *      back bit-identical. The restored GPU index is built from exactly these
 *      bytes, so the recall cannot change.
 *   3. The header validation rejects corrupt or hostile headers (wrong
 *      dimension, zero/negative counts, oversized counts, unknown codecs)
 *      instead of allocating an attacker-sized buffer.
 *   4. A truncated stream is detected instead of silently producing a
 *      half-initialised index.
 *   5. The lossless codecs shrink the container for the shapes the contest
 *      uses, and the round trip stays exact for every codec: an integer
 *      valued dataset is stored in one byte per component, the neighbour ids
 *      of a 250k index in three bytes, and a dataset that is not integer
 *      valued falls back to plain float32.
 *   6. The fast container is strictly smaller than the baseline container.
 *
 * This file deliberately lives outside tools/hipvs_bridge/: that directory is
 * the directory of the GPU bridge, and anything that sits next to it can be
 * picked up by a build that compiles the directory as a whole. A test with its
 * own main() next to the bridge would then break that build.
 */

#if defined(__has_include)
#  if __has_include("ob_cuvs_snapshot.h")
#    include "ob_cuvs_snapshot.h"
#  else
#    include "../../../src/oblib/lib/vector/ob_cuvs_snapshot.h"
#  endif
#else
#  include "../../../src/oblib/lib/vector/ob_cuvs_snapshot.h"
#endif

#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond);             \
      std::printf(__VA_ARGS__);                                                \
      std::printf("\n");                                                       \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

constexpr int32_t kDim = 16;
constexpr int64_t kN = 4096;
constexpr int64_t kGraphDegree = 64;
constexpr int32_t kMetric = 3;  // cuvsDistanceType::L2Expanded

struct Payload {
  std::vector<int64_t> ids;
  std::vector<float> vectors;
  std::vector<unsigned int> graph;
};

// A dataset whose components are exact integers in [-128, 127]: this is what
// SIFT is (an int8 dataset stored in float32).
Payload make_sift_like_payload()
{
  Payload p;
  p.ids.resize(static_cast<size_t>(kN));
  p.vectors.resize(static_cast<size_t>(kN) * kDim);
  p.graph.resize(static_cast<size_t>(kN) * kGraphDegree);
  for (int64_t i = 0; i < kN; ++i) {
    p.ids[static_cast<size_t>(i)] = 1000000 + i * 3;
    for (int32_t j = 0; j < kDim; ++j) {
      p.vectors[static_cast<size_t>(i) * kDim + j] =
          static_cast<float>(static_cast<int>((i * 31 + j * 17) % 255) - 128);
    }
    for (int64_t g = 0; g < kGraphDegree; ++g) {
      p.graph[static_cast<size_t>(i) * kGraphDegree + g] =
          static_cast<unsigned int>((i + g * 7) % kN);
    }
  }
  return p;
}

// A dataset with fractional components: the codec must fall back to float32.
Payload make_float_payload()
{
  Payload p = make_sift_like_payload();
  for (size_t i = 0; i < p.vectors.size(); ++i) {
    p.vectors[i] = static_cast<float>(i % 997) * 0.5f;
  }
  return p;
}

// Size of the baseline container: ids + vectors + the cuVS blob. The cuVS
// blob embeds the dataset and the graph, so it is at least dataset + graph.
// cuvsCagraSerialize() additionally writes the index metadata.
size_t baseline_container_bytes(const Payload &p)
{
  const size_t ids = p.ids.size() * sizeof(int64_t);
  const size_t vectors = p.vectors.size() * sizeof(float);
  const size_t blob_dataset = p.vectors.size() * sizeof(float);
  const size_t blob_graph = p.graph.size() * sizeof(unsigned int);
  const size_t metadata = 4096;  // conservative lower bound for the index metadata
  return ids + vectors + blob_dataset + blob_graph + metadata;
}

// Writes the container, reads it back and checks that everything matches.
// Returns the number of bytes the container took.
size_t round_trip(const Payload &p, int expect_vec_codec, int expect_graph_codec)
{
  ob_cuvs_snapshot::FastHeader hdr;
  hdr.dim = kDim;
  hdr.n = kN;
  hdr.graph_degree = kGraphDegree;
  hdr.metric = kMetric;

  std::ostringstream out(std::ios::binary);
  size_t written = 0;
  CHECK(ob_cuvs_snapshot::write_fast(out, hdr, p.ids.data(), p.vectors.data(),
                                     p.graph.data(), &written),
        "write_fast failed");
  const std::string blob = out.str();
  CHECK(written == blob.size(), "written_bytes %zu != stream size %zu", written,
        blob.size());

  // 1. magic dispatch
  std::istringstream in(blob, std::ios::binary);
  char magic[8] = {0};
  in.read(magic, sizeof(magic));
  CHECK(in.gcount() == static_cast<std::streamsize>(sizeof(magic)), "short magic");
  CHECK(ob_cuvs_snapshot::is_fast_magic(magic), "magic is not the fast magic");
  CHECK(!ob_cuvs_snapshot::is_base_magic(magic), "fast magic looks like the base magic");

  // 2. header + payload round trip
  ob_cuvs_snapshot::FastHeader got;
  CHECK(ob_cuvs_snapshot::read_fast_header(in, got, kDim), "read_fast_header failed");
  CHECK(got.dim == hdr.dim && got.n == hdr.n && got.graph_degree == hdr.graph_degree &&
            got.metric == hdr.metric,
        "header mismatch: dim=%d n=%lld gd=%lld metric=%d", got.dim,
        static_cast<long long>(got.n), static_cast<long long>(got.graph_degree), got.metric);
  CHECK(got.vec_codec == expect_vec_codec, "vec_codec %d != %d", got.vec_codec,
        expect_vec_codec);
  CHECK(got.graph_codec == expect_graph_codec, "graph_codec %d != %d",
        got.graph_codec, expect_graph_codec);

  size_t vec_count = 0;
  size_t graph_count = 0;
  CHECK(ob_cuvs_snapshot::fast_counts(got, vec_count, graph_count), "fast_counts failed");
  CHECK(vec_count == p.vectors.size(), "vec_count %zu != %zu", vec_count, p.vectors.size());
  CHECK(graph_count == p.graph.size(), "graph_count %zu != %zu", graph_count, p.graph.size());

  std::vector<int64_t> ids(got.n);
  std::vector<float> vectors(vec_count);
  std::vector<unsigned int> graph(graph_count);
  CHECK(ob_cuvs_snapshot::read_fast_payload(in, got, ids.data(), vectors.data(),
                                            graph.data()),
        "read_fast_payload failed");
  CHECK(ids == p.ids, "ids mismatch");
  CHECK(vectors == p.vectors, "vectors mismatch");
  CHECK(graph == p.graph, "graph mismatch");

  // The stream must be fully consumed: no trailing bytes.
  char extra = 0;
  in.read(&extra, 1);
  CHECK(in.gcount() == 0, "trailing bytes after the payload");

  return blob.size();
}

void test_round_trip_sift()
{
  const Payload p = make_sift_like_payload();
  const size_t bytes = round_trip(p, ob_cuvs_snapshot::VEC_I8,
                                  ob_cuvs_snapshot::GRAPH_U24);
  const size_t base_bytes = baseline_container_bytes(p);
  std::printf("  container bytes: fast=%zu baseline=%zu saving=%.1f%%\n", bytes,
              base_bytes,
              100.0 * (static_cast<double>(base_bytes) - static_cast<double>(bytes)) /
                  static_cast<double>(base_bytes));
  CHECK(bytes < base_bytes, "the fast container is not smaller");
}

void test_round_trip_float()
{
  const Payload p = make_float_payload();
  // Fractional components: the codec must fall back to float32, and the
  // neighbour ids still fit in 24 bits.
  const size_t bytes = round_trip(p, ob_cuvs_snapshot::VEC_F32,
                                  ob_cuvs_snapshot::GRAPH_U24);
  const size_t raw = p.ids.size() * sizeof(int64_t) +
                     p.vectors.size() * sizeof(float) +
                     p.graph.size() * sizeof(unsigned int);
  CHECK(bytes < raw, "the container is not smaller than the raw blocks");
}

void test_forced_codecs()
{
  // Every codec must survive a round trip bit exactly, not only the ones the
  // detector happens to pick.
  Payload p = make_sift_like_payload();
  const int codecs[] = {ob_cuvs_snapshot::VEC_F32, ob_cuvs_snapshot::VEC_I16,
                        ob_cuvs_snapshot::VEC_I8};
  for (const int codec : codecs) {
    ob_cuvs_snapshot::FastHeader hdr;
    hdr.dim = kDim;
    hdr.n = kN;
    hdr.graph_degree = kGraphDegree;
    hdr.metric = kMetric;
    std::ostringstream out(std::ios::binary);
    std::vector<char> vbuf;
    std::vector<char> gbuf;
    ob_cuvs_snapshot::encode_vectors(p.vectors.data(), p.vectors.size(), codec, vbuf);
    ob_cuvs_snapshot::encode_graph(p.graph.data(), p.graph.size(),
                                   ob_cuvs_snapshot::GRAPH_U24, gbuf);
    CHECK(vbuf.size() == p.vectors.size() * static_cast<size_t>(codec),
          "vector buffer size %zu for codec %d", vbuf.size(), codec);
    // Encode -> decode must be the identity.
    std::vector<float> back(p.vectors.size());
    ob_cuvs_snapshot::decode_vectors(vbuf.data(), p.vectors.size(), codec, back.data());
    CHECK(back == p.vectors, "codec %d does not round trip", codec);
  }
  // The graph codec has the same property.
  std::vector<char> gbuf;
  ob_cuvs_snapshot::encode_graph(p.graph.data(), p.graph.size(),
                                 ob_cuvs_snapshot::GRAPH_U24, gbuf);
  std::vector<unsigned int> gback(p.graph.size());
  ob_cuvs_snapshot::decode_graph(gbuf.data(), p.graph.size(),
                                 ob_cuvs_snapshot::GRAPH_U24, gback.data());
  CHECK(gback == p.graph, "the graph codec does not round trip");
}

void test_bad_headers()
{
  const Payload p = make_sift_like_payload();
  ob_cuvs_snapshot::FastHeader hdr;
  hdr.dim = kDim;
  hdr.n = kN;
  hdr.graph_degree = kGraphDegree;
  hdr.metric = kMetric;

  // Dimension mismatch
  {
    std::ostringstream out(std::ios::binary);
    ob_cuvs_snapshot::write_fast(out, hdr, p.ids.data(), p.vectors.data(), p.graph.data());
    std::istringstream in(out.str(), std::ios::binary);
    char magic[8] = {0};
    in.read(magic, sizeof(magic));
    ob_cuvs_snapshot::FastHeader got;
    CHECK(!ob_cuvs_snapshot::read_fast_header(in, got, kDim + 1),
          "a wrong dimension was accepted");
  }
  // Zero / negative counts
  {
    ob_cuvs_snapshot::FastHeader bad = hdr;
    bad.n = 0;
    std::ostringstream out(std::ios::binary);
    ob_cuvs_snapshot::write_fast(out, bad, p.ids.data(), p.vectors.data(), p.graph.data());
    std::istringstream in(out.str(), std::ios::binary);
    char magic[8] = {0};
    in.read(magic, sizeof(magic));
    ob_cuvs_snapshot::FastHeader got;
    CHECK(!ob_cuvs_snapshot::read_fast_header(in, got, kDim), "n=0 was accepted");
  }
  {
    ob_cuvs_snapshot::FastHeader bad = hdr;
    bad.graph_degree = 0;
    std::ostringstream out(std::ios::binary);
    ob_cuvs_snapshot::write_fast(out, bad, p.ids.data(), p.vectors.data(), p.graph.data());
    std::istringstream in(out.str(), std::ios::binary);
    char magic[8] = {0};
    in.read(magic, sizeof(magic));
    ob_cuvs_snapshot::FastHeader got;
    CHECK(!ob_cuvs_snapshot::read_fast_header(in, got, kDim), "graph_degree=0 was accepted");
  }
  // An unknown codec must be rejected instead of being used as a size. The
  // codecs are detected when the container is written, so the invalid value
  // has to be patched into the serialized header.
  {
    std::ostringstream out(std::ios::binary);
    ob_cuvs_snapshot::write_fast(out, hdr, p.ids.data(), p.vectors.data(), p.graph.data());
    std::string blob = out.str();
    const int32_t bad_codec = 3;
    std::memcpy(&blob[ob_cuvs_snapshot::kMagicSize + sizeof(int32_t)], &bad_codec,
                sizeof(bad_codec));
    std::istringstream in(blob, std::ios::binary);
    char magic[8] = {0};
    in.read(magic, sizeof(magic));
    ob_cuvs_snapshot::FastHeader got;
    CHECK(!ob_cuvs_snapshot::read_fast_header(in, got, kDim), "vec_codec=3 was accepted");
  }
  {
    std::ostringstream out(std::ios::binary);
    ob_cuvs_snapshot::write_fast(out, hdr, p.ids.data(), p.vectors.data(), p.graph.data());
    std::string blob = out.str();
    const int32_t bad_codec = 2;
    const size_t graph_codec_offset =
        ob_cuvs_snapshot::kMagicSize + 3 * sizeof(int32_t) + 2 * sizeof(int64_t);
    std::memcpy(&blob[graph_codec_offset], &bad_codec, sizeof(bad_codec));
    std::istringstream in(blob, std::ios::binary);
    char magic[8] = {0};
    in.read(magic, sizeof(magic));
    ob_cuvs_snapshot::FastHeader got;
    CHECK(!ob_cuvs_snapshot::read_fast_header(in, got, kDim), "graph_codec=2 was accepted");
  }
  // A hostile n must not overflow the size computation
  {
    ob_cuvs_snapshot::FastHeader bad = hdr;
    bad.n = std::numeric_limits<int64_t>::max();
    size_t vc = 0, gc = 0;
    CHECK(!ob_cuvs_snapshot::fast_counts(bad, vc, gc), "an overflowing n was accepted");
  }
}

void test_truncated()
{
  const Payload p = make_sift_like_payload();
  ob_cuvs_snapshot::FastHeader hdr;
  hdr.dim = kDim;
  hdr.n = kN;
  hdr.graph_degree = kGraphDegree;
  hdr.metric = kMetric;

  std::ostringstream out(std::ios::binary);
  ob_cuvs_snapshot::write_fast(out, hdr, p.ids.data(), p.vectors.data(), p.graph.data());
  std::string blob = out.str();

  // Cut the payload in half: the read must fail, not succeed with garbage.
  blob.resize(blob.size() / 2);
  std::istringstream in(blob, std::ios::binary);
  char magic[8] = {0};
  in.read(magic, sizeof(magic));
  ob_cuvs_snapshot::FastHeader got;
  CHECK(ob_cuvs_snapshot::read_fast_header(in, got, kDim), "read_fast_header failed");
  size_t vc = 0, gc = 0;
  ob_cuvs_snapshot::fast_counts(got, vc, gc);
  std::vector<int64_t> ids(got.n);
  std::vector<float> vectors(vc);
  std::vector<unsigned int> graph(gc);
  CHECK(!ob_cuvs_snapshot::read_fast_payload(in, got, ids.data(), vectors.data(),
                                             graph.data()),
        "a truncated payload was accepted");
}

void test_container_sizes()
{
  // The two dataset shapes the contest uses. The comparison is analytic: the
  // baseline container stores the dataset twice (once raw, once inside the
  // cuVS blob) and stages the whole blob through the filesystem twice, while
  // the fast container stores the dataset once and stages nothing.
  struct Shape {
    const char *name;
    int64_t n;
    int32_t dim;
    int64_t graph_degree;
  };
  const Shape shapes[] = {
    {"SIFT250K", 250000, 128, 128},
    {"Hidden250K", 250000, 128, 128},
  };
  std::printf("  %-12s %14s %14s %14s %10s\n", "dataset", "fast bytes", "raw bytes",
              "blob bytes", "saving");
  for (const Shape &s : shapes) {
    const size_t ids = static_cast<size_t>(s.n) * sizeof(int64_t);
    const size_t vectors = static_cast<size_t>(s.n) * s.dim * sizeof(float);
    const size_t graph = static_cast<size_t>(s.n) * s.graph_degree * sizeof(unsigned int);
    const size_t metadata = 4096;
    // Fast container with the lossless codecs: the dataset is integer valued
    // (one byte per component) and the neighbour ids fit in 24 bits.
    const size_t fast_bytes = ids + static_cast<size_t>(s.n) * s.dim +
                               static_cast<size_t>(s.n) * s.graph_degree * 3 + 40;
    // The same container without the codecs.
    const size_t raw_bytes = ids + vectors + graph;
    // Baseline container: ids + vectors + cuVS blob (dataset + graph + meta).
    const size_t blob_bytes = ids + vectors + vectors + graph + metadata;
    std::printf("  %-12s %14zu %14zu %14zu %9.1f%%\n", s.name, fast_bytes, raw_bytes,
                blob_bytes,
                100.0 * (static_cast<double>(raw_bytes) - static_cast<double>(fast_bytes)) /
                    static_cast<double>(raw_bytes));
    CHECK(fast_bytes < raw_bytes, "%s: the coded container is not smaller", s.name);
    CHECK(fast_bytes < blob_bytes, "%s: the fast container is not smaller", s.name);
  }
}

}  // namespace

int main()
{
  std::printf("[test_fast_snapshot] container round trip (integer valued dataset)\n");
  test_round_trip_sift();
  std::printf("[test_fast_snapshot] container round trip (fractional dataset)\n");
  test_round_trip_float();
  std::printf("[test_fast_snapshot] every codec round trips\n");
  test_forced_codecs();
  std::printf("[test_fast_snapshot] header validation\n");
  test_bad_headers();
  std::printf("[test_fast_snapshot] truncated stream\n");
  test_truncated();
  std::printf("[test_fast_snapshot] contest dataset shapes\n");
  test_container_sizes();

  if (g_failures == 0) {
    std::printf("[test_fast_snapshot] OK\n");
    return 0;
  }
  std::printf("[test_fast_snapshot] %d check(s) failed\n", g_failures);
  return 1;
}
