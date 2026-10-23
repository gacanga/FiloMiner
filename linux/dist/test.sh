#!/usr/bin/env bash
# test.sh - correctness tests: CPU reference vs the KawPow test vectors, then every
# GPU vs the CPU (DAG items, full hashes of 2 periods, candidate filter).
# Must end with "TEST RESULT: ALL OK" before mining.  Stop any other miner first.
cd "$(dirname "$0")"
export CUDA_DEVICE_ORDER=PCI_BUS_ID
./filominer --config filominer.conf --test "$@" 2>&1 | tee test.log
