# Fast snapshot path for the GPU (cuVS/CAGRA) vector index

Status: implemented on top of the frozen contest baseline
(`amd-oceanbase-official-contest-base-v5`). The change is **recall neutral**:
it does not touch the graph construction parameters, the search parameters, the
metric, or the dataset, so the ANN results are bit-for-bit the same as the
baseline's.

## 1. What the baseline does

The GPU path in `ob_vsag_adaptor.cpp` keeps the accumulated vectors in
`ObCuvsEntry::buf_vecs_` and, when the index is snapshotted:

1. builds a CAGRA index on the GPU (`seekdb_cuvs_build`),
2. serializes it with `seekdb_cuvs_serialize`, which writes a **cuVS blob** to
   a temporary file and then copies that blob into the seekdb snapshot stream,
3. stores seekdb's own copy of the ids and the vectors in the same stream.

Two consequences:

- **The dataset is written twice.** The cuVS file format embeds the dataset in
  the index blob, and seekdb also keeps the raw vectors because the CPU VSAG
  fallback needs them.
- **The blob travels through the filesystem twice.** It is written to a temp
  file, copied into the snapshot stream, and on reload it is read back from
  the snapshot stream into a temp file again before `cuvsCagraDeserialize`
  reads it.

For the contest shapes (`n = 250000`, `dim = 128`, graph degree `128`) the
snapshot container holds:

| container | ids | vectors | graph | cuVS blob | total |
| --- | --- | --- | --- | --- | --- |
| baseline (`OBCUVS02`) | 2 MB | 128 MB | 128 MB (inside the blob) | 256 MB | **386 MB** |
| fast (`OBCUVS03`) | 2 MB | 128 MB | 128 MB | – | **258 MB** |
| fast + lossless codecs (`OBCUVS04`) | 2 MB | **32 MB** | **96 MB** | – | **130 MB** |

`docs/gpu-vector-index-hipvs-cuvs/validation/test_fast_snapshot.cpp` prints these numbers; `OBCUVS03`
saves 33.2 % of the container bytes and drops the temp-file staging of the blob
entirely, `OBCUVS04` saves a further 49.6 % on top of it (see §2.4).

## 2. What the fast path does

Instead of shipping a cuVS blob, the fast path ships only the **knn graph**:

```
OBCUVS04 | dim | vec_codec | n | graph_degree | metric | graph_codec | ids[n] | vectors[n*dim] | graph[n*degree]
```

`vec_codec` and `graph_codec` name the encoding of the two big blocks (see
§2.4); the container is therefore **self describing** and needs no external
parameter to be read back.

The index is then re-created from `(graph, dataset)` with
`cuvsCagraIndexFromArgs()`.

### 2.1 Why the result is identical

The argument is taken from the upstream sources at the pinned hipVS commit
`f96a663d0a4441d6f5344bd3ef552b71ae7b370b`:

1. `cagra::build()` (`cpp/src/neighbors/cagra_build.cuh`) runs nn-descent into
   an intermediate host graph, then `optimize()` into `cagra_graph`, a **host**
   matrix of degree `params.graph_degree`. It then constructs the index with
   `index(res, params.metric, dataset, raft::make_const_mdspan(cagra_graph.view()))`.
2. The `index` constructor (`cpp/include/cuvs/neighbors/cagra.hpp`) does
   `dataset_(make_aligned_dataset(res, dataset, 16))` and `update_graph(res, knn_graph)`.
   The **host** overload of `update_graph` copies the graph to the device into
   `graph_` and sets `graph_view_ = graph_.view()`; it does not modify it.
3. `cuvsCagraIndexGetGraph` (`cpp/src/neighbors/cagra_c.cpp`) returns
   `to_dlpack(index_ptr->graph(), graph)`, and `index_ptr->graph()` is exactly
   `graph_view_` — the **optimized search graph**, with the final degree.
4. `cuvsCagraIndexFromArgs` (`_from_args` in the same file) constructs
   `index(res, metric, dataset_mds, graph_mds)`. The **device** overload of
   `update_graph` only stores the reference (`graph_view_ = knn_graph`), and
   `make_aligned_dataset` copies the same vectors to the device.

So the restored index searches **the same graph** over **the same vector
values** as the index that was built. The distances are computed by the same
kernel over the same device buffers, hence the neighbour ids — and therefore
the recall — are unchanged.

Two details that this depends on:

- `to_dlpack` sets `strides = nullptr` for a row-major matrix and attaches a
  deleter that releases the host-side shape metadata. The bridge therefore
  copies the graph with a plain `cudaMemcpy` and calls the deleter afterwards.
- The metric is taken from the **built handle** (`seekdb_cuvs_index_metric`),
  not from a global default, so the restored index always uses the same
  distance function.

### 2.2 Self-check before enabling it

`seekdb_cuvs_supports_fast_snapshot()` runs a self-contained probe on the
first call: it builds a tiny index (256 x 16), exports the graph, restores it
and compares the search results. Two independent conditions must hold:

1. the restored index returns the **same ids** (and matching distances) as the
   built one — this is the property the fast path relies on;
2. those results are **genuine neighbours**: the ids are in range, distinct,
   and the reported distance matches the host-computed squared L2 distance.

If either fails — or if the linked cuVS does not export the required entry
points — the fast path stays disabled and seekdb keeps the baseline cuVS blob
format. The decision is logged once per process:

