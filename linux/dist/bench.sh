#!/usr/bin/env bash
# bench.sh - 30 s hashrate benchmark on all GPUs (extra options allowed,
# e.g. ./bench.sh --gpus 0 --tpb 128). Stop any other miner first.
cd "$(dirname "$0")"
export CUDA_DEVICE_ORDER=PCI_BUS_ID
./filominer --config filominer.conf --bench 30 --bench-height 150000 "$@" 2>&1 | tee bench.log
