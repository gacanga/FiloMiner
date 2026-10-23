@echo off
REM ===========================================================================
REM fpow_pool.bat - FiloMiner on the FiloPow pool (settings: filominer.conf, extra
REM   arguments in run\pool_args.txt, e.g. --gpus 2).
REM   Automatic restart 10 s after any exit.
REM   Log: logs\pool.log (appended).  Stop: stop_fpow.bat or close the window.
REM ===========================================================================
setlocal EnableExtensions
title FiloMiner - miner
cd /d "%~dp0"
if not exist logs mkdir logs
set "PATH=%CD%\bin;C:\msys64\mingw64\bin;%PATH%"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
:loop
set "ARGS="
if exist run\pool_args.txt set "ARGS=--args-file run\pool_args.txt"
set "FM_NVRTC_OPTIONS="
if exist run\nvrtc_options.txt set /p FM_NVRTC_OPTIONS=<run\nvrtc_options.txt
echo [%date% %time%] miner start %ARGS%>> logs\pool.log
bin\filominer.exe --config filominer.conf --log logs\pool.log %ARGS%
echo [%date% %time%] miner exited with code %ERRORLEVEL% - restart in 10 s>> logs\pool.log
echo [%date% %time%] arret du mineur (code %ERRORLEVEL%), relance dans 10 s
timeout /t 10 /nobreak >nul
goto loop
