#!/usr/bin/env bash
# build_linux.sh - builds FiloMiner for Linux x86-64 (NVIDIA Pascal sm_61 and newer).
#
#   bash linux/build_linux.sh         -> build/filominer
#
# Requirements (Ubuntu/Debian):
#   * CUDA Toolkit 12.x (12.0 .. 12.9) with NVRTC (cuda-nvrtc-dev). CUDA 13 dropped
#     Pascal (GTX 10xx, P104/P106). Override with NVCC=/usr/local/cuda-12.9/bin/nvcc.
#   * g++ (C++17), libssl-dev:   sudo apt install build-essential libssl-dev
#
# Single binary: the CUDA runtime, NVRTC (+ builtins + PTX compiler), OpenSSL (when
# libssl.a is available) and the C++ runtime are linked statically; only the NVIDIA
# driver (libcuda.so.1) is needed at run time.
# Environment overrides: NVCC, CUDA_ARCH (default 61), FM_HOST_ARCH (default
# -march=native; the release uses "-march=x86-64 -mtune=generic").
set -euo pipefail
cd "$(dirname "$0")/.."

NVCC="${NVCC:-}"
if [ -z "$NVCC" ]; then
    if [ -x /usr/local/cuda-12.9/bin/nvcc ]; then NVCC=/usr/local/cuda-12.9/bin/nvcc
    elif command -v nvcc >/dev/null 2>&1; then NVCC="$(command -v nvcc)"
    elif [ -x /usr/local/cuda/bin/nvcc ]; then NVCC=/usr/local/cuda/bin/nvcc
    else echo "ERROR: nvcc not found (install the CUDA Toolkit 12.x or set NVCC=...)"; exit 1; fi
fi
VER="$("$NVCC" --version | sed -n 's/.*release \([0-9]*\)\.\([0-9]*\).*/\1.\2/p' | head -1)"
MAJOR="${VER%%.*}"
echo "nvcc: $NVCC (CUDA $VER)"
ARCH="${CUDA_ARCH:-61}"
if [ "$MAJOR" -ge 13 ] && [ "$ARCH" -lt 75 ]; then
    echo "ERROR: CUDA $VER cannot build for sm_$ARCH (Pascal support was removed in CUDA 13)."
    echo "       Install CUDA 12.x (e.g. 12.9) next to it and run: NVCC=/usr/local/cuda-12.9/bin/nvcc bash $0"
    exit 1
fi

CUDA_HOME="$(cd "$(dirname "$NVCC")/.." && pwd)"
CUDALIB=""
for d in "$CUDA_HOME/lib64" "$CUDA_HOME/targets/x86_64-linux/lib" /usr/lib/x86_64-linux-gnu; do
    if [ -f "$d/libcudart_static.a" ] && [ -f "$d/libnvrtc_static.a" ]; then CUDALIB="$d"; break; fi
done
[ -n "$CUDALIB" ] || { echo "ERROR: libcudart_static.a / libnvrtc_static.a not found under $CUDA_HOME (package cuda-nvrtc-dev)"; exit 1; }
STUBS="$CUDALIB/stubs"
[ -f "$STUBS/libcuda.so" ] || STUBS="$CUDA_HOME/lib64/stubs"

mkdir -p build/obj
python3 tools/embed_kernel.py >/dev/null
echo "== GPU engine (DAG kernel: sm_$ARCH SASS + compute_$ARCH PTX; search kernel: NVRTC at run time)"
"$NVCC" -O3 -std=c++17 -Wno-deprecated-gpu-targets -allow-unsupported-compiler \
    -gencode "arch=compute_$ARCH,code=[sm_$ARCH,compute_$ARCH]" \
    -Igpu -Isrc -c gpu/fm_gpu.cu -o build/obj/fm_gpu.o

echo "== ethash library (FiloPow sources) + host program"
HOSTARCH="${FM_HOST_ARCH:--march=native}"
E=third_party/crypto/ethash/lib
for c in keccak/keccak keccak/keccakf1600 keccak/keccakf800 ethash/primes; do
    gcc -O2 $HOSTARCH -Ithird_party -c "$E/$c.c" -o "build/obj/$(basename $c).o"
done
FLAGS="-O2 $HOSTARCH -std=gnu++17 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Isrc -Igpu -Ithird_party"
SSL="-lssl -lcrypto"
for d in /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib; do
    if [ -f "$d/libssl.a" ] && [ -f "$d/libcrypto.a" ]; then SSL="$d/libssl.a $d/libcrypto.a"; break; fi
done
# glibc compatibility layer: a binary built on a recent distribution still runs
# on glibc 2.35+ (see linux/glibc_compat.c)
gcc -O2 -std=c11 -c linux/glibc_compat.c -o build/obj/glibc_compat.o
WRAP=""
for s in __isoc23_strtol __isoc23_strtoul __isoc23_strtoll __isoc23_strtoull __isoc23_fscanf __isoc23_sscanf \
         arc4random arc4random_buf arc4random_uniform fmod; do WRAP="$WRAP -Wl,--wrap=$s"; done
g++ $FLAGS -o build/filominer src/main.cpp src/kawpow_ref.cpp src/tls_conn.cpp $E/ethash/ethash.cpp $E/ethash/progpow.cpp \
    build/obj/keccak.o build/obj/keccakf1600.o build/obj/keccakf800.o build/obj/primes.o build/obj/fm_gpu.o build/obj/glibc_compat.o \
    -L"$CUDALIB" -lnvrtc_static -lnvrtc-builtins_static -lnvptxcompiler_static -lcudart_static -L"$STUBS" -lcuda \
    $SSL -static-libstdc++ -static-libgcc -lpthread -ldl -lrt -lm $WRAP
MAXGLIBC="$(objdump -T build/filominer 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1 || true)"
[ -n "$MAXGLIBC" ] && echo "   needs $MAXGLIBC or newer"
echo "   dynamic libraries: $(objdump -p build/filominer | awk '/NEEDED/{print $2}' | tr '\n' ' ')"
echo "== OK: build/filominer"
echo "   first run:  ./build/filominer --test        (CPU reference + every GPU against the CPU)"
echo "   then:       ./build/filominer -o stratum+tcp://pool.filopow.xyz:3333 -u FPOW_ADDRESS.rig"
