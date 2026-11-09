#!/usr/bin/env bash
#
# CPU vs GPU vector-index build benchmark for seekdb + hipVS/cuVS.
#
# Measures the *timed build window* of the contest:
#
#   CREATE VECTOR INDEX idx ON tbl(v)
#     WITH (distance=l2, type=hnsw, lib=<cuvs|vsag>, m=16, ef_construction=200, ef_search=64);
#
# on two tables that hold byte-identical data, so the CPU (lib=vsag) and the
# GPU (lib=cuvs) builds are directly comparable. The index parameters are the
# ones the contest fixes; the only difference between the two runs is `lib`.
#
# Every repetition uses a fresh pair of tables, so no index, graph or cached
# build from a previous repetition can be reused.
#
# The script also collects the per-stage timings that the adaptor emits, so
# the build window can be split into
#   build   = the cuVS/CAGRA graph construction on the GPU
#   export  = the knn graph copy to the host (fast path only)
#   write   = the snapshot stream write
#   restore = re-creating the GPU index from the persisted graph (reload only)
#
# Usage:
#   run_build_benchmark.sh --observer PATH --bridge PATH --base-dir PATH \
#                           --port PORT --evidence PATH [--rows N] [--dim D] \
#                           [--repeat R] [--data FILE.f32] [--topk K]
#
# --data FILE.f32  optional raw float32 matrix (rows*dim values, row-major);
#                  when omitted a synthetic Gaussian dataset is generated.
#
# Output:
#   $EVIDENCE/benchmark.json     machine-readable results
#   $EVIDENCE/benchmark.md       Markdown comparison table
#   $EVIDENCE/observer-*.log     observer logs (contain the stage timings)
#   $EVIDENCE/stages.txt         extracted stage timings

set -Eeuo pipefail

usage()
{
  cat <<'EOF'
Usage: run_build_benchmark.sh --observer PATH --bridge PATH --base-dir PATH
                             --port PORT --evidence PATH
                             [--rows N] [--dim D] [--repeat R]
                             [--data FILE.f32] [--topk K]
EOF
}

OBSERVER=""
BRIDGE=""
BASE_DIR=""
PORT=""
EVIDENCE=""
ROWS=250000
DIM=128
REPEAT=3
DATA_FILE=""
TOPK=64

while (($# > 0)); do
  case "$1" in
    --observer) OBSERVER=$2; shift 2 ;;
    --bridge) BRIDGE=$2; shift 2 ;;
    --base-dir) BASE_DIR=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --evidence) EVIDENCE=$2; shift 2 ;;
    --rows) ROWS=$2; shift 2 ;;
    --dim) DIM=$2; shift 2 ;;
    --repeat) REPEAT=$2; shift 2 ;;
    --data) DATA_FILE=$2; shift 2 ;;
    --topk) TOPK=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ -x "$OBSERVER" ]] || { echo "observer is not executable: $OBSERVER" >&2; exit 2; }
[[ -f "$BRIDGE" ]] || { echo "bridge does not exist: $BRIDGE" >&2; exit 2; }
[[ "$PORT" =~ ^[0-9]+$ ]] || { echo "invalid port: $PORT" >&2; exit 2; }
[[ "$ROWS" =~ ^[0-9]+$ && "$ROWS" -gt 0 ]] || { echo "invalid rows: $ROWS" >&2; exit 2; }
[[ "$DIM" =~ ^[0-9]+$ && "$DIM" -gt 0 ]] || { echo "invalid dim: $DIM" >&2; exit 2; }
[[ "$REPEAT" =~ ^[0-9]+$ && "$REPEAT" -gt 0 ]] || { echo "invalid repeat: $REPEAT" >&2; exit 2; }
[[ -n "$BASE_DIR" && "$BASE_DIR" != "/" ]] || { echo "unsafe base-dir" >&2; exit 2; }
[[ -n "$EVIDENCE" && "$EVIDENCE" != "/" ]] || { echo "unsafe evidence path" >&2; exit 2; }
[[ -z "$DATA_FILE" || -f "$DATA_FILE" ]] || { echo "data file not found: $DATA_FILE" >&2; exit 2; }
command -v mysql >/dev/null
command -v python3 >/dev/null

RUN_DIR=$BASE_DIR/run
DATA_DIR=$EVIDENCE/data
LOG=$EVIDENCE/observer.log
TRACE=$EVIDENCE/observer-trace.log
JSON=$EVIDENCE/benchmark.json
MD=$EVIDENCE/benchmark.md
STAGES=$EVIDENCE/stages.txt
OBS_PID=""

mkdir -p "$BASE_DIR" "$EVIDENCE" "$DATA_DIR"
rm -f "$LOG" "$TRACE" "$JSON" "$MD" "$STAGES"

stop_observer()
{
  local pid=$1
  [[ -n "$pid" && -d "/proc/$pid" ]] || return 0
  local cmd
  cmd=$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)
  if [[ "$cmd" != *"$OBSERVER"* ]]; then
    echo "refusing to stop pid=$pid: $cmd" >&2
    return 1
  fi
  kill -TERM "$pid" 2>/dev/null || true
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    [[ -d "/proc/$pid" ]] || break
    sleep 0.5
  done
  if [[ -d "/proc/$pid" ]]; then kill -KILL "$pid" 2>/dev/null || true; fi
  wait "$pid" 2>/dev/null || true
}

