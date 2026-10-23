FiloMiner - FiloPow (FPOW, KawPow) GPU miner for NVIDIA, Linux x86-64. No dev fee.

Requirements
  - NVIDIA driver 525 or newer. The CUDA 12 runtime and NVRTC are built in:
    no CUDA toolkit needed.
  - glibc 2.35 or newer (Ubuntu 22.04+, Debian 12+).
  - Pascal (GTX 10xx, P104-100, P106-100) or any newer NVIDIA GPU, 4 GB or more
    (the DAG is about 1.2 GB in late 2026 and grows by 8 MB every 5 days).

Manual use
  0. Stop any other miner on the GPU(s).
  1. filominer.conf: replace YOUR_FPOW_ADDRESS on the "user" line (FPOW_ADDRESS.rig_name)
  2. ./test.sh     must end with "TEST RESULT: ALL OK"  (about 1 min per GPU)
  3. ./bench.sh    optional (P104-100: about 16 MH/s per card, stock)
  4. ./start.sh    mines on pool.filopow.xyz, restarts by itself; log filominer.log

As a service (starts at boot, restarts after any stop, GPU guard)
  sudo bash install-service.sh      installs filominer.service + filominer-guard.service
                                    and starts FiloMiner (stop any other miner service
                                    on the same GPU first: they would fight for it).
  sudo bash uninstall-service.sh    removes the FiloMiner services.
  Settings: /etc/filominer/guard.conf (power limit, guard delays).
  Logs: filominer.log (miner), guard.log (guard), journalctl -u filominer.

At start the DAG is built on each GPU (about 13 s) and the search kernel is compiled
for the current ProgPoW period (about 1.5 s, then every 3 blocks in the background).
Every share is recomputed on the CPU before it is sent ("GPU ERRORS" otherwise).

Exit codes: 3 = CUDA error, 4 = a GPU stopped progressing (unstable clocks),
5 = --test/--bench deadline.  ./filominer --nvrtc-check  checks the built-in
compiler without a GPU.

License: Apache-2.0 (LICENSE.txt, NOTICE.txt).
