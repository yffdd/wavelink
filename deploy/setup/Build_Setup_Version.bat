@echo off
chcp 65001 > nul
setlocal DisableDelayedExpansion

:: ================================================================
:: Configuration: only installed tool paths are machine-specific.
:: All generated files stay inside this script's setup directory.
:: Set ENABLE_BLE to 0 for a release without the native BLE helper.
:: BLE builds require Visual Studio 2022 C++ tools and a Windows SDK.
:: ================================================================
set "ENABLE_BLE=1"
set "QT_BIN=E:\software\Qt\Qt5.14.2\5.14.2\mingw73_64\bin"
set "MINGW_BIN=E:\software\Qt\Qt5.14.2\Tools\mingw730_64\bin"
set CMAKE="C:\Program Files\CMake\bin\cmake.exe"
set INNO_BIN="C:\Program Files\Inno Setup 7\ISCC.exe"

:: Leave empty for the CMake version, or enter e.g. 0.10 / 0.1.1 here.
set "APP_VERSION_OVERRIDE="

:: The source directory is READ ONLY throughout publishing. In particular,
:: resources/source.zip and the root build directory are never modified.
set "WAVELINK_PUBLISH_DIR=%~dp0"
set "SOURCE=%~dp0..\..\WaveLink"
set "BUILD=%~dp0build-WaveLink-Release"
set "RELDIR=%~dp0WaveLink_Release"
set "WORK=%~dp0_Setup_Work"
set "ISS_SCRIPT=%~dp0wavelink.iss"
set "BLE_BUILD=%WORK%\ble-helper-msvc-release"
set "BLE_HOST=%BLE_BUILD%\Release\WaveLinkBleHost.exe"
set "SOURCE_ZIP=%WORK%\source.zip"
set "PATH=%QT_BIN%;%MINGW_BIN%;%PATH%"
:: Even preflight/cleanup child processes use an existing local TEMP folder.
set "TEMP=%WAVELINK_PUBLISH_DIR%"
set "TMP=%TEMP%"

if not exist "%SOURCE%\CMakeLists.txt" ( echo [ERROR] Source project was not found. & goto FAILED )
if not exist "%ISS_SCRIPT%" ( echo [ERROR] Inno Setup script was not found. & goto FAILED )
if not exist "%SOURCE%\resources\app.ico" ( echo [ERROR] Setup icon was not found. & goto FAILED )
if not exist %INNO_BIN% ( echo [ERROR] Inno Setup compiler was not found. & goto FAILED )
where tar.exe > nul 2>&1
if errorlevel 1 ( echo [ERROR] tar.exe is required but was not found. & goto FAILED )

:: Read and validate the version before changing any working directory.
set "APP_VERSION="
if defined APP_VERSION_OVERRIDE set "APP_VERSION=%APP_VERSION_OVERRIDE%"
if not defined APP_VERSION for /f "tokens=2" %%V in ('findstr /r /c:"^set(WAVELINK_DEFAULT_VERSION " "%SOURCE%\CMakeLists.txt"') do set "APP_VERSION=%%V"
set "APP_VERSION=%APP_VERSION:)=%"
powershell.exe -NoProfile -Command "if($env:APP_VERSION -match '^\d+(\.\d+){0,3}$') { exit 0 }; Write-Output '[ERROR] Invalid or missing application version.'; exit 1"
if errorlevel 1 goto FAILED
set "SETUP_FILENAME=WaveLink_v%APP_VERSION%_Windows_x64_Setup"

echo ========================================
echo    WaveLink Isolated Setup Build
echo    Version: %APP_VERSION%
echo    Working root: "%WAVELINK_PUBLISH_DIR%"
echo ========================================

:: Prepare/check the public update channel BEFORE building. Reuse the existing
:: signing key; never regenerate or embed it. No hosted files are uploaded.
call :PREPARE_UPDATE_PUBLISHING
if errorlevel 1 ( echo [ERROR] Update publishing preflight failed. Existing build files were not cleaned. & goto FAILED )

:: Reset only the THREE exact child directories owned by this workflow.
:: The validation rejects junctions, symbolic links and unexpected targets
:: before deleting anything. No external cleanup PS1 file is required.
call :CLEAN_WORK_DIRECTORIES
if errorlevel 1 goto FAILED
mkdir "%BUILD%" "%RELDIR%" "%WORK%\Temp"
if errorlevel 1 ( echo [ERROR] Could not create local working directories. & goto FAILED )

:: Redirect compiler, MSVC and packager temporary files to our own folder.
:: SETLOCAL restores the caller's TEMP/TMP/PATH when this script returns.
set "TEMP=%WORK%\Temp"
set "TMP=%TEMP%"

