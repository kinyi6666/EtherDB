@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  EtherDB server build script (Windows / Visual Studio 2022)
REM
REM  Builds etherdb_dserver.exe with the local VS2022 C++ toolchain.
REM
REM  The server compiles the src\base sources directly into the executable,
REM  so no prebuilt base library (base.lib / base.dll) is required.
REM
REM  Output:
REM    src\bin\Release\etherdb_dserver.exe   (self-contained)
REM
REM  The server defaults to reading etherdb.cfg and creating
REM  etherdb_log / etherdb_data in the same directory as the exe,
REM  so you can just copy the exe + optional etherdb.cfg
REM  to any folder and run it.
REM
REM  Usage:
REM    scripts\build_dserver_windows.bat                build (Release)
REM    scripts\build_dserver_windows.bat --config Debug build given config
REM    scripts\build_dserver_windows.bat --clean        clean and rebuild
REM ============================================================

cd /d "%~dp0\.."

set CONFIG=Release
set CLEAN=0

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--config" ( set CONFIG=%~2 & shift & shift & goto parse )
if /i "%~1"=="--debug"  ( set CONFIG=Debug & shift & goto parse )
if /i "%~1"=="--clean"  ( set CLEAN=1 & shift & goto parse )
echo Unknown argument: %~1
exit /b 1
:parsed

set DSERVER_BUILD=build_win\dserver

if %CLEAN%==1 (
    if exist "%DSERVER_BUILD%" rmdir /s /q "%DSERVER_BUILD%"
)

echo [EtherDB] =============================================
echo [EtherDB] Building DB server   (config=%CONFIG%)
echo [EtherDB] =============================================
cmake -S src\dserver -B %DSERVER_BUILD% -G "Visual Studio 17 2022" -A x64
if errorlevel 1 exit /b 1
cmake --build %DSERVER_BUILD% --config %CONFIG%
if errorlevel 1 exit /b 1

echo [EtherDB] Done.
if exist "src\bin\%CONFIG%\etherdb_dserver.exe" (
    echo   Server:  src\bin\%CONFIG%\etherdb_dserver.exe
    echo   The exe is self-contained - no base.dll is required at runtime.
    echo   Logs and data are created next to the exe; etherdb.cfg is optional.
) else (
    echo   Build FAILED - no etherdb_dserver.exe produced.
)

endlocal
exit /b 0
