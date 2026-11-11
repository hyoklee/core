#!/bin/bash
# Run the PoroTomo OMNI through the CAE on a fresh single-node runtime and
# verify every ingested dataset byte-for-byte against the source HDF5 file.
#
# Usage: run_porotomo.sh [omni.yaml]
# Env:   CLIO_BIN   build bin dir (default ~/clio-core/build/bin)
#        CONDA_PREFIX  env providing python3 + h5py + pyyaml + libhdf5
HERE=$(cd "$(dirname "$0")" && pwd)
OMNI=${1:-$HERE/porotomo_dasv_omni.yaml}
D=${PORO_DATA:-$HOME/cae_test_data/porotomo}
BIN=${CLIO_BIN:-$HOME/clio-core/build/bin}
export PATH=${CONDA_PREFIX:-$HOME/micromamba/envs/clio}/bin:$PATH
export LD_LIBRARY_PATH=$BIN:${CONDA_PREFIX:-$HOME/micromamba/envs/clio}/lib:$LD_LIBRARY_PATH
export CLIO_SERVER_CONF=$HERE/clio_porotomo_config.yaml
export CLIO_BIND_ADDR=127.0.0.1
export CLIO_PORT=10600
mkdir -p "$D"
"$BIN/clio_run" stop --force >/dev/null 2>&1
echo "== starting runtime"
nohup "$BIN/clio_run" start > "$D/runtime.out" 2>&1 &
RPID=$!
for i in $(seq 1 60); do
  sleep 1
  if "$BIN/clio_run" status >/dev/null 2>&1; then echo "runtime up after ${i}s (pid $RPID)"; break; fi
  if ! kill -0 $RPID 2>/dev/null; then echo "runtime died"; tail -30 "$D/runtime.out"; exit 1; fi
done
sleep 2
echo "== clio_cae $OMNI"
T0=$(date +%s%N)
"$BIN/clio_cae" "$OMNI" 2>&1 | tail -8
RC=${PIPESTATUS[0]}
T1=$(date +%s%N)
echo "clio_cae exit=$RC elapsed=$(( (T1-T0)/1000000 )) ms (schedules only; assimilation is asynchronous)"
echo "== verify"
python3 "$HERE/verify_porotomo.py" "$OMNI" "$CLIO_SERVER_CONF" "$BIN" 2>&1 | grep -v '^\['
VRC=${PIPESTATUS[0]}
echo "verify exit=$VRC"
echo "== stopping runtime"
"$BIN/clio_run" stop >/dev/null 2>&1
wait $RPID 2>/dev/null
echo "RUN_DONE cae=$RC verify=$VRC"
exit $VRC
