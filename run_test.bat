@echo off
REM run_test.bat - correctness tests of dev\filominer.exe: CPU reference vs the KawPow
REM test vectors, then on every selected GPU: DAG items, full hashes (mix + final)
REM of 2 periods and the search kernel's candidate filter, all against the CPU.
REM Log: logs\test.log.  Extra arguments (e.g. --gpus 2) in run\test_args.txt
setlocal EnableExtensions
cd /d "%~dp0"
if not exist logs mkdir logs
set "PATH=%CD%\dev;C:\msys64\mingw64\bin;%PATH%"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
set "ARGS="
if exist run\test_args.txt set "ARGS=--args-file run\test_args.txt"
echo [%date% %time%] test start> logs\test.log
if exist run\test_args.txt type run\test_args.txt>> logs\test.log
dev\filominer.exe --test %ARGS% >> logs\test.log 2>&1
echo [%date% %time%] test exit=%ERRORLEVEL%>> logs\test.log
