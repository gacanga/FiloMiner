FiloMiner 0.1.0 - GPU miner for FiloPow (FPOW, KawPow), NVIDIA, Windows x64
No developer fee: every share goes to your address.
Source, documentation and new versions: https://github.com/gacanga/FiloMiner

Requirements
  - NVIDIA Pascal (GTX 10xx, P104-100, P106-100) or newer GPU with 4 GB or more.
  - NVIDIA driver R5xx or newer (CUDA 12). No CUDA toolkit needed: the CUDA
    runtime and NVRTC DLLs are in this folder.

Start
  1. Open filominer.conf with Notepad and replace YOUR_FPOW_ADDRESS on the
     "user" line with your FPOW address (starts with F). ".rig1" is the rig
     name shown by the pool; change it if you like.
  2. Optional check: open a command prompt in this folder and run
         filominer.exe --test
     It must end with "TEST RESULT: ALL OK" (about 1 minute per GPU).
  3. Double-click start_fpow.bat. The miner restarts by itself 10 s after any
     exit; the log is filominer.log. Close the window to stop.

At start the DAG is built on each GPU (about 13 s) and the search kernel is
compiled for the current ProgPoW period (about 1.5 s); then shares start.
Every share is recomputed on the CPU before it is sent ("GPU ERRORS" in the
summary line means unstable clocks).

Other commands
  filominer.exe --bench 60         60 s hashrate benchmark
  filominer.exe --gpus 0,2 ...     use only some GPUs (nvidia-smi order)
  filominer.exe --help             all options (see the README on GitHub)

Pool: pool.filopow.xyz, port 3333 (start difficulty 0.1) or 4444 (difficulty 1).

License: Apache-2.0 (LICENSE, NOTICE).
