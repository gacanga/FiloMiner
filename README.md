# FiloMiner

Version française : [README.fr.md](README.fr.md)

GPU miner (NVIDIA, CUDA) for **FiloPow (FPOW)** and other KawPow coins that use the Ravencoin
constants. Written for and tested on NVIDIA **P104-100** (Pascal, sm_61) cards.

**No developer fee.** Every share found goes to your address. Apache-2.0.

Windows release zip: https://github.com/gacanga/FiloMiner/releases

## What it mines

- **Coin:** FiloPow (FPOW) — https://github.com/filoproject/filopow
- **Algorithm:** KawPow (ProgPoW 0.9.4: period 3 blocks, epoch 7 500 blocks, DAG items with 512
  parents), identical to Ravencoin's; FiloPow only renamed the constants array. Other KawPow coins
  with the Ravencoin constants should work too (only FPOW was tested).
- **Pool protocol:** KawPow NOMP / kawpow-stratum
  (`mining.notify [job, header_hash, seed_hash, target, clean, height, nbits]`), plain TCP or TLS.

## How it works

- The DAG is built on the GPU from the light cache (one build per epoch).
- The search kernel is compiled **at run time by NVRTC** for each ProgPoW period (every 3 blocks),
  and the next period's kernel is compiled ahead in the background, so mining never waits for the
  compiler.
- Every GPU candidate is **recomputed on the CPU** (ethash reference library) before it is sent;
  a mismatch is reported as a GPU error (usually unstable clocks) and the share is not submitted.
- Watchdogs: a GPU whose launch never returns makes the miner exit with code 4, a CUDA error with
  code 3, so the launcher restarts it.

## Supported GPUs

NVIDIA Pascal (GTX 10xx, P104-100, P106-100) and newer, with 4 GB of memory or more (the FPOW DAG is
about 1.2 GB in late 2026 and grows by 8 MB every 5 days). The fixed part of the GPU code is built for
sm_61 + PTX, so newer cards JIT it; the search kernel is compiled by NVRTC for the actual card.
Needs an NVIDIA driver R5xx or newer that supports CUDA 12 (CUDA 12.9 is the last toolkit that builds
for Pascal).

## Quick start (Windows release zip)

1. Download the zip from https://github.com/gacanga/FiloMiner/releases and unzip it.
2. Edit `filominer.conf`: replace `YOUR_FPOW_ADDRESS` on the `user` line with your FPOW address
   (starts with `F`), keep `.rig1` or use any rig name.
3. Optional: open a command prompt in the folder and run `filominer.exe --test` — it must end with
   `TEST RESULT: ALL OK`.
4. Double-click `start_fpow.bat` (restarts the miner 10 s after any exit; log `filominer.log`).

Command line equivalent:

```
filominer.exe -o stratum+tcp://pool.filopow.xyz:3333 -u YOUR_FPOW_ADDRESS.rig1 -p x
filominer.exe --test        # CPU reference + GPU correctness tests
filominer.exe --bench 60    # hashrate benchmark (60 s)
```

## Configuration reference

`filominer.conf` is read from the folder of `filominer.exe` (or its parent folder), or from the file
given with `--config`. Each line is `key = value` (the option name without `--`); `#` or `;` start a
comment. Command-line options override the file; `-o` on the command line replaces the pools of the
file.

| Option (CLI) | Config key | Default | Meaning |
|---|---|---|---|
| `-o`, `--pool`, `--url URL` | `pool` / `url` / `o` | — | Pool URL, repeatable (failover order). `stratum+tcp://` = plain TCP, `stratum+ssl://` / `stratum+tls://` (or no `tcp` in the scheme) = TLS |
| `-u`, `--user ADDR.RIG` | `user` / `u` | — | FPOW address, a dot, a rig name |
| `-p`, `--pass X` | `pass` / `p` | `x` | Pool password |
| `--gpus 0,1\|all` | `gpus` / `devices` | `all` | GPUs to use (CUDA indices; the launchers set `CUDA_DEVICE_ORDER=PCI_BUS_ID`, i.e. the `nvidia-smi` order) |
| `--tpb N` | `tpb` | `256` | Threads per block (multiple of 32, 32..1024) |
| `--grid N` | `grid` | `0` | Blocks per launch, `0` = automatic from `--target-ms` |
| `--target-ms N` | `target-ms` | `150` | Duration of one GPU launch for the automatic grid |
| `--stats N` | `stats` | `30` | Seconds between summary lines (minimum 5) |
| `--log FILE` | `log` | — | Append the log to a file |
| `--cafile FILE` | `cafile` | see below | CA bundle for TLS pools |
| `--bench-height N` | `bench-height` | `150000` | Block height (epoch / period) used by `--bench`, `--nvrtc-check` |
| `--config FILE`, `-c FILE` | — | `filominer.conf` | Config file |
| `--args-file FILE` | — | — | Read more arguments from a file (whitespace separated) |
| `--test` | — | — | CPU reference vs the KawPow test vectors, then on every selected GPU: DAG items, full hashes (mix + final) of 2 periods, the search kernel's candidate filter |
| `--bench [s]` | — | 30 | Hashrate benchmark |
| `--nvrtc-check` | — | — | Compiles one period's kernel for sm_61 without a GPU (checks the NVRTC DLLs). **Source only / next release:** not in the v0.1.0 Windows binary |
| `--help`, `-h` | — | — | Usage |

