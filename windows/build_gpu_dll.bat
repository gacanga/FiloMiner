@echo off
REM ===========================================================================
REM build_gpu_dll.bat - builds <dir>\fmgpu.dll (CUDA KawPow engine) with nvcc + MSVC.
REM   Target : NVIDIA P104-100 (Pascal, sm_61) + PTX, CUDA 12.9, Visual Studio 2022.
REM   The search kernel itself is compiled at run time by NVRTC (one program per
REM   ProgPoW period): nvrtc64_*.dll and nvrtc-builtins64_*.dll are copied next to it.
REM   Output : <dir>\fmgpu.dll, cudart64_12.dll, nvrtc DLLs   (dir = %1, default dev)
REM   Env    : FM_CRT=/MT for a static C runtime, FM_NVCC_EXTRA = extra nvcc flags
REM   Log    : logs\build_gpu_dll.log
REM ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0.."
set "OUT=%~1"
if "%OUT%"=="" set "OUT=dev"
if "%FM_CRT%"=="" set "FM_CRT=/MD"
if not exist logs mkdir logs
if not exist "%OUT%" mkdir "%OUT%"
set "LOG=%CD%\logs\build_gpu_dll.log"
(echo [%date% %time%] build_gpu_dll start> "%LOG%") 2>nul || set "LOG=%CD%\logs\build_gpu_dll_%RANDOM%.log"

set "CUDA129=%CUDA_PATH_V12_9%"
if "%CUDA129%"=="" set "CUDA129=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9"
if not exist "%CUDA129%\bin\nvcc.exe" (
    echo ERROR: CUDA 12.9 nvcc.exe not found >> "%LOG%"
    echo ERROR: CUDA 12.9 nvcc.exe not found.
    exit /b 1
)
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSINSTALL="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
)
if "%VSINSTALL%"=="" set "VSINSTALL=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: vcvars64.bat not found under "%VSINSTALL%" >> "%LOG%"
    echo ERROR: MSVC x64 tools not found.
    exit /b 1
)
set VSCMD_SKIP_SENDTELEMETRY=1
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
where cl >nul 2>&1
if errorlevel 1 (
    echo ERROR: MSVC environment setup failed >> "%LOG%"
    echo ERROR: MSVC environment setup failed.
    exit /b 1
)
set "PATH=%CUDA129%\bin;%PATH%"
nvcc --version >> "%LOG%" 2>&1

if exist "%OUT%\fmgpu.dll" del /q "%OUT%\fmgpu.dll"
if exist "%OUT%\fmgpu.dll" (
    echo ERROR: %OUT%\fmgpu.dll is in use and cannot be replaced >> "%LOG%"
    echo ERROR: %OUT%\fmgpu.dll is in use
    exit /b 1
)
echo [%date% %time%] compiling gpu\fm_gpu.cu >> "%LOG%"
echo   compiling fm_gpu.cu ...
nvcc -O3 %FM_NVCC_EXTRA% -gencode arch=compute_61,code=[sm_61,compute_61] -Wno-deprecated-gpu-targets -allow-unsupported-compiler -std=c++17 -Igpu -Isrc ^
     -Xptxas -v -Xcompiler "%FM_CRT% /O2 /EHsc /W3" -cudart shared --shared ^
     -o "%OUT%\fmgpu.dll" gpu\fm_gpu.cu -lcuda -lnvrtc -Xlinker /DEF:windows\fmgpu.def >> "%LOG%" 2>&1
if not exist "%OUT%\fmgpu.dll" (
    echo [%date% %time%] fm_gpu.cu FAILED >> "%LOG%"
    echo ERROR: fmgpu.dll build failed, see logs\build_gpu_dll.log
    exit /b 1
)
copy /y "%CUDA129%\bin\cudart64_12.dll" "%OUT%\" >> "%LOG%" 2>&1
copy /y "%CUDA129%\bin\nvrtc64_*.dll" "%OUT%\" >> "%LOG%" 2>&1
copy /y "%CUDA129%\bin\nvrtc-builtins64_*.dll" "%OUT%\" >> "%LOG%" 2>&1
dir /b "%OUT%\*.dll" >> "%LOG%" 2>&1
echo [%date% %time%] build_gpu_dll OK >> "%LOG%"
echo fmgpu.dll OK
exit /b 0
