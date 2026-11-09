#!/usr/bin/env bash
#
# One-shot cloud runbook: build, validate and benchmark the GPU vector index.
#
# It performs exactly the steps the contest evaluation performs, plus the
# measurements that are needed to decide what to optimise next:
#
#   1. preflight (ROCm / hipVS / GPU device / third-party dependency cache)
#   2. build the cuVS bridge
#   3. build seekdb with the fixed evaluation CMake arguments
#   4. GPU correctness smoke test (the gate-side checks)
#   5. CPU-vs-GPU build benchmark
#   6. extract the per-stage build timings and render the chart
#   7. print a summary that can be pasted into the report
#
# Usage:
#   run_cloud_benchmark.sh [--base-dir DIR] [--port PORT] [--rows N] [--dim D]
#                          [--repeat R] [--skip-build] [--skip-smoke]
#
# The script stops at the first failure. Nothing it does modifies the
# repository, so it is safe to re-run.

set -Eeuo pipefail

BASE_DIR=/tmp/seekdb-cloud-bench
PORT=2981
ROWS=250000
DIM=128
REPEAT=3
SKIP_BUILD=0
SKIP_SMOKE=0

while (($# > 0)); do
  case "$1" in
    --base-dir) BASE_DIR=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --rows) ROWS=$2; shift 2 ;;
    --dim) DIM=$2; shift 2 ;;
    --repeat) REPEAT=$2; shift 2 ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --skip-smoke) SKIP_SMOKE=1; shift ;;
    -h|--help)
      sed -n '2,30p' "$0"
      exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
EVIDENCE=$BASE_DIR/evidence
BRIDGE_DIR=$BASE_DIR/bridge
BRIDGE=$BRIDGE_DIR/libseekdb_cuvs_bridge.so
OBSERVER=$REPO_ROOT/build_release/src/observer/seekdb
DEPS_CACHE=${SEEKDB_DEPS_CACHE:-/opt/seekdb-deps-cache/ac032efc6b707fc0273e6b553db53011/3rd}

