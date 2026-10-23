#!/usr/bin/env bash
# uninstall-service.sh - removes the FiloMiner services:  sudo bash uninstall-service.sh
set -u
[ "$(id -u)" -eq 0 ] || { echo "run with sudo: sudo bash $0"; exit 1; }
systemctl disable --now filominer-guard.service filominer.service 2>/dev/null || true
rm -f /etc/systemd/system/filominer.service /etc/systemd/system/filominer-guard.service
rm -rf /usr/local/lib/filominer
systemctl daemon-reload
echo "FiloMiner services removed (/etc/filominer/guard.conf kept)."
