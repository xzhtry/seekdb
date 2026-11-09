# CPU / GPU vector-index build performance

Scope: the **timed build window** of the contest, i.e. the wall time of

```sql
CREATE VECTOR INDEX idx ON tbl(v)
  WITH (distance=l2, type=hnsw, lib=<cuvs|vsag>, m=16, ef_construction=200, ef_search=64);
```

on a table that is already populated. The only variable between the two
measurements is `lib`: both runs load byte-identical rows into two separate
tables, so the CPU (VSAG) and the GPU (cuVS) builds are directly comparable.

Everything in this document is either

- **[measured]** — produced by a script in this repository on the AMD
  instance, with the command that reproduces it, or
- **[model]** — derived analytically from the sources and the container
  layout, clearly labelled as such.

Nothing here is estimated and presented as a measurement.

## 1. How to reproduce

### 1.1 Build

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  tools/hipvs_bridge/build.sh /work/bridge/libseekdb_cuvs_bridge.so

cmake -S . -B build_release -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
  -DCUVS_BRIDGE_LIB=/work/bridge/libseekdb_cuvs_bridge.so
cmake --build build_release --target seekdb
```

### 1.2 Run the benchmark

```bash
docs/gpu-vector-index-hipvs-cuvs/validation/run_build_benchmark.sh \
  --observer /path/to/build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/seekdb-bench \
  --port 2981 \
  --evidence /tmp/seekdb-bench-evidence \
  --rows 250000 --dim 128 --repeat 3 --topk 64
```

The script writes `benchmark.json` and `benchmark.md` into the evidence
directory. Each repetition uses a **fresh pair of tables**, so no index, graph
or cached build from an earlier repetition can be reused. Pass `--data
sift_base.f32` to use a real `.f32` matrix instead of the synthetic dataset
(the loader accepts a raw row-major float32 file of `rows*dim` values).

### 1.3 Run the GPU correctness smoke test

```bash
docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh \
  --observer /path/to/build_release/src/observer/seekdb \
  --bridge /work/bridge/libseekdb_cuvs_bridge.so \
  --base-dir /tmp/seekdb-cuvs-smoke \
  --port 2981 \
  --render /dev/dri/renderD135 \
  --evidence /tmp/seekdb-cuvs-evidence
```

This is the gate-side check: it asserts that `lib=cuvs` really builds and
serves on the GPU, that `lib=vsag` stays on the CPU, that the non-L2 DDL is
rejected, that filter/delete/freshness fall back correctly, that
`batch_knn` returns the right cardinality and recall, and that the observer
holds the GPU device file descriptors.

## 2. Cost model of the build window

The GPU build window is not just the graph construction. The relevant stages,
all of which the adaptor now times and logs, are:

| stage | what happens | baseline | fast path |
| --- | --- | --- | --- |
| `build` | nn-descent + `optimize()` + graph upload (GPU) | yes | unchanged |
| `export` | copy the knn graph to the host | – | **new** |
| `write` | write the snapshot container | ids + vectors + **cuVS blob** | ids + vectors + **graph** |
| `restore` | re-create the GPU index from the snapshot | read the blob from a temp file | build the index from (graph, dataset) |

Each stage is emitted as one greppable line:

```text
[OBVSAG][cuVS] stage op=serialize   path=fast ... build_us=... export_us=... write_us=... total_us=...
[OBVSAG][cuVS] stage op=serialize   path=blob  ... build_us=... export_us=... write_us=... total_us=...
[OBVSAG][cuVS] stage op=deserialize path=fast ... read_us=... restore_us=... total_us=...
[OBVSAG][cuVS] stage op=deserialize path=blob  ... read_us=... restore_us=... total_us=...
```

and the bridge logs the GPU-side stages:

```text
[seekdb_cuvs] build n=... dim=... gd=... igd=... niter=... algo=... ms=...
[seekdb_cuvs] export_graph n=... gd=... ms=...
[seekdb_cuvs] restore n=... dim=... gd=... metric=... ms=...
```

Collect them with:

```bash
grep -a '\[OBVSAG\]\[cuVS\] stage\|\[seekdb_cuvs\]' /tmp/seekdb-bench-evidence/observer.log
```

The `build` stage is the same in both containers — the fast path does not
change the graph construction at all. The saving comes entirely from the
`write` stage (and, on reload, from the `restore` stage).

## 3. What the fast path changes, in bytes

**[measured]** — output of `docs/gpu-vector-index-hipvs-cuvs/validation/test_fast_snapshot.cpp`, which
runs offline without a GPU:

```text
[test_fast_snapshot] contest dataset shapes
  dataset          fast bytes     blob bytes     saving
  SIFT250K          258000000      386004096      33.2%
  Hidden250K        258000000      386004096      33.2%
```

| | baseline (`OBCUVS02`) | fast (`OBCUVS03`) |
| --- | --- | --- |
| dataset copies | 2 (raw + inside the cuVS blob) | 1 (raw) |
| snapshot container | 386 MB | 258 MB |
| temp-file staging of the blob | 1 write + 1 read on save, 1 write + 1 read on reload | none |

Reproduce with:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -I src/oblib/lib/vector \
    docs/gpu-vector-index-hipvs-cuvs/validation/test_fast_snapshot.cpp -o /tmp/test_fast_snapshot
/tmp/test_fast_snapshot
```

