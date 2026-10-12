@echo off
setlocal

:: ============================================================================
::  GPU ROP Probe - Run Script
::  尝试锁定频率，不支持则继续以默认频率运行
:: ============================================================================

set "TARGET_CLOCK=2500"
set "PROBE_EXE=%~dp0gpu_rop_probe.exe"
set "LOCK_ACQUIRED=0"

echo ============================================================
echo   GPU ROP Probe - Run
echo ============================================================
echo(

:: --------------------------------------------------------------------------
:: 管理员检查
:: --------------------------------------------------------------------------
net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Run as Administrator.
    exit /b 1
)
echo [INFO] Administrator privileges confirmed.
echo(

:: --------------------------------------------------------------------------
:: 检查 nvidia-smi
:: --------------------------------------------------------------------------
where nvidia-smi >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [WARN] nvidia-smi not found. Skipping clock management.
    goto :run_test
)
echo [INFO] nvidia-smi found.
echo(

:: --------------------------------------------------------------------------
:: 尝试锁定频率（失败不中断）
:: --------------------------------------------------------------------------
echo [INFO] Attempting to lock GPU clock to %TARGET_CLOCK% MHz...
nvidia-smi --lock-gpu-clocks=%TARGET_CLOCK% >nul 2>&1

if %ERRORLEVEL% neq 0 (
    echo [WARN] Clock locking is not supported on this platform.
    echo [WARN] Continuing with default clock management.
    echo [WARN] ROP count may show higher variance.
    set "LOCK_ACQUIRED=0"
) else (
    echo [INFO] Clock locked to %TARGET_CLOCK% MHz.
    set "LOCK_ACQUIRED=1"
)
echo(

:: --------------------------------------------------------------------------
:: 打印当前频率
:: --------------------------------------------------------------------------
echo [INFO] Current GPU clock (sample):
nvidia-smi --query-gpu=clocks.gr,clocks.mem --format=csv,noheader 2>nul
echo(

:: --------------------------------------------------------------------------
:: 运行测试
:: --------------------------------------------------------------------------
:run_test
if not exist "%PROBE_EXE%" (
    echo [ERROR] %PROBE_EXE% not found. Build first.
    if "%LOCK_ACQUIRED%"=="1" nvidia-smi --reset-gpu-clocks >nul 2>&1
    exit /b 1
)

echo [INFO] Running gpu_rop_probe.exe with GPU_CLOCK_MHZ=%TARGET_CLOCK% ...
echo(

set "GPU_CLOCK_MHZ=%TARGET_CLOCK%"
"%PROBE_EXE%"
set "PROBE_EXIT=%ERRORLEVEL%"

echo(
echo [INFO] Probe finished with exit code %PROBE_EXIT%.
echo(

:: --------------------------------------------------------------------------
:: 解锁（仅当之前锁成功过）
:: --------------------------------------------------------------------------
if "%LOCK_ACQUIRED%"=="1" (
    echo [INFO] Resetting GPU clocks...
    nvidia-smi --reset-gpu-clocks >nul 2>&1
    echo [INFO] GPU clock reset.
) else (
    echo [INFO] No clock lock to release.
)
echo(

echo ============================================================
echo   Done.
echo ============================================================

endlocal
exit /b %PROBE_EXIT%