echo.
echo [1/7] Creating the local embedded source archive...
:: Exclude the previous embedded archive, without deleting it. The new ZIP
:: sits outside WaveLink, so it cannot include itself or another build cache.
tar.exe -a -c -f "%SOURCE_ZIP%" --exclude="WaveLink/resources/source.zip" -C "%SOURCE%\.." WaveLink
if errorlevel 1 ( echo [ERROR] Source archive creation failed. & goto FAILED )
if not exist "%SOURCE_ZIP%" ( echo [ERROR] Source archive was not created. & goto FAILED )
echo Source archive: "%SOURCE_ZIP%"

echo.
echo [2/7] Building BLE and configuring the isolated Release cache...
if "%ENABLE_BLE%"=="1" (
    call "%~dp0..\Build_BLE_Host.bat" Release "" "%BLE_BUILD%"
    if errorlevel 1 ( echo [ERROR] BLE host build failed. & goto FAILED )
    if not exist "%BLE_HOST%" ( echo [ERROR] Local BLE host was not found. & goto FAILED )
)
:: Explicit archive and BLE paths prevent fallback to the source archive or
:: any helper in the root Debug/Release build caches.
%CMAKE% -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_BIN%\.." "-DWAVELINK_VERSION_OVERRIDE=%APP_VERSION%" "-DWAVELINK_ENABLE_BLE=%ENABLE_BLE%" "-DWAVELINK_SOURCE_ARCHIVE:FILEPATH=%SOURCE_ZIP%" "-DWAVELINK_BLE_HOST_PATH:FILEPATH=%BLE_HOST%" -S "%SOURCE%" -B "%BUILD%"
if errorlevel 1 ( echo [ERROR] CMake configure failed. & goto FAILED )

echo.
echo [3/7] Building the application...
%CMAKE% --build "%BUILD%" --parallel 2
if errorlevel 1 ( echo [ERROR] Application build failed. & goto FAILED )
if not exist "%BUILD%\WaveLink.exe" ( echo [ERROR] Application EXE was not found. & goto FAILED )

echo.
echo [4/7] Collecting Qt dependencies into the local staging folder...
copy /y "%BUILD%\WaveLink.exe" "%RELDIR%\" > nul
if errorlevel 1 ( echo [ERROR] Application deployment failed. & goto FAILED )
copy /y "%BUILD%\WaveLinkUpdateLauncher.exe" "%RELDIR%\" > nul
if errorlevel 1 ( echo [ERROR] Update launcher deployment failed. & goto FAILED )
if "%ENABLE_BLE%"=="1" (
    copy /y "%BUILD%\WaveLinkBleHost.exe" "%RELDIR%\" > nul
    if errorlevel 1 ( echo [ERROR] BLE host deployment failed. & goto FAILED )
)
"%QT_BIN%\windeployqt.exe" --no-translations --dir "%RELDIR%" --libdir "%RELDIR%" --plugindir "%RELDIR%" "%RELDIR%\WaveLink.exe"
if errorlevel 1 ( echo [ERROR] windeployqt failed. & goto FAILED )

echo.
echo [5/7] Keeping all Qt runtime and graphics fallback dependencies...
:: Keep windeployqt's dependencies for machines without usable native OpenGL.

echo.
echo [6/7] Skipping UPX. Inno Setup will compress the installer.

echo.
echo [7/7] Generating the installer in separate local attempt folders...
:: Retry packaging only. Each attempt gets its own output AND TEMP folder,
:: so a failed/locked intermediate EXE is not reused by the next attempt.
:: The previous published installer remains untouched until ISCC succeeds.
set "INNO_MAX_ATTEMPTS=4"
for /l %%A in (1,1,%INNO_MAX_ATTEMPTS%) do (
    call :PACKAGE_ATTEMPT %%A
    if not errorlevel 1 goto INNO_SETUP_SUCCEEDED
    if %%A LSS %INNO_MAX_ATTEMPTS% (
        echo [WARNING] Packaging failed. Retrying in 5 seconds with new local temporary files.
        timeout /t 5 /nobreak > nul
    )
)
echo [ERROR] Inno Setup failed after %INNO_MAX_ATTEMPTS% attempts.
goto FAILED

:INNO_SETUP_SUCCEEDED
:: Publish only the successfully compiled file. Refuse a linked destination;
:: if the old installer is locked, keep the new one for manual recovery.
set "SETUP_FINAL=%~dp0%SETUP_FILENAME%.exe"
powershell.exe -NoProfile -Command "$ErrorActionPreference='Stop'; try { $root=[IO.Path]::GetFullPath($env:WAVELINK_PUBLISH_DIR); $target=[IO.Path]::GetFullPath($env:SETUP_FINAL); if($target -ine (Join-Path $root ($env:SETUP_FILENAME+'.exe'))) { throw 'Unsafe installer destination.' }; if(Test-Path -LiteralPath $target) { $item=Get-Item -LiteralPath $target -Force; if($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Installer destination is not a regular file.' } }; if(-not (Test-Path -LiteralPath $env:SETUP_COMPILED -PathType Leaf)) { throw 'Compiled installer was not found.' }; Move-Item -LiteralPath $env:SETUP_COMPILED -Destination $target -Force; exit 0 } catch { Write-Error $_.Exception.Message -ErrorAction Continue; exit 1 }"
if errorlevel 1 ( echo [ERROR] Could not publish the compiled installer. & goto FAILED )
echo Installer generated successfully: "%SETUP_FINAL%"
:: Optional signed update feed generation. No address/key is invented, and no
:: upload is performed. The private key must live OUTSIDE this source tree.
if exist "%~dp0..\update\publish.local.json" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\update\Publish-Update.ps1" -Config "%~dp0..\update\publish.local.json" -Installer "%SETUP_FINAL%" -Version "%APP_VERSION%"
    if errorlevel 1 ( echo [ERROR] Installer exists, but update manifest signing failed. Do not publish a stale latest.json. & goto FAILED )
)