```text
[seekdb_cuvs] fast_snapshot_api graph=1 degree=1 dataset=1 from_args=1
[seekdb_cuvs] fast_snapshot_supported=1
```

### 2.3 ABI safety

The three cuVS entry points that the baseline does not use
(`cuvsCagraIndexGetGraph`, `cuvsCagraIndexGetGraphDegree`,
`cuvsCagraIndexFromArgs`) are resolved with `dlsym()` instead of being linked
directly. A hipVS build that does not export them keeps working: the bridge
falls back to the cuVS file format, so the server still links and still serves
GPU queries.

### 2.4 Lossless compression of the two big blocks

The contest shapes are dominated by two blocks: the dataset (250000 x 128
float32 = 128 MB) and the knn graph (250000 x 128 neighbour ids = 128 MB).
`OBCUVS04` stores both in a **lossless** codec that the writer *detects* and
records in the header:

| block | codec | encoding | selected when |
| --- | --- | --- | --- |
| vectors | `VEC_I8 = 1` | 1 byte per component | every component is an integer in [-128, 127] |
| vectors | `VEC_I16 = 2` | 2 bytes per component | every component is an integer in [-32768, 32767] |
| vectors | `VEC_F32 = 4` | 4 bytes per component | anything else (identical to `OBCUVS03`) |
| graph | `GRAPH_U24 = 3` | 3 bytes per neighbour id | every neighbour id is < 2^24 |
| graph | `GRAPH_U32 = 4` | 4 bytes per neighbour id | anything else (identical to `OBCUVS03`) |

The property that matters: **the codec is only chosen when the round trip is
bit exact.** A component that cannot be represented exactly falls back to the
next codec, and the last fallback is the original float32 layout. The graph is
a list of `uint32` ids, so 24 bits store it exactly whenever the ids fit.

Consequences:

- the restored index holds **the same graph over the same vector values**, so
  the recall, the neighbour ids and the query latency are unchanged — the
  change is a **storage** change, not an index change;
- a dataset that is not integer valued simply keeps the float32 codec, so the
  mechanism can never lose precision;
- the container still describes itself, so an `OBCUVS04` reader never has to
  guess how to interpret the payload.

For the contest shapes the two codecs are the int8 one (the SIFT vectors are
integer valued; the benchmark stores int8 data in float32) and the 24 bit one
(250000 neighbours fit in 24 bits), which gives 130,000,040 bytes instead of
258,000,000 — **49.6 % smaller**.

## 3. Files

| file | role |
| --- | --- |
| `tools/hipvs_bridge/seekdb_cuvs_bridge.c` | graph export, index restore, probe |
| `tools/hipvs_bridge/seekdb_cuvs_bridge.h` | the new C ABI |
| `src/oblib/lib/vector/ob_cuvs_snapshot.h` | the `OBCUVS04` container (no OB dependency) |
| `src/oblib/lib/vector/ob_vsag_adaptor.cpp` | fast serialize/deserialize, stage timings |
| `docs/gpu-vector-index-hipvs-cuvs/validation/test_fast_snapshot.cpp` | standalone container unit test |

## 4. Reproducible checks

### 4.1 Offline, no GPU required

```bash
# container round trip, header validation, truncation detection
g++ -std=c++17 -O2 -Wall -Wextra \
    -I src/oblib/lib/vector \
    docs/gpu-vector-index-hipvs-cuvs/validation/test_fast_snapshot.cpp -o /tmp/test_fast_snapshot
/tmp/test_fast_snapshot
```

### 4.2 On the AMD instance

```bash
tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so
```

The build log must contain `fast_snapshot_supported=1`. If it contains
`fast_snapshot_supported=0`, the bridge stayed on the baseline format and the
optimization is not in effect — the reason is printed by
`fast_snapshot_api ...` just above.

Then run the GPU smoke runner and the build benchmark; see
[`build-performance.md`](build-performance.md).

## 5. Rollback

`SEEKDB_CUVS_FAST_SNAPSHOT=0` disables the fast path without a rebuild, and
the `fast_snapshot` key of the bridge configuration file does the same. The
baseline `OBCUVS02` container is still written and still read, so a snapshot
written by one setting can be read by the other.

## 6. Build constraint: the hipVS header only redirects some CUDA names

The bridge compiles against `HIPVS_ROOT/include`. On AMD that pulls in
`cuvs/cuda_runtime.h`, a shim that includes `<hip/hip_runtime.h>` and
`#define`s the CUDA spellings of the HIP ones. **The shim is not complete**:
it maps `cudaMemcpy`, `cudaMemcpyDefault`, `cudaMemcpyDeviceToHost` and
`cudaSuccess`, but it does **not** map `cudaMemcpy2D` (only
`cudaMemcpy2DAsync` is mapped).

A bridge that calls `cudaMemcpy2D` therefore does not compile against the
hipVS headers, and the build stops in the first step — before a single test
case runs. The graph export consequently uses the one-dimensional
`cudaMemcpy` for the padded block and drops the row padding on the host.

Rule for this file: **only use CUDA spellings that the shim actually
redirects.** Check the list in `cuvs/cuda_runtime.h` before adding a runtime
call, and compile against the real header instead of a stub — a stub that
declares `cudaMemcpy2D` hides exactly this class of failure.

