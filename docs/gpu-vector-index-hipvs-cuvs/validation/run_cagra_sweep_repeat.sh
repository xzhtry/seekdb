#!/bin/bash
# sweep3: repeatability + the third cuVS build algorithm (ITERATIVE_CAGRA_SEARCH).
set -u
PROBE=/root/work/probe
exec > >(tee -a /root/work/sweep3_out.txt) 2>&1

echo "### compile sweep2 (with single-query latency probe)"
/opt/rocm/bin/hipcc -O2 -fopenmp -D__HIP_PLATFORM_AMD__ \
  -I/opt/hipvs/include -I/opt/rocm/include \
  "$PROBE/sweep2.c" -L/opt/hipvs/lib -lcuvs_c \
  -Wl,-rpath,/opt/hipvs/lib -Wl,-rpath,/opt/rocm/lib \
  -o "$PROBE/sweep2" || { echo "COMPILE_FAILED"; exit 1; }
echo "compiled ok"

S250=$PROBE/base250k.f32
Q250=$PROBE/query250k.f32
S10=/seekdb_workspace/datasets/base.f32
Q10=/seekdb_workspace/datasets/query.f32

run() { local label=$1; shift; echo "### CFG $label args: $*"; "$PROBE/sweep2" "$@" 2>&1 | sed "s/^/[$label] /"; }

echo "### load before"; uptime

echo "############ R1: 3 reps, 250k, gd=128 igd=256 ############"
run R1_n100  "$S250" "$Q250" 250000 128 100 100 128 256 100 2 0 3
run R1_n50   "$S250" "$Q250" 250000 128 100 100 128 256  50 2 0 3
run R1_n20   "$S250" "$Q250" 250000 128 100 100 128 256  20 2 0 3

echo "############ R2: 3 reps, 250k, gd=128 igd=128 niter=50 ############"
run R2_igd128_n50 "$S250" "$Q250" 250000 128 100 100 128 128 50 2 0 3

echo "############ R3: ITERATIVE_CAGRA_SEARCH (algo=3) ############"
run R3_iter100 "$S250" "$Q250" 250000 128 100 100 128 256 100 3 0 1
run R3_iter20  "$S250" "$Q250" 250000 128 100 100 128 256  20 3 0 1

echo "############ R4: 10k real, ITERATIVE vs NN_DESCENT ############"
run R4_iter100 "$S10" "$Q10" 10000 128 100 100 128 256 100 3 0 1
run R4_nd100   "$S10" "$Q10" 10000 128 100 100 128 256 100 2 0 1

echo "### load after"; uptime
echo "### DONE"
