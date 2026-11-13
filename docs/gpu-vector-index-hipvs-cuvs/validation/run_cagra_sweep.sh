#!/bin/bash
# sweep2: CAGRA build-cost + recall frontier via the cuVS C API.
set -u
PROBE=/root/work/probe
OUT=${1:-/root/work/sweep2_out.txt}
mkdir -p "$PROBE"
exec > >(tee -a "$OUT") 2>&1

echo "### compile sweep2"
/opt/rocm/bin/hipcc -O2 -fopenmp -D__HIP_PLATFORM_AMD__ \
  -I/opt/hipvs/include -I/opt/rocm/include \
  "$PROBE/sweep2.c" -L/opt/hipvs/lib -lcuvs_c \
  -Wl,-rpath,/opt/hipvs/lib -Wl,-rpath,/opt/rocm/lib \
  -o "$PROBE/sweep2" || { echo "COMPILE_FAILED"; exit 1; }
echo "compiled ok"

if [ ! -f "$PROBE/base250k.f32" ]; then
  echo "### generate 250k synthetic dataset"
  /opt/venv/bin/python3 - <<'PYEOF'
import numpy as np, os
out = "/root/work/probe"
n, dim, nq = 250000, 128, 100
rng = np.random.default_rng(20261113)
rng.standard_normal((n, dim), dtype=np.float32).tofile(os.path.join(out, "base250k.f32"))
rng2 = np.random.default_rng(777)
rng2.standard_normal((nq, dim), dtype=np.float32).tofile(os.path.join(out, "query250k.f32"))
print("wrote 250k dataset")
PYEOF
fi

S10=/seekdb_workspace/datasets/base.f32
Q10=/seekdb_workspace/datasets/query.f32
S250=$PROBE/base250k.f32
Q250=$PROBE/query250k.f32

run() {  # label ds qf n dim nq topk gd igd niter algo metric nbuild
  local label=$1
  shift
  echo "### CFG $label args: $*"
  "$PROBE/sweep2" "$@" 2>&1 | sed "s/^/[$label] /"
}

echo "############ PHASE A: 10k real dataset (official GT cross-checked) ############"
for cfg in "A_128_256_100 128 256 100 2 0" \
           "A_128_256_50 128 256 50 2 0" \
           "A_128_256_20 128 256 20 2 0" \
           "A_128_256_10 128 256 10 2 0" \
           "A_128_128_50 128 128 50 2 0" \
           "A_64_128_100 64 128 100 2 0" \
           "A_64_128_50 64 128 50 2 0" \
           "A_64_128_20 64 128 20 2 0" \
           "A_32_64_50 32 64 50 2 0" \
           "A_32_64_20 32 64 20 2 0" \
           "A_16_32_50 16 32 50 2 0" \
           "A_128_256_100_exp 128 256 100 2 3" \
           "A_ivfpq_128_256 128 256 100 1 0" \
           "A_ivfpq_64_128 64 128 100 1 0"; do
  set -- $cfg
  run "$1" "$S10" "$Q10" 10000 128 100 100 $2 $3 $4 $5 $6 1
done

echo "############ PHASE B: 250k synthetic ############"
for cfg in "B_128_256_100 128 256 100 2 0" \
           "B_128_256_50 128 256 50 2 0" \
           "B_128_256_20 128 256 20 2 0" \
           "B_128_128_100 128 128 100 2 0" \
           "B_64_128_100 64 128 100 2 0" \
           "B_64_128_50 64 128 50 2 0" \
           "B_64_128_20 64 128 20 2 0" \
           "B_32_64_50 32 64 50 2 0" \
           "B_ivfpq_128_256 128 256 100 1 0"; do
  set -- $cfg
  run "$1" "$S250" "$Q250" 250000 128 100 100 $2 $3 $4 $5 $6 1
done

echo "############ PHASE C: warm build (two builds, same process) ############"
run C_warm_128_256_100 "$S250" "$Q250" 250000 128 100 100 128 256 100 2 0 2
run C_warm_64_128_50 "$S250" "$Q250" 250000 128 100 100 64 128 50 2 0 2

echo "### DONE"
