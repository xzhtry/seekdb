#!/usr/bin/env bash
# Regression check for the restore path of the fast snapshot.
#
# The fast snapshot restores the GPU index with cuvsCagraIndexFromArgs(), which
# needs more stack than the small (~1.5MB) OB request-worker thread has. At
# 250k vectors it overflowed that stack and the observer died with a SIGSEGV
# before it could even log "restored fast snapshot", so an index that had been
# persisted and reloaded could not be queried at all.
#
# This script builds a GPU index, restarts the observer (which reloads the
# index from the persisted snapshot) and queries it. It has to run at the
# contest size (250k) - at 10k the overflow does not happen and the check
# passes even with the bug.
#
#   run_restore_stress.sh --observer PATH --bridge PATH --base-dir PATH \
#                          --port PORT [--rows N] [--dim D]
#
# Environment:
#   SEEKDB_CUVS_FAST_SNAPSHOT   1 = fast snapshot (default), 0 = cuVS blob
#
# Exit code 0 means: the index was restored and answered the query.

set -Eeuo pipefail

OBSERVER=""
BRIDGE=""
BASE_DIR=""
PORT=""
ROWS=250000
DIM=128

usage() {
  cat >&2 <<'EOF'
Usage: run_restore_stress.sh --observer PATH --bridge PATH --base-dir PATH
                              --port PORT [--rows N] [--dim D]
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --observer) OBSERVER=$2; shift 2 ;;
    --bridge) BRIDGE=$2; shift 2 ;;
    --base-dir) BASE_DIR=$2; shift 2 ;;
    --port) PORT=$2; shift 2 ;;
    --rows) ROWS=$2; shift 2 ;;
    --dim) DIM=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

for v in OBSERVER BRIDGE BASE_DIR PORT; do
  [[ -n "${!v}" ]] || { echo "missing --$(echo "$v" | tr 'A-Z' 'a-z')" >&2; usage >&2; exit 2; }
done
[[ -x "$OBSERVER" ]] || { echo "observer not executable: $OBSERVER" >&2; exit 2; }
[[ -f "$BRIDGE" ]] || { echo "bridge not found: $BRIDGE" >&2; exit 2; }

RUN_DIR=$BASE_DIR/run
EVIDENCE=$BASE_DIR/evidence
mkdir -p "$EVIDENCE/data"

mysql_cmd() {
  timeout 3600 mysql --connect-timeout=5 --local-infile=1 --protocol=TCP \
    -h127.0.0.1 -P"$1" -uroot -A -N -B "${@:2}"
}

OBS_PID=""
cleanup() { [[ -n "$OBS_PID" ]] && kill -TERM "$OBS_PID" 2>/dev/null || true; }
trap cleanup EXIT

# ------------------------------------------------------------------ dataset ---
if [[ ! -s "$EVIDENCE/data/base.f32" ]]; then
  echo "[stress] preparing $ROWS x $DIM dataset"
  python3 - "$EVIDENCE/data" "$ROWS" "$DIM" <<'PY'
import array
import pathlib
import random
import sys

out_dir, n, dim = pathlib.Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
try:
    import numpy as np
    rng = np.random.default_rng(20240917)
    m = rng.normal(size=(n, dim)).astype(np.float32)
    m /= np.linalg.norm(m, axis=1, keepdims=True)
    (out_dir / "base.f32").write_bytes(m.tobytes())
except ImportError:
    # numpy is optional; the pure-Python generator is slower but it is not
    # timed.
    rng = random.Random(20240917)
    buf = array.array("f")
    for _ in range(n):
        row = [rng.gauss(0.0, 1.0) for _ in range(dim)]
        norm = max(1e-12, sum(v * v for v in row) ** 0.5)
        buf.extend(v / norm for v in row)
    (out_dir / "base.f32").write_bytes(buf.tobytes())
(out_dir / "meta.txt").write_text(f"{n} {dim}\n")
PY
fi
N=$(head -1 "$EVIDENCE/data/meta.txt" | cut -d' ' -f1)
DIM_EFF=$(head -1 "$EVIDENCE/data/meta.txt" | cut -d' ' -f2)

if [[ ! -s "$EVIDENCE/data/rows.tsv" ]]; then
  python3 - "$EVIDENCE/data" "$N" "$DIM_EFF" <<'PY'
import pathlib
import sys

