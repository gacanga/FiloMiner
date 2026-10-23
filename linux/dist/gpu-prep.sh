#!/usr/bin/env bash
# gpu-prep.sh - run as root by filominer.service before each start (ExecStartPre=+):
# persistence mode; POWER_LIMIT from /etc/filominer/guard.conf only if set (0 = unchanged). Never fatal.
POWER_LIMIT=0
[ -f /etc/filominer/guard.conf ] && . /etc/filominer/guard.conf
command -v nvidia-smi >/dev/null 2>&1 || exit 0
timeout 20 nvidia-smi -pm 1 >/dev/null 2>&1 || true
if [ "${POWER_LIMIT:-0}" -gt 0 ] 2>/dev/null; then
    timeout 20 nvidia-smi -pl "$POWER_LIMIT" >/dev/null 2>&1 || echo "gpu-prep: power limit $POWER_LIMIT W not applied"
fi
exit 0