**[model]** — what that means for the timed window, using the SIFT250K shape
(`n = 250000`, `dim = 128`, graph degree `128`):

- the snapshot write drops from 386 MB to 258 MB of bytes produced;
- the 256 MB cuVS blob is no longer staged through a temporary file at all, so
  the save path loses one 256 MB file write plus one 256 MB file read, and the
  reload path loses one 256 MB file write plus one 256 MB file read;
- the restore path replaces `cuvsCagraDeserialize` (which parses the blob and
  uploads the dataset and the graph) with `cuvsCagraIndexFromArgs` (which
  uploads the graph and copies the dataset), i.e. the same GPU transfers
  without the blob parsing.

The wall-clock effect depends on the instance's storage and memory bandwidth,
which is exactly what the stage timings measure. **The end-to-end speedup has
to be measured with the script in §1.2 — it is not inferred here.**

## 4. Baseline reference values

**[measured]** — the frozen contest baseline (`445b9b2`, V5) on the contest
instance (AMD W7900D, ROCm 7.2.4, 24 cores):

| dataset | build time | Recall@10 | Recall@100 | query QPS |
| --- | --- | --- | --- | --- |
| SIFT250K | 20–27 s (median ≈ 23 s) | 1.000 | 1.000 | 147 / 143 |
| Hidden250K | 29–35 s (median ≈ 30 s) | ≈ 0.985 | ≈ 0.914 | 87 / 85 |

Build parameters of that baseline, which the fast path keeps unchanged:
`graph_degree = 128`, `intermediate_graph_degree = 256`,
`nn_descent_niter = 100`, `build_algo = nn_descent`, `itopk_min = 512`,
`search_width = 4`.

## 5. Results

Fill this table from `benchmark.md`, which the script writes automatically.

| dataset | CPU `lib=vsag` build (median) | GPU `lib=cuvs` build (median) | speedup | Recall@10 | Recall@100 |
| --- | --- | --- | --- | --- | --- |
| SIFT250K | *pending cloud run* | *pending cloud run* | *pending* | *pending* | *pending* |
| Hidden250K | *pending cloud run* | *pending cloud run* | *pending* | *pending* | *pending* |

Stage split, to paste from the observer log:

```text
SIFT250K   serialize path=fast  build_us=... export_us=... write_us=... total_us=...
SIFT250K   serialize path=blob   build_us=... export_us=... write_us=... total_us=...
Hidden250K serialize path=fast  build_us=... export_us=... write_us=... total_us=...
Hidden250K serialize path=blob   build_us=... export_us=... write_us=... total_us=...
```

The `path=blob` rows are produced by setting `SEEKDB_CUVS_FAST_SNAPSHOT=0`,
which keeps everything else identical — that is the A/B switch for this
optimization.

## 6. Recall neutrality

The fast path does not touch anything that influences the search:

| | baseline | fast path |
| --- | --- | --- |
| graph construction | nn-descent, `niter=100`, intermediate degree 256 | **identical** |
| final graph | `optimize()` output, degree 128 | **identical** (same graph, exported and re-imported) |
| dataset | the original vectors | **identical** (same values, same order) |
| metric | `L2Expanded` | **identical** (taken from the built handle) |
| search parameters | `itopk_min=512`, `search_width=4` | **identical** |

The restored index searches the same graph over the same vectors, so the
neighbour ids are the same and the recall cannot move. This is additionally
verified at runtime by the probe in
[`fast-snapshot.md`](fast-snapshot.md) §2.2, which refuses to enable the fast
path unless the round trip reproduces the built index's results.

The evaluation gates that this protects:

- SIFT Recall ≥ 0.95; Hidden Recall@10 ≥ 0.90, @100 ≥ 0.85;
- no drop of more than 0.005 against the frozen baseline of the same round;
- query throughput ≥ 90 % of the baseline (the search path is untouched);
- peak GPU memory ≤ 1.25x the baseline (the fast path allocates the graph
  buffer once and keeps it, the baseline allocates the same graph inside the
  index — see §7);
- `lib=vsag` still works (untouched, and exercised by the benchmark script
  and by the smoke runner).

## 7. Resource notes

- The restored index owns one device buffer for the graph
  (`n * graph_degree * 4` bytes = 128 MB for SIFT250K). The baseline's index
  owns the same graph inside the deserialized cuVS index, so the steady-state
  device footprint is the same order; the fast path additionally holds the
  host-side copy of the graph while it is being written.
- The fast path removes the cuVS blob from the snapshot, so the snapshot
  written to disk is 33 % smaller for the contest shapes.
- `cuvsRMMPoolMemoryResourceEnable` is deliberately **not** used: it installs
  a thread-local pool as the global memory resource, which would make the
  peak-memory gate unpredictable.

## 8. Risks and rollbacks

| risk | mitigation |
| --- | --- |
| the linked cuVS does not export the three new entry points | they are resolved with `dlsym`; the bridge falls back to the baseline container |
| the export/restore round trip is not exact on some GPU | the runtime probe fails closed and the baseline container is used |
| the graph copy is slow on a slow interconnect | it is one D2H copy of `n * degree * 4` bytes, timed as `export_us` |
| a regression in the build window | `SEEKDB_CUVS_FAST_SNAPSHOT=0` restores the baseline behaviour without a rebuild |
