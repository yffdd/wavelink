@echo off
setlocal
:: Build the Windows BLE helper with a separate MSVC cache. The main Qt/MinGW
:: application does not need to change its compiler or link against this EXE.
:: Usage: Build_BLE_Host.bat [Debug^|Release] [optional destination directory]
::        [optional independent build directory]
:: Release scripts always pass argument 3 to keep their MSVC cache isolated.
:: Leaving it empty preserves the standalone IDE/debug workflow.
:: Prerequisites: CMake, Visual Studio 2022 C++ tools and Windows 10/11 SDK.
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
set "BLE_SOURCE=%~dp0..\WaveLink\src\platform\windows\blehelper"
set "BLE_BUILD=%~dp0..\build\ble-helper-msvc-%BLE_FOLDER%"
if not "%~3"=="" set "BLE_BUILD=%~3"
if not defined CMAKE set "CMAKE=cmake.exe"
echo [BLE] Configuring the Windows BLE host (%BLE_CONFIG%)...
%CMAKE% -S "%BLE_SOURCE%" -B "%BLE_BUILD%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
%CMAKE% --build "%BLE_BUILD%" --config "%BLE_CONFIG%" --parallel 2
if errorlevel 1 exit /b 1
if not exist "%BLE_BUILD%\%BLE_CONFIG%\WaveLinkBleHost.exe" (
    echo [ERROR] The BLE host executable was not generated.
    exit /b 1
)
if not "%~2"=="" (
    if not exist "%~2" mkdir "%~2"
    copy /y "%BLE_BUILD%\%BLE_CONFIG%\WaveLinkBleHost.exe" "%~2\WaveLinkBleHost.exe" > nul
    if errorlevel 1 exit /b 1
)
echo [BLE] Windows BLE host built successfully.
exit /b 0
