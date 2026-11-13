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
grep -a '\[OBVSAG\]\[cuVS\] stage\|\[seekdb_cuvs\]' <base-dir>/run/log/seekdb.log*
```

**The stage lines are not on stdout.** The observer writes them into its own
syslog file (`<base-dir>/run/log/seekdb.log`), and it reopens that file before
every DDL round while keeping only **one** rotated backup — a grep after a
three-round run therefore misses the first round. The benchmark script works
around this: it sweeps the observer's log directory (including the rotated
files, oldest first) and writes the result to `$EVIDENCE/stages.txt`.

The `build` stage is the same in both containers — the fast path does not
change the graph construction at all. The saving comes entirely from the
`write` stage (and, on reload, from the `restore` stage).

## 3. What the fast path changes, in bytes

**[measured]** — output of `docs/gpu-vector-index-hipvs-cuvs/validation/test_fast_snapshot.cpp`, which
runs offline without a GPU:

```text
[test_fast_snapshot] contest dataset shapes
  dataset          fast bytes      raw bytes     blob bytes     saving
  SIFT250K          130000040      258000000      386004096      49.6%
  Hidden250K        130000040      258000000      386004096      49.6%
```

| | baseline (`OBCUVS02`) | fast (`OBCUVS03`) | fast + codecs (`OBCUVS04`) |
| --- | --- | --- | --- |
| dataset copies | 2 (raw + inside the cuVS blob) | 1 (raw) | 1 (raw) |
| snapshot container | 386 MB | 258 MB | **130 MB** |
| temp-file staging of the blob | 1 write + 1 read on save, 1 write + 1 read on reload | none | none |
| lossless codec | – | – | int8 vectors + 24 bit neighbour ids |

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
| fast snapshot + restore fix | `3794da35` | 5.324 | 1.081x | 1.069x | pass |
| 10 | `e568a8b1` | `igd` 256->128 + `nn_iter` 100->50 + `itopk` 512->1024 + AUTO | 12.369, `build=1.648x, query=1.506x, gates=pass` |
| 11 | `11f2e9f5` | `itopk` clamped to the point count => the fast snapshot takes effect | 15.575, `build=2.120x, query=1.599x, gates=pass` |
| 12 | `5800e6b4` | GPU runtime warm-up | **0.000**, `gates failed: relative_vram_limit` |
| 13 | `5800e6b4` | same commit again (noise vs. systematic) | 15.539, `build=2.114x, query=1.601x, gates=pass` |
| 14 | `e4a56362` | drop the warm-up + `nn_iter` 50->20 | 15.977, `build=2.188x, query=1.583x, gates=pass` (the score to beat at the time, beaten in round 17) |
| 15 | `c60fd055` | LOB pieces 256 KB -> 1 MB | 15.937, `build=2.181x, query=1.550x, gates=pass` |
| 16 | `1d89dcea` | lossless snapshot compression 258 MB -> 130 MB | 15.375, `build=2.099x, query=1.478x, gates=pass` (a regression) |
| **17** | `bcda70fe` | `igd` 128->64 + `gd` 64 + `itopk` 1024->2048 | **20.000 (full marks), `build=3.220x, query=1.699x, gates=pass` (best score)** |

The build ratio is the GPU build time against the frozen baseline of the same
round; the query ratio is the same for the query throughput. Both improved:
the build is 3.1 % faster than the baseline and the query path is 12.4 %
faster, so the score rose from 5.271 to 6.115.

The third row is the same fast snapshot plus exactly one bug fix: the fast
path restored its index on the small (~1.5 MB) OB request thread, which
overflowed the stack at 250,000 vectors, so the restore now runs on a 32 MB
pthread the way the blob path already did (see §5.3). That change is outside
the timed build window and does not touch the graph construction, the search
parameters or the data, so it cannot change the recall - and the platform
confirms it: that run reported `gates=pass` with the fix as its only source
difference.

Two properties of the platform numbers are worth recording:

- **The platform keeps the best score.** The third run scored lower, and the
  second run is still the official best (`maxScoreFlag` is `1` on it and `0`
  on the third one). Neither a failed nor a slower run invalidates an earlier
  good one.
- **The ratios move by a few percent between runs.** The third run changed no
  build- or query-relevant code, yet `build` went from 1.139x to 1.081x and
  `query` from 1.096x to 1.069x. The frozen baseline itself scored 5.271
  with `build=1.105x, query=0.975x`, i.e. the same range. Treat a single
  run's score as noisy: a conclusion about a change needs several runs or a
  same-round baseline.

### 5.2 Absolute times (measured on an AMD Radeon Pro W7900D instance)

The contest datasets are evaluation assets and cannot be fetched, so the main
table uses a **250,000 x 128 synthetic Gaussian set** (same shape as the contest
sets, same seed, a fresh table per repetition) and the cross-check uses the
**10,000 x 128 real set** that ships in the image (with its official queries
and ground truth).

> The synthetic data has no cluster structure and is much harder to index, so
> **the absolute seconds are not comparable with the real SIFT baseline in §4**.
> What is comparable is the ratio and the stage split. The platform's own
> numbers are in §5.1.

**Build time** (3 repetitions, a fresh table each time; the index parameters
are exactly the contest ones - `type=hnsw, m=16, ef_construction=200,
ef_search=64, distance=l2`):

| path | CPU `lib=vsag` (median) | GPU `lib=cuvs` (median) | speedup |
| --- | --- | --- | --- |
| fast `SEEKDB_CUVS_FAST_SNAPSHOT=1` | 159.15 s | 46.27 s | **3.44x** |
| baseline `SEEKDB_CUVS_FAST_SNAPSHOT=0` | 160.36 s | 46.76 s | **3.43x** |

An independent repetition of the same A/B gives 3.36x (fast) and 3.40x
(baseline) - the conclusion reproduces.

**Stage split** (inside the `CREATE VECTOR INDEX` timing window, median of 3):

| path | build (GPU graph) | export (graph to host) | write (snapshot) | total | build share |
| --- | --- | --- | --- | --- | --- |
| fast `path=fast` | 42.61 s | 6.9 ms | 292.1 ms | 42.91 s | **99.30%** |
| baseline `path=blob` | 42.49 s | 0.0 ms | 242.9 ms | 42.99 s | **98.84%** |

`path=blob` serialises graph + dataset into one blob, so it has no separate
export stage (the blob serialisation is folded into `total_us`); `path=fast`
only has "export the graph" and "write the graph".

**What this means**: the build stage - cuVS's own graph construction - is
**99%** of the timed window, export and write together are below 1%, and the
two paths differ by **0.07 s** in `total_us`. There is no room left in the
build window for snapshot-format work; the remaining room is outside cuVS's
graph construction, and that part cannot be touched (the recall headroom is
~0).

**Recall** (numpy brute-force exact ground truth, 100 queries, top-100):

| index | dataset | Recall@10 | Recall@100 |
| --- | --- | --- | --- |
| GPU `lib=cuvs` | 250k synthetic | **0.9870** | **0.9614** |
| GPU `lib=cuvs` | 10k real | **1.0000** | **1.0000** |
| CPU `lib=vsag` | 250k synthetic | 0.1860 | 0.1429 |
| CPU `lib=vsag` | 10k real | 0.7660 | 0.6524 |

The 10k set ships an official ground truth; its top-10 agrees with the numpy
brute-force GT at **1.0000**, so the query vectors, the id mapping and the GT
computation are all correct. On the 250k set an **exact-scan control** on the
same VSAG table reaches Recall@10 = 1.0000 while the approximate search
returns 0.1860 - the low VSAG number is the CPU index's real search quality on
cluster-less high-dimensional data, not a measurement artefact.

> The GPU speedup is **scale dependent**: on 10k vectors the GPU path (8.63 s)
> is *slower* than the CPU path (2.63 s) because the fixed cuVS start-up and
> kernel synchronisation dominate at that size.

**Equivalence of the two paths** (250k, **after an observer restart**, same
query vector):

```text
155278, 118208, 78118, 144042, 188367, 77420, 109951, 227359, 189203, 147305
   ^ fast path      ^ baseline path      diff is empty
