#!/usr/bin/env bash
# package_linux.sh - portable Linux release package:
#   release/FiloMiner-v<ver>-linux-x64-cuda12.tar.gz (+ .sha256.txt)
# Generic x86-64 code; CUDA runtime, NVRTC, OpenSSL and C++ runtime linked statically.
set -euo pipefail
cd "$(dirname "$0")/.."
VER="$(sed -n 's/^#define FILOMINER_VERSION "\(.*\)"/\1/p' src/main.cpp)"
NAME="FiloMiner-v$VER-linux-x64-cuda12"
FM_HOST_ARCH="-march=x86-64 -mtune=generic" bash linux/build_linux.sh
OUT="release/$NAME"
rm -rf "$OUT" "release/$NAME.tar.gz"
mkdir -p "$OUT"
cp build/filominer "$OUT/"
for f in filominer.conf README.txt start.sh test.sh bench.sh guard.conf gpu-prep.sh filominer-guard.sh install-service.sh uninstall-service.sh; do
    cp "linux/dist/$f" "$OUT/"
done
cp LICENSE "$OUT/LICENSE.txt"; cp NOTICE "$OUT/NOTICE.txt"
chmod 755 "$OUT/filominer" "$OUT"/*.sh
# GPU-less self-checks: CPU reference and the built-in NVRTC compiler
TLOG="$(cd "$OUT" && ./filominer --config /dev/null --test 2>&1 || true)"
echo "$TLOG" | grep -q "TEST CPU reference: .* vectors OK" || { echo "$TLOG"; echo "CPU self-test FAILED"; exit 1; }
echo "$TLOG" | grep "TEST CPU reference"
(cd "$OUT" && ./filominer --config /dev/null --nvrtc-check)
tar -C release --owner=0 --group=0 -czf "release/$NAME.tar.gz" "$NAME"
(cd release && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256.txt" && cat "$NAME.tar.gz.sha256.txt")
