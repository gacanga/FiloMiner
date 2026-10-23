@echo off
REM ===========================================================================
REM start_fpow.bat - FiloMiner launcher of the release zip: mines with
REM   filominer.conf (same folder), restarts 10 s after any exit.
REM   Extra arguments are passed to filominer.exe (e.g. start_fpow.bat --gpus 0,1).
REM   Log: filominer.log (appended).  Stop: close this window.
REM ===========================================================================
setlocal EnableExtensions
title FiloMiner - miner
cd /d "%~dp0"
set CUDA_DEVICE_ORDER=PCI_BUS_ID
findstr /r /c:"^user *= *YOUR_FPOW_ADDRESS" "%~dp0filominer.conf" >nul 2>&1
if not errorlevel 1 (
    echo Edit filominer.conf first: put your FPOW address on the "user" line.
    pause
    exit /b 1
)
:loop
"%~dp0filominer.exe" --config "%~dp0filominer.conf" --log "%~dp0filominer.log" %*
set "RC=%ERRORLEVEL%"
if "%RC%"=="2" (
    echo Configuration error, see the messages above.
    pause
    exit /b 2
)
echo [%date% %time%] miner exited with code %RC% - restart in 10 s>> "%~dp0filominer.log"
echo Miner exited (code %RC%), restart in 10 s - close this window to stop.
timeout /t 10 /nobreak >nul
goto loop
