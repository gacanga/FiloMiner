#!/usr/bin/env bash
# start.sh - mines with filominer.conf on all GPUs, restarts 10 s after any exit.
# Log: filominer.log (appended). Stop with Ctrl+C (twice: miner, then loop).
# Do not use it while the systemd service (install-service.sh) is running.
cd "$(dirname "$0")"
export CUDA_DEVICE_ORDER=PCI_BUS_ID
while true; do
    echo "[$(date '+%F %T')] miner start" >> filominer.log
    ./filominer --config filominer.conf --log filominer.log "$@"
    code=$?
    echo "[$(date '+%F %T')] miner exited with code $code - restart in 10 s" >> filominer.log
    [ "$code" -eq 130 ] && break
    echo "miner exited (code $code), restart in 10 s (Ctrl+C to quit)"
    sleep 10 || break
done