on_exit()
{
  local rc=$?
  trap - EXIT INT TERM
  stop_observer "$OBS_PID" || true
  exit "$rc"
}
trap on_exit EXIT INT TERM

mysql_cmd()
{
  timeout 3600 mysql --connect-timeout=5 --protocol=TCP -h127.0.0.1 -P"$1" -uroot -A -N -B "${@:2}"
}

# ---------------------------------------------------------------- data prep ---
echo "[bench] preparing $ROWS x $DIM dataset"
python3 - "$DATA_DIR" "$ROWS" "$DIM" "$DATA_FILE" <<'PY'
import array
import pathlib
import random
import struct
import sys

out_dir, rows, dim, data_file = pathlib.Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]

if data_file:
    raw = pathlib.Path(data_file).read_bytes()
    need = rows * dim * 4
    if len(raw) < need:
        raise SystemExit(f"data file too small: {len(raw)} < {need}")
    vectors = raw[:need]
    n = rows
else:
    n = rows
    try:
        import numpy as np
        rng = np.random.default_rng(20240917)
        matrix = rng.normal(size=(n, dim)).astype(np.float32)
        matrix /= np.linalg.norm(matrix, axis=1, keepdims=True)
        vectors = matrix.tobytes()
        print("[bench] dataset generated with numpy")
    except ImportError:
        # numpy is optional: a pure-Python generator keeps the benchmark
        # runnable on a bare instance. It is slower but it is not timed.
        print("[bench] numpy unavailable, falling back to the pure-Python generator")
        rng = random.Random(20240917)
        buf = array.array("f")
        for _ in range(n):
            row = [rng.gauss(0.0, 1.0) for _ in range(dim)]
            norm = max(1e-12, sum(v * v for v in row) ** 0.5)
            buf.extend(v / norm for v in row)
        vectors = buf.tobytes()
        print("[bench] dataset generated with the pure-Python generator")

(out_dir / "base.f32").write_bytes(vectors)
(out_dir / "meta.txt").write_text(f"{n} {dim}\n")
print(f"[bench] dataset ready: {n} x {dim}")
PY

N=$(head -1 "$DATA_DIR/meta.txt" | cut -d' ' -f1)
DIM_EFF=$(head -1 "$DATA_DIR/meta.txt" | cut -d' ' -f2)
[[ "$N" == "$ROWS" && "$DIM_EFF" == "$DIM" ]] || {
  echo "[bench] dataset shape differs from --rows/--dim" >&2; exit 2; }

# Write the rows as a TSV that LOAD DATA LOCAL INFILE can consume.
python3 - "$DATA_DIR" "$N" "$DIM_EFF" <<'PY'
import pathlib
import sys

out_dir, n, dim = pathlib.Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
src = (out_dir / "base.f32").read_bytes()
vals = memoryview(src).cast("f")
path = out_dir / "rows.tsv"
with path.open("w") as fh:
    for i in range(n):
        row = vals[i * dim:(i + 1) * dim]
        fh.write(str(i))
        fh.write("\t[")
        fh.write(",".join(f"{v:.6f}" for v in row))
        fh.write("]\n")
print(f"[bench] rows.tsv written: {path}")
PY

# ----------------------------------------------------------- observer start ---
echo "[bench] starting observer"
ulimit -n 65535
env \
  "LD_LIBRARY_PATH=$(dirname "$BRIDGE"):/opt/hipvs/lib:/opt/rocm/lib" \
  "OB_VSAG_TRACE=1" \
  "OB_VSAG_TRACE_FILE=$TRACE" \
  "$OBSERVER" \
    --nodaemon \
    --port "$PORT" \
    --data-dir "$RUN_DIR/store" \
    --base-dir "$RUN_DIR" \
    --parameter __min_full_resource_pool_memory=2147483648 \
    --parameter memory_budget=2G \
    --parameter datafile_size=40G \
    --parameter cpu_count=24 \
    --parameter local_ip=127.0.0.1 \
    >"$LOG" 2>&1 &
OBS_PID=$!

for _ in $(seq 1 240); do
  [[ -d "/proc/$OBS_PID" ]] || { echo "[bench] observer died" >&2; tail -40 "$LOG" >&2; exit 1; }
  if mysql_cmd "$PORT" -e 'select 1' >/dev/null 2>&1; then break; fi
  sleep 0.5
done
mysql_cmd "$PORT" -e 'select 1' >/dev/null || { echo "[bench] observer not ready" >&2; exit 1; }
mysql_cmd "$PORT" -e 'alter system set ob_vector_memory_limit_percentage=30; create database if not exists bench;' >/dev/null