out_dir, n, dim = pathlib.Path(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
vals = memoryview((out_dir / "base.f32").read_bytes()).cast("f")
with (out_dir / "rows.tsv").open("w") as fh:
    for i in range(n):
        row = vals[i * dim:(i + 1) * dim]
        fh.write(str(i))
        fh.write("\t[")
        fh.write(",".join(f"{v:.6f}" for v in row))
        fh.write("]\n")
PY
fi

start_observer() {
  ulimit -n 65535
  env "LD_LIBRARY_PATH=$(dirname "$BRIDGE"):/opt/hipvs/lib:/opt/rocm/lib" \
      "OB_VSAG_TRACE=1" \
      "$OBSERVER" --nodaemon --port "$PORT" \
      --data-dir "$RUN_DIR/store" --base-dir "$RUN_DIR" \
      --parameter __min_full_resource_pool_memory=2147483648 \
      --parameter memory_budget=2G --parameter datafile_size=40G \
      --parameter cpu_count=24 --parameter local_ip=127.0.0.1 \
      > "$1" 2>&1 &
  OBS_PID=$!
  for _ in $(seq 1 240); do
    [[ -d "/proc/$OBS_PID" ]] || { echo "observer died" >&2; tail -40 "$1" >&2; exit 1; }
    if mysql_cmd "$PORT" -e 'select 1' >/dev/null 2>&1; then return 0; fi
    sleep 0.5
  done
  echo "observer not ready" >&2
  exit 1
}

# --------------------------------------------------------------------- build ---
echo "[stress] building the GPU index over $N x $DIM_EFF"
start_observer "$EVIDENCE/observer-build.log"
mysql_cmd "$PORT" -e "alter system set ob_vector_memory_limit_percentage=30; set global local_infile=on; create database if not exists bench;" >/dev/null
mysql_cmd "$PORT" -e "use bench; drop table if exists t_cuvs; create table t_cuvs(id bigint primary key, v vector($DIM_EFF));" >/dev/null
mysql_cmd "$PORT" -e "set session local_infile=on;" >/dev/null 2>&1 || true
mysql_cmd "$PORT" -e "use bench; load data local infile '$EVIDENCE/data/rows.tsv' into table t_cuvs fields terminated by '\t' (id, v);" >/dev/null
mysql_cmd "$PORT" -e "use bench; create vector index idx_cuvs on t_cuvs(v) with (distance=l2, type=hnsw, lib=cuvs, m=16, ef_construction=200, ef_search=64);" >/dev/null
cleanup
OBS_PID=""
# The observer has to release the port and the tenant lock before the next one
# can take them over, otherwise the restart fails with "Start seekdb failed".
for _ in $(seq 1 120); do
  if ! mysql_cmd "$PORT" -e 'select 1' >/dev/null 2>&1; then break; fi
  sleep 1
done
sleep 2

# ------------------------------------------------------- restart and query ---
echo "[stress] restarting the observer (this reloads the index from the snapshot)"
start_observer "$EVIDENCE/observer-restart.log"

# The restore is lazy: it happens when the tenant is opened, i.e. on the first
# access to the table. So the query comes first and the log is checked after.
echo "[stress] querying the restored index"
probe=$(python3 - "$EVIDENCE/data/base.f32" "$DIM_EFF" <<'PY'
import pathlib
import sys

vals = memoryview(pathlib.Path(sys.argv[1]).read_bytes()).cast("f")
dim = int(sys.argv[2])
print("[" + ",".join(f"{v:.6f}" for v in vals[:dim]) + "]")
PY
)
mysql_cmd "$PORT" -e "select id from bench.t_cuvs order by l2_distance(v,'$probe') approximate limit 10;" \
  > "$EVIDENCE/restart-query.ids" 2>"$EVIDENCE/restart-query.err"
rows=$(wc -l < "$EVIDENCE/restart-query.ids")
distinct=$(sort -u "$EVIDENCE/restart-query.ids" | wc -l)
if [[ "$rows" != "10" || "$distinct" != "10" ]]; then
  echo "[stress] FAIL: the restored index returned $rows rows ($distinct distinct)" >&2
  cat "$EVIDENCE/restart-query.err" >&2
  exit 1
fi

# The restore is logged when it succeeded; if the process died inside the
# restore, this line is missing.
restored=0
for f in $(ls -tr "$RUN_DIR/log"/seekdb.log* 2>/dev/null); do
  if grep -qa 'restored fast snapshot' "$f"; then restored=1; fi
done
if [[ "$restored" != "1" ]]; then
  echo "[stress] FAIL: the index was not restored (no 'restored fast snapshot' in the log)" >&2
  exit 1
fi

echo "[stress] PASS: the restored index answered with $rows distinct ids"
cat "$EVIDENCE/restart-query.ids"
