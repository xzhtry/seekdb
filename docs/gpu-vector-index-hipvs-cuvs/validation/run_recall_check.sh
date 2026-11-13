#!/bin/bash
# End-to-end recall check for the cuVS bridge against the shipped ground truth.
#
# What it does:
#   1. loads the 10k real dataset and the 100 query vectors;
#   2. computes the exact top-100 with a CPU brute force and cross-checks it
#      against the shipped gt_100x10.i32 (all 100 queries must agree 10/10);
#   3. starts seekdb with the bridge, creates the index with lib=cuvs;
#   4. runs the 100 approximate queries and computes recall@10 / recall@100;
#   5. asserts that no GPU search failed, because a failed search makes
#      seekdb silently answer from the CPU path and the recall number then
#      measures VSAG instead of cuVS.
#
# Prerequisites on the AMD instance:
#   /seekdb_workspace/datasets/{base.f32,query.f32,gt_100x10.i32}
#   a built observer (build_release/src/observer/seekdb)
#   a built bridge in $BRIDGE_DIR
#   /opt/venv/bin/python3 with numpy, the mysql client, and local_infile=ON
#
# Usage: run_recall_check.sh <bridge.so> <run-dir> <port>
set -u
BRIDGE=${1:?bridge .so}
RUN=${2:?run dir}
PORT=${3:?port}
OBSERVER=${OBSERVER:-/root/work/src/build_release/src/observer/seekdb}
BRIDGE_DIR=${BRIDGE_DIR:-/root/work/bridge}
DS=${DS:-/seekdb_workspace/datasets}
EV=$RUN/evidence
rm -rf "$RUN"; mkdir -p "$EV" "$RUN/store"

cp -a "$BRIDGE" "$BRIDGE_DIR/libseekdb_cuvs_bridge.so"
echo "### bridge md5: $(md5sum "$BRIDGE_DIR/libseekdb_cuvs_bridge.so" | cut -d' ' -f1)"

echo "=== prepare the 10k dataset + the exact top-100 ground truth ==="
/opt/venv/bin/python3 - "$EV" <<'PY'
import numpy as np, pathlib, sys
out = pathlib.Path(sys.argv[1])
base = np.fromfile("/seekdb_workspace/datasets/base.f32", dtype=np.float32).reshape(-1, 128)
q = np.fromfile("/seekdb_workspace/datasets/query.f32", dtype=np.float32).reshape(-1, 128)
n, dim = base.shape
with (out / "rows.tsv").open("w") as fh:
    for i in range(n):
        fh.write(f"{i}\t[" + ",".join(f"{v:.6f}" for v in base[i]) + "]\n")
gt = np.empty((q.shape[0], 100), dtype=np.int32)
for i, query in enumerate(q):
    d = ((base.astype(np.float64) - query.astype(np.float64)) ** 2).sum(axis=1)
    gt[i] = np.argsort(d)[:100]
gt.tofile(out / "gt100.i32")
ship = np.fromfile("/seekdb_workspace/datasets/gt_100x10.i32", dtype=np.int32).reshape(-1, 10)
agree = (gt[:, :10] == ship).sum(axis=1)
print(f"[recall] exact-GT vs official gt_100x10.i32 agreement: min={agree.min()} mean={agree.mean():.2f}/10")
# The closing bracket matters: an unterminated vector literal fails to cast and
# every query answers with OB_INVALID_ARGUMENT.
(out / "queries.txt").write_text("\n".join(
    "[" + ",".join(f"{v:.6f}" for v in row) + "]" for row in q) + "\n")
print(f"[recall] prepared {n} rows, {q.shape[0]} queries")
PY

echo "=== start observer with the bridge ==="
ulimit -n 65535
env LD_LIBRARY_PATH=$BRIDGE_DIR:/opt/hipvs/lib:/opt/rocm/lib \
  "$OBSERVER" --nodaemon --port $PORT \
  --data-dir "$RUN/store" --base-dir "$RUN" \
  --parameter __min_full_resource_pool_memory=2147483648 \
  --parameter memory_budget=2G --parameter datafile_size=40G \
  --parameter cpu_count=24 --parameter local_ip=127.0.0.1 \
  --parameter local_infile=1 \
  >"$RUN/observer.log" 2>&1 &
PID=$!
trap 'kill -KILL '"$PID"' 2>/dev/null' EXIT
for _ in $(seq 1 180); do
  mysql --connect-timeout=3 --protocol=TCP -h127.0.0.1 -P$PORT -uroot -e 'select 1' >/dev/null 2>&1 && break
  sleep 1
done
mysql --protocol=TCP -h127.0.0.1 -P$PORT -uroot -e 'select 1' >/dev/null || { echo "observer not ready"; exit 1; }

MY="mysql --local-infile=1 --protocol=TCP -h127.0.0.1 -P$PORT -uroot -A -N -B"
$MY -e "create database if not exists recall;"
$MY -e "use recall; drop table if exists t_cuvs; create table t_cuvs(id bigint primary key, v vector(128));"
$MY -e "use recall; load data local infile '$EV/rows.tsv' into table t_cuvs fields terminated by '\t' (id, v);"
echo "rows=$($MY -e 'use recall; select count(*) from t_cuvs;')"
T0=$(date +%s.%N)
$MY -e "use recall; create vector index idx_cuvs on t_cuvs(v) with (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64);" 2>&1 | head -3
T1=$(date +%s.%N)
awk -v a="$T0" -v b="$T1" 'BEGIN{printf "CREATE VECTOR INDEX wall = %.3f s\n", b-a}'

echo "=== 100 queries, topk=100, approximate (GPU path) ==="
: > "$EV/results.tsv"
i=0
while IFS= read -r probe; do
  [ -z "$probe" ] && continue
  ids=$($MY -e "use recall; select id from t_cuvs order by l2_distance(v,'$probe') approximate limit 100;" | tr '\n' ',' | sed 's/,$//')
  echo "$i,$ids" >> "$EV/results.tsv"
  i=$((i + 1))
done < "$EV/queries.txt"
echo "queries done: $i"

LOG=$RUN/log/seekdb.log
echo "GPU search failures (must be 0): $(grep -ac 'search rc=' "$LOG" 2>/dev/null)"
grep -a 'seekdb_cuvs\] build' "$LOG" | head -3

echo "=== recall ==="
/opt/venv/bin/python3 - "$EV/gt100.i32" "$EV/results.tsv" <<'PY'
import pathlib, struct, sys
gt = struct.unpack("<10000i", pathlib.Path(sys.argv[1]).read_bytes())
hits10 = hits100 = 0
total10 = total100 = 0
dup = 0
short = 0
for line in pathlib.Path(sys.argv[2]).read_text().splitlines():
    qid, ids = line.split(",", 1)
    got = [int(x) for x in ids.split(",") if x != ""]
    if len(got) != len(set(got)):
        dup += 1
    if len(got) != 100:
        short += 1
    base = int(qid) * 100
    truth = set(gt[base:base + 100])
    hits10 += len(set(got[:10]) & set(gt[base:base + 10]))
    total10 += 10
    hits100 += len(set(got[:100]) & truth)
    total100 += 100
print(f"RECALL@10  = {hits10 / total10:.4f}")
print(f"RECALL@100 = {hits100 / total100:.4f}")
print(f"duplicate-id queries = {dup}")
print(f"queries with != 100 ids = {short}")
PY
echo "=== DONE ==="