TLS certificate verification uses `ca-bundle.crt` next to the exe if present, else
`C:\msys64\mingw64\etc\ssl\certs\ca-bundle.crt` or `/etc/ssl/certs/ca-certificates.crt`.

Environment variables: `FM_NVRTC_OPTIONS` (extra NVRTC flags, e.g. `--maxrregcount=72`),
`FM_DUMP_DIR` (keep the generated kernel sources and cubins in that folder).

Exit codes: 2 = bad arguments / missing pool or user, 3 = CUDA error, 4 = a GPU stopped progressing
(unstable clocks), 5 = `--test` / `--bench` deadline.

## Pool

Official pool **pool.filopow.xyz** (as configured in `filominer.conf`):

- `stratum+tcp://pool.filopow.xyz:3333` — start difficulty 0.1
- `stratum+tcp://pool.filopow.xyz:4444` — difficulty 1
- pool fee 1 %, PROP, payout every 10 min, minimum 1 FPOW (pool terms at the time of writing)

## Performance (measured)

NVIDIA P104-100, default settings (`tpb 256`, automatic grid):

| | |
|---|---|
| Hashrate | about **16 MH/s** per card (stock card settings) |
| DAG build at start / new epoch | about 13 s per GPU |
| NVRTC compile of a period's kernel | about 1.5 s (then in the background, every 3 blocks) |

## Build

### Windows

Expected toolchain (paths are the installers' defaults):

- **CUDA 12.9** (`CUDA_PATH_V12_9`, else `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9`)
- **Visual Studio 2022** with the C++ x64 tools (found with `vswhere`) — builds `fmgpu.dll` with nvcc
- **MSYS2** in `C:\msys64`, MINGW64 environment, packages `mingw-w64-x86_64-gcc` and
  `mingw-w64-x86_64-openssl` — builds `filominer.exe` (statically linked when possible)

```
build_all.bat            builds dev\ (fmgpu.dll, filominer.exe, CUDA/NVRTC DLLs, ca-bundle.crt), then runs the tests
build_all.bat nobuild    tests only
run_test.bat             tests of dev\filominer.exe        -> logs\test.log  ("TEST RESULT: ALL OK")
run_bench.bat            30 s benchmark of dev\            -> logs\bench.log
deploy.bat               dev\ -> bin\ (refused unless the last tests are OK)
fpow_pool.bat            mines with bin\filominer.exe and filominer.conf, auto-restart, logs\pool.log
stop_fpow.bat            stops fpow_pool.bat and filominer.exe
```

Logs go to `logs\`. Optional tuning files in `run\`: `nvcc_extra.txt` (extra nvcc flags),
`nvrtc_options.txt` (`FM_NVRTC_OPTIONS`), `test_args.txt`, `bench_args.txt`, `pool_args.txt` (extra
miner arguments, e.g. `--gpus 2`). `FM_HOST_ARCH` overrides `-march=native` for the host program
(release builds: `-march=x86-64 -mtune=generic`); `FM_CRT=/MT` builds the DLL with the static C runtime.

The search kernel source (`gpu/kawpow_kernel.cuh`) is embedded in the DLL through the generated
`gpu/kawpow_kernel_src.h`; after editing the kernel, run `python tools/embed_kernel.py`.

### Linux

```
bash linux/build_linux.sh        # -> build/filominer
bash linux/package_linux.sh      # -> release/FiloMiner-v<ver>-linux-x64-cuda12.tar.gz
```

Needs CUDA Toolkit 12.x (12.0 .. 12.9) with NVRTC (`cuda-nvrtc-dev`), g++ (C++17), `libssl-dev`,
python3. The CUDA runtime, NVRTC, OpenSSL (when `libssl.a` exists) and the C++ runtime are linked
statically, so the binary needs only the NVIDIA driver; it runs on glibc 2.35+ (see
`linux/glibc_compat.c`). Overrides: `NVCC`, `CUDA_ARCH` (default 61), `FM_HOST_ARCH`. The Linux package
(`linux/dist/`) has `start.sh`, `test.sh`, `bench.sh` and an optional systemd service with a GPU guard
(`install-service.sh`).

## Tests

- `filominer --test` (or `run_test.bat`, `linux/dist/test.sh`): ethash reference vs the KawPow test
  vectors, then on each GPU the DAG items, full hashes of 2 periods and the candidate filter against
  the CPU.
- GPU-less checks: `tests/ref_test.cpp` (reference vs test vectors, DAG item code vs the library) and
  `tools/sim_test.sh` (runs the generated KawPow kernel on the CPU through `tests/sim_kernel.cpp` and
  compares 16 nonces per test vector with the reference).

## Layout

```
src/          host program: stratum, config, CPU verification (kawpow_ref), TLS, JSON
gpu/          CUDA engine (fmgpu.dll): DAG build, ProgPoW program generator, NVRTC search kernel
third_party/  ethash / ProgPoW reference library (KawPow variant), Apache-2.0
windows/      Windows build scripts (nvcc + MSVC for the DLL, MSYS2 g++ for the exe)
linux/        Linux build, packaging and release scripts
tests/ tools/ GPU-less tests, kernel embedding and program generator
dist/         non-binary files of the Windows release zip
```

## License

Apache License 2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). `third_party/crypto/ethash` is
Copyright 2018-2019 Pawel Bylica, Apache-2.0 (taken from the FiloPow sources, which take it from
Ravencoin).
