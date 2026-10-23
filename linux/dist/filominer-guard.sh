#!/usr/bin/env bash
# filominer-guard.sh - run as root by filominer-guard.service. Every minute: when
# filominer.service is active but the miner has printed no summary line for
# STALL_MIN minutes, the GPU is checked:
#   - no NVIDIA device on the PCI bus at all  -> log only (hardware removed)
#   - GPU lost (dmesg "fallen off the bus" / Xid 79, nvidia-smi failing) -> reboot
#   - GPU answering -> restart the service once; still stalled after STALL_MIN -> reboot
# At most MAX_REBOOTS reboots per REBOOT_WINDOW_H hours, nothing during GRACE_MIN after boot.
# Usage: filominer-guard.sh <miner directory>
DIR="$1"
LOG="$DIR/filominer.log"
GLOG="$DIR/guard.log"
STATE=/var/lib/filominer-guard
mkdir -p "$STATE"
STALL_MIN=10; MAX_REBOOTS=3; REBOOT_WINDOW_H=6; GRACE_MIN=5
glog() { echo "[$(date '+%F %T')] $*" >> "$GLOG"; }
last_summary_epoch() {
    # log lines start with "[YYYY-MM-DD HH:MM:SS]"; NUL bytes left by hard stops removed
    local l
    l="$(tail -c 400000 "$LOG" 2>/dev/null | tr -d '\000' | grep -a "MH/s total" | tail -1)"
    [ -n "$l" ] || { echo 0; return; }
    date -d "$(echo "$l" | sed -n 's/^\[\([0-9-]* [0-9:]*\)\].*/\1/p')" +%s 2>/dev/null || echo 0
}
service_start_epoch() {
    local t
    t="$(systemctl show -p ActiveEnterTimestamp --value filominer.service 2>/dev/null)"
    [ -n "$t" ] && date -d "$t" +%s 2>/dev/null || echo 0
}
gpu_lost() {
    dmesg 2>/dev/null | tail -n 300 | grep -qiE "fallen off the bus|Xid.*79" && return 0
    timeout 20 nvidia-smi -L >/dev/null 2>&1 || return 0
    return 1
}
can_reboot() {
    local now cutoff n
    now=$(date +%s); cutoff=$((now - REBOOT_WINDOW_H * 3600))
    touch "$STATE/reboots"
    awk -v c="$cutoff" '$1 >= c' "$STATE/reboots" > "$STATE/reboots.tmp" && mv "$STATE/reboots.tmp" "$STATE/reboots"
    n=$(wc -l < "$STATE/reboots")
    [ "$n" -lt "$MAX_REBOOTS" ]
}
do_reboot() {
    if can_reboot; then
        date +%s >> "$STATE/reboots"
        glog "REBOOT: $*"
        sync
        systemctl reboot
        sleep 120
    else
        glog "reboot needed ($*) but $MAX_REBOOTS reboots already in ${REBOOT_WINDOW_H} h: nothing done"
    fi
}
glog "guard started (dir $DIR)"
restarted_at=0
while true; do
    sleep 60
    [ -f /etc/filominer/guard.conf ] && . /etc/filominer/guard.conf
    up=$(cut -d. -f1 /proc/uptime)
    [ "$up" -lt $((GRACE_MIN * 60)) ] && continue
    systemctl is-active --quiet filominer.service || { restarted_at=0; continue; }
    now=$(date +%s)
    last=$(last_summary_epoch)
    started=$(service_start_epoch)
    [ "$started" -gt "$last" ] && last=$started      # fresh start: DAG build + compile
    age=$((now - last))
    [ "$age" -lt $((STALL_MIN * 60)) ] && { restarted_at=0; continue; }
    if ! lspci -n 2>/dev/null | grep -qiE " 03[0-9a-f]{2}: 10de:"; then
        glog "stalled for $((age / 60)) min and no NVIDIA GPU on the PCI bus: nothing done"
        continue
    fi
    if gpu_lost; then
        do_reboot "GPU lost (stalled for $((age / 60)) min)"
        continue
    fi
    if [ "$restarted_at" -eq 0 ]; then
        glog "stalled for $((age / 60)) min, GPU answers: restarting filominer.service"
        systemctl restart filominer.service
        restarted_at=$now
    elif [ $((now - restarted_at)) -ge $((STALL_MIN * 60)) ]; then
        do_reboot "still stalled $((STALL_MIN)) min after a service restart"
        restarted_at=0
    fi
done