echo.
echo [Cleanup] Removing setup-owned build, staging and temporary files...
:: Stop using the local TEMP directory before deleting it. No further build
:: commands run after this point; SETLOCAL restores the original environment.
set "TEMP=%WAVELINK_PUBLISH_DIR%"
set "TMP=%TEMP%"
call :CLEAN_WORK_DIRECTORIES
if errorlevel 1 (
    echo [WARNING] The installer was generated, but local cleanup failed.
    goto FAILED
)
echo Done. The source project and all other build directories were left unchanged.
echo.
if not defined WAVELINK_NO_PAUSE pause
exit /b 0

:PREPARE_UPDATE_PUBLISHING
:: Without keys or local publishing settings, offline packaging still works.
if not exist "%~dp0..\update\publish.local.json" if not exist "%USERPROFILE%\.wavelink-release\update-signing.key" if not exist "%USERPROFILE%\.wavelink-release\update-public.txt" (
    echo [INFO] No update signing key configured. Building an offline installer only.
    exit /b 0
)
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\update\Initialize-UpdatePublishing.ps1"
exit /b %ERRORLEVEL%

:PACKAGE_ATTEMPT
set "INNO_ATTEMPT_DIR=%WORK%\Inno-Attempt-%~1"
set "INNO_LOG=%WORK%\Inno-Attempt-%~1.log"
mkdir "%INNO_ATTEMPT_DIR%\Temp"
if errorlevel 1 exit /b 1
set "TEMP=%INNO_ATTEMPT_DIR%\Temp"
set "TMP=%TEMP%"
echo Inno Setup attempt %~1 of %INNO_MAX_ATTEMPTS%...
%INNO_BIN% /DMyAppVersion=%APP_VERSION% "/O%INNO_ATTEMPT_DIR%" "/F%SETUP_FILENAME%" "%ISS_SCRIPT%" > "%INNO_LOG%" 2>&1
set "INNO_RESULT=%ERRORLEVEL%"
type "%INNO_LOG%"
set "SETUP_COMPILED=%INNO_ATTEMPT_DIR%\%SETUP_FILENAME%.exe"
if not "%INNO_RESULT%"=="0" exit /b 1
if not exist "%SETUP_COMPILED%" exit /b 1
exit /b 0

:CLEAN_WORK_DIRECTORIES
:: Validate all targets before deleting any of them. The final installer,
:: deploy/portable, root build and source files are never cleanup targets.
powershell.exe -NoProfile -Command "$ErrorActionPreference='Stop'; try { $root=[IO.Path]::GetFullPath($env:WAVELINK_PUBLISH_DIR).TrimEnd('\'); $rootItem=Get-Item -LiteralPath $root -Force; if(-not $rootItem.PSIsContainer -or ($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Publishing root is not a regular directory.' }; $targets=@($env:BUILD,$env:RELDIR,$env:WORK); $names=@('build-WaveLink-Release','WaveLink_Release','_Setup_Work'); for($i=0; $i -lt $targets.Count; $i++) { $actual=[IO.Path]::GetFullPath($targets[$i]).TrimEnd('\'); if($actual -ine (Join-Path $root $names[$i])) { throw ('Unsafe cleanup target: '+$actual) }; $targets[$i]=$actual; if(Test-Path -LiteralPath $actual) { $item=Get-Item -LiteralPath $actual -Force; if(-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw ('Not a regular directory: '+$actual) }; $link=Get-ChildItem -LiteralPath $actual -Force -Recurse | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint } | Select-Object -First 1; if($link) { throw ('Link/junction found: '+$link.FullName) } } }; foreach($target in $targets) { if(Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force; Write-Output ('Removed local working directory: '+$target) } }; exit 0 } catch { Write-Error ('Safe cleanup failed: '+$_.Exception.Message) -ErrorAction Continue; exit 1 }"
exit /b %ERRORLEVEL%

:FAILED
echo [ERROR] Publishing stopped. Existing local working files and logs were retained.
echo Local work folder: "%WORK%"
if not defined WAVELINK_NO_PAUSE pause
exit /b 1
