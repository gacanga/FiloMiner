#!/usr/bin/env bash
# install-service.sh - installs FiloMiner as systemd services (run with sudo from the
# miner directory):  sudo bash install-service.sh
#   filominer.service        the miner (starts at boot, restarts after any stop)
#   filominer-guard.service  GPU stall / lost-GPU guard (see filominer-guard.sh)
# Stop any other miner service that uses the same GPU(s) before installing.
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "run with sudo: sudo bash $0"; exit 1; }
DIR="$(cd "$(dirname "$0")" && pwd)"
USER_NAME="${SUDO_USER:-$(stat -c %U "$DIR")}"
[ -x "$DIR/filominer" ] || chmod 755 "$DIR/filominer"
"$DIR/filominer" --config /dev/null --nvrtc-check >/dev/null 2>&1 || { echo "ERROR: $DIR/filominer --nvrtc-check failed"; exit 1; }
grep -q '^user *= *F' "$DIR/filominer.conf" || echo "WARNING: the user line of filominer.conf does not start with an F address"

install -d -m 755 /usr/local/lib/filominer /etc/filominer
install -m 755 -o root -g root "$DIR/gpu-prep.sh" /usr/local/lib/filominer/gpu-prep.sh
install -m 755 -o root -g root "$DIR/filominer-guard.sh" /usr/local/lib/filominer/filominer-guard.sh
[ -f /etc/filominer/guard.conf ] || install -m 644 "$DIR/guard.conf" /etc/filominer/guard.conf

cat > /etc/systemd/system/filominer.service <<UNIT
[Unit]
Description=FiloMiner (FiloPow / FPOW, KawPow GPU miner)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$USER_NAME
WorkingDirectory=$DIR
Environment=CUDA_DEVICE_ORDER=PCI_BUS_ID
ExecStartPre=+/usr/local/lib/filominer/gpu-prep.sh
ExecStart=$DIR/filominer --config $DIR/filominer.conf --log $DIR/filominer.log
Restart=always
RestartSec=10
KillSignal=SIGINT
TimeoutStopSec=20

[Install]
WantedBy=multi-user.target
UNIT

cat > /etc/systemd/system/filominer-guard.service <<UNIT
[Unit]
Description=FiloMiner GPU guard (restart / reboot on a stalled or lost GPU)
After=filominer.service

[Service]
Type=simple
ExecStart=/usr/local/lib/filominer/filominer-guard.sh $DIR
Restart=always
RestartSec=30

[Install]
WantedBy=multi-user.target
UNIT

touch "$DIR/filominer.log" "$DIR/guard.log"
chown "$USER_NAME" "$DIR/filominer.log" "$DIR/guard.log"
systemctl daemon-reload
systemctl enable --now filominer.service filominer-guard.service
sleep 3
systemctl --no-pager --lines=0 status filominer.service || true
echo
echo "FiloMiner installed. Log: tail -f $DIR/filominer.log   (first shares after ~30 s: DAG + compile)"
echo "Settings: /etc/filominer/guard.conf (POWER_LIMIT, guard delays)."