log()  { printf '\n=== %s ===\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

mkdir -p "$BASE_DIR" "$EVIDENCE" "$BRIDGE_DIR"

# --------------------------------------------------------------- 1. preflight --
log "preflight"
[[ -d /opt/rocm ]]       || fail "/opt/rocm is missing"
[[ -d /opt/hipvs ]]      || fail "/opt/hipvs is missing"
[[ -c /dev/kfd ]]        || fail "/dev/kfd is missing (no AMD GPU visible)"
command -v hipcc >/dev/null 2>&1 || [[ -x /opt/rocm/bin/hipcc ]] || fail "hipcc is missing"
command -v mysql >/dev/null || fail "the mysql client is missing"
command -v python3 >/dev/null || fail "python3 is missing"
[[ -d "$DEPS_CACHE" ]]   || fail "the third-party dependency cache is missing: $DEPS_CACHE"
if command -v rocminfo >/dev/null 2>&1; then
  rocminfo 2>/dev/null | grep -m1 -E 'Name:.*gfx' || echo "WARNING: rocminfo did not report a gfx device"
fi
echo "repo        : $REPO_ROOT"
echo "evidence    : $EVIDENCE"
echo "deps cache  : $DEPS_CACHE"

# ------------------------------------------------------------------ 2. bridge --
log "build the cuVS bridge"
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm \
  "$REPO_ROOT/tools/hipvs_bridge/build.sh" "$BRIDGE" | tee "$EVIDENCE/bridge-build.log"
[[ -f "$BRIDGE" ]] || fail "the bridge was not built"

# ----------------------------------------------------------------- 3. seekdb --
if [[ "$SKIP_BUILD" == "0" ]]; then
  log "build seekdb"
  if [[ ! -e "$REPO_ROOT/deps/3rd" ]]; then
    ln -s "$DEPS_CACHE" "$REPO_ROOT/deps/3rd" \
      || fail "could not link the dependency cache into deps/3rd"
  fi
  cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build_release" -G "Unix Makefiles" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DOB_ENABLE_UNITY=ON -DOB_USE_LLD=ON \
    -DOB_BUILD_CUVS=ON -DOB_BUILD_CUVS_TRACE=OFF \
    -DCUVS_BRIDGE_LIB="$BRIDGE" > "$EVIDENCE/cmake-configure.log" 2>&1 \
    || fail "cmake configure failed, see $EVIDENCE/cmake-configure.log"
  cmake --build "$REPO_ROOT/build_release" --target seekdb -j "$(nproc)" \
    > "$EVIDENCE/cmake-build.log" 2>&1 || fail "the seekdb build failed, see $EVIDENCE/cmake-build.log"
else
  log "skip the seekdb build (--skip-build)"
fi
[[ -x "$OBSERVER" ]] || fail "the observer is missing: $OBSERVER"

# ------------------------------------------------------------------- 4. smoke --
if [[ "$SKIP_SMOKE" == "0" ]]; then
  log "GPU correctness smoke test"
  RENDER_NODE=$(ls /dev/dri/renderD* 2>/dev/null | head -1 || true)
  [[ -n "$RENDER_NODE" ]] || RENDER_NODE=/dev/dri/renderD128
  "$REPO_ROOT/docs/gpu-vector-index-hipvs-cuvs/validation/run_gpu_smoke.sh" \
    --observer "$OBSERVER" \
    --bridge "$BRIDGE" \
    --base-dir "$BASE_DIR/smoke" \
    --port $((PORT + 1)) \
    --render "$RENDER_NODE" \
    --evidence "$EVIDENCE/smoke" 2>&1 | tee "$EVIDENCE/smoke.log" \
    || fail "the GPU smoke test failed, see $EVIDENCE/smoke.log"
else
  log "skip the GPU smoke test (--skip-smoke)"
fi

# ---------------------------------------------------------------- 5. benchmark --
log "CPU vs GPU build benchmark"
"$REPO_ROOT/docs/gpu-vector-index-hipvs-cuvs/validation/run_build_benchmark.sh" \
  --observer "$OBSERVER" \
  --bridge "$BRIDGE" \
  --base-dir "$BASE_DIR/bench" \
  --port $((PORT + 2)) \
  --evidence "$EVIDENCE/bench" \
  --rows "$ROWS" --dim "$DIM" --repeat "$REPEAT" 2>&1 | tee "$EVIDENCE/benchmark.log" \
  || fail "the build benchmark failed, see $EVIDENCE/benchmark.log"

# ------------------------------------------------------- 6. stages and charts --
log "stage timings and chart"
grep -a '\[OBVSAG\]\[cuVS\] stage\|\[seekdb_cuvs\]' "$EVIDENCE/bench/observer.log" \
  > "$EVIDENCE/bench/stages.txt" 2>/dev/null || true
python3 "$REPO_ROOT/docs/gpu-vector-index-hipvs-cuvs/validation/make_build_chart.py" \
  --mode benchmark \
  --benchmark "$EVIDENCE/bench/benchmark.json" \
  --stages "$EVIDENCE/bench/stages.txt" \
  --out "$EVIDENCE/bench/build-performance.svg" || true

# ------------------------------------------------------------------ 7. summary --
log "summary"
echo "--- fast snapshot support ---"
grep -a 'fast_snapshot_api\|fast_snapshot_supported' "$EVIDENCE/bench/observer.log" \
  | sort -u || echo "NOT FOUND (the bridge did not log its capability probe)"
echo
echo "--- stage timings ---"
cat "$EVIDENCE/bench/stages.txt" 2>/dev/null || echo "NOT FOUND"
echo
echo "--- CPU vs GPU build time ---"
python3 - "$EVIDENCE/bench/benchmark.json" <<'PY'
import json, math, pathlib, sys
data = json.loads(pathlib.Path(sys.argv[1]).read_text())
cpu = data.get("cpu_vsag") or {}
gpu = data.get("gpu_cuvs") or {}
print(f"CPU  lib=vsag  median {cpu.get('median_ms', '?')} ms  (min {cpu.get('min_ms','?')} / max {cpu.get('max_ms','?')})")
print(f"GPU  lib=cuvs  median {gpu.get('median_ms', '?')} ms  (min {gpu.get('min_ms','?')} / max {gpu.get('max_ms','?')})")
speedup = data.get("speedup_median_cpu_over_gpu")
if speedup:
    score = 14.0 * math.log(speedup) / math.log(3.0)
    print(f"GPU build speedup {speedup:.4f}x  ->  build score {score:.2f} / 14")
PY
echo
echo "artifacts:"
ls -1 "$EVIDENCE/bench" 2>/dev/null || true
echo
echo "evidence directory: $EVIDENCE"