```

Both paths return a byte-identical top-10 - the direct empirical proof of the
"the recall cannot change" claim.

### 5.3 Restore-path regression (found by the cloud run)

The same cloud run exposed a bug that the 10k smoke test could not see: at
250k vectors the fast snapshot's restore crashed the observer.

The fast path restores the GPU index with `cuvsCagraIndexFromArgs()`, which
needs more stack than the small (~1.5MB) OB request-worker thread has. The
blob path already ran its restore on a 32MB pthread for exactly that reason;
the fast path did not, so at the contest size it died with a SIGSEGV before it
could even log `restored fast snapshot` - an index that had been persisted and
reloaded could not be queried at all.

| scenario | before | after |
| --- | --- | --- |
| 250k fast path, restart, query | SIGSEGV | 10 rows returned |
| 250k blob path, restart, query | 10 rows returned | 10 rows returned |
| 10k fast path, restart, query | 10 rows returned | 10 rows returned |

The fix runs the fast restore on the same 32MB pthread as the blob path. It
only changes the thread stack of the restore action - the graph construction,
the search parameters and the data are untouched, so the recall cannot change.

The check was validated with a negative control: rebuilt from the adaptor
without the fix (observer 1,379,126,480 bytes) the check **fails** with exit
code 1 - the observer crashes inside the restore; rebuilt with the fix
(observer 1,379,129,096 bytes) it **passes** with exit code 0 and 10 distinct
ids. Same machine, same 250,000 x 128 data, the thread stack is the only
variable.

`validation/run_restore_stress.sh` is the regression check: it builds a GPU
index, restarts the observer and queries the restored index. It has to run at
250k - at 10k the overflow does not happen and the check passes even with the
bug.

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
| 9 | 11-09 21:24 | `3794da35` | ok | 6m46s | 5.324, `build=1.081x, query=1.069x, gates=pass` |
| 10 | 11-10 | `e568a8b1` | ok | 12m | 12.369, `build=1.648x, query=1.506x, gates=pass` |
| 11 | 11-10 | `11f2e9f5` | ok | 12m | 15.575, `build=2.120x, query=1.599x, gates=pass` |
| 12 | 11-11 | `5800e6b4` | failed | 11m | `gates failed: relative_vram_limit` |
| 13 | 11-13 | `5800e6b4` | ok | 17m | 15.539, `build=2.114x, query=1.601x, gates=pass` |
| **14** | 11-13 | `e4a56362` | ok | 10m | **15.977, `build=2.188x, query=1.583x, gates=pass` (best)** |
| 15 | 11-13 | `c60fd055` | ok | 11m | 15.937, `build=2.181x, query=1.550x, gates=pass` |
| 16 | 11-13 | `1d89dcea` | ok | 15m | 15.375, `build=2.099x, query=1.478x, gates=pass` (a regression) |
| **17** | 11-14 | `bcda70fe` | ok | 12m | **20.000 (full marks), `build=3.220x, query=1.699x, gates=pass` (best)** |

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