# ------------------------------------------------------------------- build ---
results=""
for rep in $(seq 1 "$REPEAT"); do
  for lib in cuvs vsag; do
    tbl=t_${lib}_${rep}
    echo "[bench] rep $rep: preparing $tbl"
    mysql_cmd "$PORT" -e "use bench; drop table if exists $tbl; create table $tbl(id bigint primary key, v vector($DIM_EFF));" >/dev/null
    # Same data for both libraries.
    mysql_cmd "$PORT" -e "use bench; load data local infile '$DATA_DIR/rows.tsv' into table $tbl fields terminated by '\t' (id, v);" \
      >"$EVIDENCE/load-$tbl.log" 2>&1 || {
        echo "[bench] LOAD DATA failed for $tbl, see $EVIDENCE/load-$tbl.log" >&2
        exit 1
      }
    loaded=$(mysql_cmd "$PORT" -e "use bench; select count(*) from $tbl;")
    [[ "$loaded" == "$N" ]] || { echo "[bench] row count mismatch for $tbl: $loaded != $N" >&2; exit 1; }

    idx=idx_${lib}_${rep}
    echo "[bench] rep $rep: CREATE VECTOR INDEX on $tbl (lib=$lib)"
    start_ns=$(date +%s%N)
    mysql_cmd "$PORT" -e "use bench; create vector index $idx on $tbl(v) with (distance=l2, type=hnsw, lib=$lib, m=16, ef_construction=200, ef_search=$TOPK);" \
      >"$EVIDENCE/ddl-$tbl.log" 2>&1 || {
        echo "[bench] CREATE VECTOR INDEX failed for $tbl" >&2
        cat "$EVIDENCE/ddl-$tbl.log" >&2
        exit 1
      }
    end_ns=$(date +%s%N)
    elapsed_ms=$(( (end_ns - start_ns) / 1000000 ))
    echo "[bench] rep $rep lib=$lib build_ms=$elapsed_ms"
    results+="$rep"$'\t'"$lib"$'\t'"$elapsed_ms"$'\n'
  done
done

# ------------------------------------------------------------- stage timings ---
grep -a '\[OBVSAG\]\[cuVS\] stage' "$LOG" > "$STAGES" 2>/dev/null || true
grep -a '\[seekdb_cuvs\]' "$LOG" >> "$STAGES" 2>/dev/null || true

# ----------------------------------------------------------------- report ---
RESULTS_FILE=$EVIDENCE/results.tsv
printf '%s' "$results" > "$RESULTS_FILE"

python3 - "$RESULTS_FILE" "$JSON" "$MD" "$N" "$DIM_EFF" "$REPEAT" "$TOPK" <<'PY'
import json
import pathlib
import statistics
import sys

results_path, json_path, md_path, n, dim, repeat, topk = sys.argv[1:]
results_path, json_path, md_path = map(pathlib.Path, (results_path, json_path, md_path))
rows = [line.split("\t") for line in results_path.read_text().splitlines() if line]

by_lib = {}
for rep, lib, ms in rows:
    by_lib.setdefault(lib, []).append(int(ms))

def stat(values):
    if not values:
        return None
    return {
        "runs": len(values),
        "ms": values,
        "median_ms": statistics.median(values),
        "mean_ms": statistics.fmean(values),
        "min_ms": min(values),
        "max_ms": max(values),
    }

cpu = stat(by_lib.get("vsag", []))
gpu = stat(by_lib.get("cuvs", []))
speedup = (cpu["median_ms"] / gpu["median_ms"]) if cpu and gpu else None

payload = {
    "dataset": {"rows": int(n), "dim": int(dim)},
    "index_params": {
        "distance": "l2", "type": "hnsw", "m": 16,
        "ef_construction": 200, "ef_search": int(topk),
    },
    "repeat": int(repeat),
    "cpu_vsag": cpu,
    "gpu_cuvs": gpu,
    "speedup_median_cpu_over_gpu": speedup,
}
json_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")

def fmt(entry):
    if not entry:
        return "n/a"
    return (f"{entry['median_ms']:.0f} | {entry['mean_ms']:.0f} | "
            f"{entry['min_ms']:.0f} | {entry['max_ms']:.0f} | {entry['runs']}")

lines = []
lines.append("# CPU / GPU build benchmark")
lines.append("")
lines.append(f"- dataset: {n} vectors x {dim} dims")
lines.append(f"- index: `type=hnsw, m=16, ef_construction=200, ef_search={topk}, distance=l2`")
lines.append(f"- repetitions: {repeat} (fresh tables each time)")
lines.append("")
lines.append("| library | path | median ms | mean ms | min ms | max ms | runs |")
lines.append("| --- | --- | --- | --- | --- | --- | --- |")
lines.append(f"| CPU | `lib=vsag` | {fmt(cpu)} |")
lines.append(f"| GPU | `lib=cuvs` | {fmt(gpu)} |")
lines.append("")
if speedup:
    lines.append(f"**GPU build speedup (CPU median / GPU median): {speedup:.2f}x**")
else:
    lines.append("**GPU build speedup: n/a**")
lines.append("")
md_path.write_text("\n".join(lines) + "\n")
print("\n".join(lines))
PY

stop_observer "$OBS_PID"
OBS_PID=""
echo "[bench] done: $JSON and $MD"
