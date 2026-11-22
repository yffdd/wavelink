@echo off
setlocal
:: Build the Windows BLE helper with a separate MSVC cache. The main Qt/MinGW
:: application does not need to change its compiler or link against this EXE.
:: Usage: Build_BLE_Host.bat [Debug^|Release] [optional destination directory]
::        [optional independent build directory]
:: Release scripts always pass argument 3 to keep their MSVC cache isolated.
:: Leaving it empty preserves the standalone IDE/debug workflow.
:: Uses MSVC when available; otherwise requires a source-matched helper cache.
set "BLE_CONFIG=%~1"
if not defined BLE_CONFIG set "BLE_CONFIG=Debug"
if /i "%BLE_CONFIG%"=="Debug" (
    set "BLE_FOLDER=debug"
) else if /i "%BLE_CONFIG%"=="Release" (
    set "BLE_FOLDER=release"
) else (
    echo [ERROR] Configuration must be Debug or Release.
    exit /b 1
)
set "BLE_BUILD=%~dp0..\build\ble-helper-msvc-%BLE_FOLDER%"
if not "%~3"=="" set "BLE_BUILD=%~3"
if "%~2"=="" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-BleHost.ps1" -Configuration "%BLE_CONFIG%" -BuildDirectory "%BLE_BUILD%"
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build-BleHost.ps1" -Configuration "%BLE_CONFIG%" -BuildDirectory "%BLE_BUILD%" -Destination "%~2"
)
exit /b %ERRORLEVEL%
