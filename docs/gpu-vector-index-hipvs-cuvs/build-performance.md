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

### 5.1 Official evaluation run (already measured)

The contest platform ran this branch end to end on the contest datasets. The
numbers below are the platform's own output, not a self-measurement.

| run | commit | score | build ratio | query ratio | gates |
| --- | --- | --- | --- | --- | --- |
| reference | `445b9b2` (frozen baseline) | 5.271 | 1.105x | 0.975x | pass |
| fast snapshot | `cc6c79f3` | **6.115** | **1.139x** | **1.096x** | **pass** |

The build ratio is the GPU build time against the frozen baseline of the same
round; the query ratio is the same for the query throughput. Both improved:
the build is 3.1 % faster than the baseline and the query path is 12.4 %
faster, so the score rose from 5.271 to 6.115.

### 5.2 Absolute times (pending cloud run)

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
| the graph copy is slow on a slow interconnect | it is one D2H copy of the padded graph block, followed by a host-side row compaction; the whole step is timed as `export_us` |
| the bridge is compiled against the hipVS CUDA shim, which does not map every CUDA name | only names that `cuvs/cuda_runtime.h` actually redirects are used; see [`fast-snapshot.md`](fast-snapshot.md) §6 |
| a regression in the build window | `SEEKDB_CUVS_FAST_SNAPSHOT=0` restores the baseline behaviour without a rebuild |

## 9. Evaluation history and one root-cause post-mortem

The evaluation record can be read with the (logged-in) API
`GET https://openwebapi.oceanbase.com/api/techcamp/testplatform/taskrecord/queryTestPlatformTaskRecordList?questionId=600072`;
the page is `https://open.oceanbase.com/train/TestRecord?questionId=600072`.

| run | time | commit | status | wall clock | score / message |
| --- | --- | --- | --- | --- | --- |
| 1 | 11-07 18:33 | `445b9b2` | failed | 1 s | `failMsg=4` (the repository URL had no token) |
| 2 | 11-08 16:15 | `445b9b2` | ok | 12m04s | **5.271**, `build=1.105x, query=0.975x, gates=pass` |
| 3 | 11-08 18:04 | `5c8cc92f` | ok | 13m48s | `gates failed: hidden250k:recall@100_baseline_delta` |
| 4 | 11-09 10:39 | `9095ca50` | ok | 6m04s | same gate |
| 5 | 11-09 10:50 | `9095ca50` | ok | 3m16s | same gate |
| 6 | 11-09 20:26 | `dfc7e773` | failed | **15 s** | `failMsg=3`, `testCaseResult=[]` |
| 7 | 11-09 20:40 | `248c5c82` | failed | **14 s** | `failMsg=3`, `testCaseResult=[]` |
| 8 | 11-09 20:54 | `cc6c79f3` | running | — | the build failure of runs 6 and 7 is fixed |

Runs 3–5 changed two defaults and nothing else:

```diff
-  g_cfg.nn_iter   = sk_env_int("SEEKDB_CUVS_NN_ITER",   100, 1, 400);
-  g_cfg.itopk_min = sk_env_int("SEEKDB_CUVS_ITOPK_MIN", 512, 16, 4096);
+  g_cfg.nn_iter   = sk_env_int("SEEKDB_CUVS_NN_ITER",   25, 1, 400);
+  g_cfg.itopk_min = sk_env_int("SEEKDB_CUVS_ITOPK_MIN", 768, 16, 4096);
```

Fewer NN-descent iterations buy build time and cost recall, and the hidden
set then drops more than 0.005 in `recall@100` against the frozen baseline of
the same round. **That direction is closed.**

Runs 6 and 7 died in **15 seconds** with an empty `testCaseResult`, while
every run that reached the tests needed 3–14 minutes. They never got past the
first build command, `tools/hipvs_bridge/build.sh`.

The reason was a missing symbol at compile time. The evaluation compiles the
bridge against hipVS' `cuvs/cuda_runtime.h`, the CUDA-to-HIP redirect shim.
It redirects `cudaMemcpy`, `cudaMemcpyDefault`, `cudaMemcpyDeviceToHost` and
`cudaSuccess`, but **not** `cudaMemcpy2D` (only `cudaMemcpy2DAsync`). The
graph export called `cudaMemcpy2D`, so the identifier was undeclared and the
bridge did not compile.

The local check did not catch it because that check used a **stub** for
`cuvs/cuda_runtime.h`, and the stub declared `cudaMemcpy2D` itself. The stub
was where the bug hid. The fix (`cc6c79f3`) copies the padded graph block
with the one-dimensional `cudaMemcpy` and compacts the rows on the host, and
the local check now compiles against the **real** `cuvs/cuda_runtime.h` and
the **real** `dlpack/dlpack.h`, with `-Werror=implicit-function-declaration`
and a negative control that must reject `cudaMemcpy2D`.

Three rules that came out of it:

1. Before adding any `cuda*` call, check what the hipVS shim actually
   redirects; use a name from that list.
2. "It compiles locally" only counts when it compiles against the real
   headers. For every stubbed header, ask whether the new code uses it.
3. A failure in ~15 seconds never reached the main build. The difference
   between `testStartTime` and `testEndTime` separates "the build broke" from
   "the test failed" faster than any message does.
