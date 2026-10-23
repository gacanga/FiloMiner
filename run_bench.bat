@echo off
REM run_bench.bat - hashrate benchmark of dev\filominer.exe (30 s by default).
REM Extra arguments (e.g. --gpus 2 --tpb 128 --bench 60) in run\bench_args.txt
REM NVRTC options for an experiment (e.g. --maxrregcount=72) in run\nvrtc_options.txt
REM Log: logs\bench.log (overwritten), history appended to logs\bench_history.log
setlocal EnableExtensions
cd /d "%~dp0"
if not exist logs mkdir logs
set "PATH=%CD%\dev;C:\msys64\mingw64\bin;%PATH%"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
set "ARGS="
if exist run\bench_args.txt set "ARGS=--args-file run\bench_args.txt"
set "FM_NVRTC_OPTIONS="
if exist run\nvrtc_options.txt set /p FM_NVRTC_OPTIONS=<run\nvrtc_options.txt
echo [%date% %time%] bench start> logs\bench.log
if exist run\bench_args.txt type run\bench_args.txt>> logs\bench.log
if exist run\nvrtc_options.txt type run\nvrtc_options.txt>> logs\bench.log
dev\filominer.exe --bench 30 %ARGS% >> logs\bench.log 2>&1
echo [%date% %time%] bench exit=%ERRORLEVEL%>> logs\bench.log
type logs\bench.log >> logs\bench_history.log
