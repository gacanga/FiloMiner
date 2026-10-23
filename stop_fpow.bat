@echo off
REM stop_fpow.bat - stops FiloMiner: the auto-restart loop(s) (fpow_pool.bat),
REM then any remaining filominer.exe.   stop_fpow.bat nopause  (no key press)
REM Other miners are not touched.
taskkill /fi "WINDOWTITLE eq FiloMiner - miner*" /t /f >nul 2>&1
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='cmd.exe'\" | Where-Object { $_.CommandLine -match 'fpow_pool\.bat' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }" >nul 2>&1
timeout /t 2 /nobreak >nul
taskkill /im filominer.exe /t /f >nul 2>&1
cd /d "%~dp0"
if not exist logs mkdir logs
echo [%date% %time%] miner stopped by stop_fpow.bat>> logs\pool.log
tasklist /fi "imagename eq filominer.exe" 2>nul | find /i "filominer.exe" >nul
if errorlevel 1 (echo Mineur arrete.) else (echo filominer.exe tourne encore.)
if /i not "%~1"=="nopause" pause
