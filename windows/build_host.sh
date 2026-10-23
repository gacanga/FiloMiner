#!/usr/bin/env bash
# build_host.sh - builds <dir>/filominer.exe (MSYS2 MINGW64 g++ + OpenSSL + the ethash
# library of the FiloPow sources), linked directly against <dir>/fmgpu.dll.
# Log: logs/build_host.log.  FM_HOST_ARCH overrides -march=native
# (release: "-march=x86-64 -mtune=generic").
set -u
cd "$(dirname "$0")/.."
OUT="${1:-dev}"
mkdir -p logs
LOG=logs/build_host.log
: > "$LOG"
echo "[$(date '+%F %T')] build_host start" >> "$LOG"
g++ --version | head -1 >> "$LOG"
if [ ! -f "$OUT/fmgpu.dll" ]; then
    echo "ERROR: $OUT/fmgpu.dll missing" | tee -a "$LOG"
    exit 1
fi
if [ ! -f /mingw64/include/openssl/ssl.h ]; then
    echo "ERROR: OpenSSL headers missing (/mingw64/include/openssl). Package: mingw-w64-x86_64-openssl" | tee -a "$LOG"
    exit 1
fi
ARCH="${FM_HOST_ARCH:--march=native}"
OBJ=logs/obj
mkdir -p "$OBJ"
E=third_party/crypto/ethash/lib
for c in keccak/keccak keccak/keccakf1600 keccak/keccakf800 ethash/primes; do
    if ! gcc -O2 $ARCH -Ithird_party -c "$E/$c.c" -o "$OBJ/$(basename $c).o" >> "$LOG" 2>&1; then
        echo "[$(date '+%F %T')] HOST BUILD FAILED ($c.c)" | tee -a "$LOG"; exit 1
    fi
done
FLAGS="-O2 $ARCH -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc -Igpu -Ithird_party"
SRC="src/main.cpp src/kawpow_ref.cpp src/tls_conn.cpp $E/ethash/ethash.cpp $E/ethash/progpow.cpp"
LIBOBJ="$OBJ/keccak.o $OBJ/keccakf1600.o $OBJ/keccakf800.o $OBJ/primes.o"
if g++ $FLAGS -o "$OUT/filominer.exe" $SRC $LIBOBJ "$OUT/fmgpu.dll" -static -lssl -lcrypto -lws2_32 -lcrypt32 -lgdi32 -luser32 -ladvapi32 -lbcrypt -pthread >> "$LOG" 2>&1; then
    echo "static link OK" >> "$LOG"
else
    echo "static link failed, trying dynamic link" >> "$LOG"
    if ! g++ $FLAGS -o "$OUT/filominer.exe" $SRC $LIBOBJ "$OUT/fmgpu.dll" -lssl -lcrypto -lws2_32 -lcrypt32 -pthread >> "$LOG" 2>&1; then
        echo "[$(date '+%F %T')] HOST BUILD FAILED" | tee -a "$LOG"
        exit 1
    fi
    echo "dynamic link OK (needs C:\\msys64\\mingw64\\bin in PATH)" >> "$LOG"
fi
if [ -f /mingw64/etc/ssl/certs/ca-bundle.crt ]; then cp -f /mingw64/etc/ssl/certs/ca-bundle.crt "$OUT/"; fi
echo "[$(date '+%F %T')] host OK" | tee -a "$LOG"
exit 0
