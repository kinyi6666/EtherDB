@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  EtherDB Client SDK build script (Windows / Visual Studio 2022)
REM
REM  NEW build (additive): builds the etherdb_client library only.
REM  The original src\client\CMakeLists.txt (shell) is NOT touched.
REM
REM  Output: src\bin\sdk\   (include\ + lib\ + README.md)
REM
REM  Usage:
REM    scripts\build_sdk.bat                build static library (default)
REM    scripts\build_sdk.bat --shared       build shared library (dll)
REM    scripts\build_sdk.bat --clean        clean build dir then rebuild
REM    scripts\build_sdk.bat --config Debug build with given config
REM    scripts\build_sdk.bat --debug        shortcut for --config Debug
REM ============================================================

cd /d "%~dp0\.."

set CONFIG=Release
set SHARED=OFF
set CLEAN=0

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--shared"  ( set SHARED=ON   & shift & goto parse )
if /i "%~1"=="--clean"   ( set CLEAN=1     & shift & goto parse )
if /i "%~1"=="--config"  ( set CONFIG=%~2  & shift & shift & goto parse )
if /i "%~1"=="--debug"   ( set CONFIG=Debug & shift & goto parse )
echo Unknown argument: %~1
exit /b 1
:parsed

set BUILD_DIR=build_win_sdk
if %CLEAN%==1 (
    if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
)

echo [EtherDB SDK] Configure: config=%CONFIG% shared=%SHARED% build_dir=%BUILD_DIR%
cmake -S src\client\sdk -B %BUILD_DIR% -G "Visual Studio 17 2022" -A x64 -DETHERDB_BUILD_SHARED=%SHARED%
if errorlevel 1 exit /b 1

echo [EtherDB SDK] Build target: etherdb_client (%CONFIG%)
cmake --build %BUILD_DIR% --config %CONFIG% --target etherdb_client
if errorlevel 1 exit /b 1

echo [EtherDB SDK] Done. SDK at src\bin\sdk
if exist src\bin\sdk (
    dir /b src\bin\sdk
    echo   include\ - C/C++ interface headers
    echo   lib\     - library files
)
endlocal
exit /b 0
