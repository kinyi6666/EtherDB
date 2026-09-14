@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  EtherDB test build script (Windows / Visual Studio 2022)
REM
REM  Builds the standalone integration / performance tests in tests\ that
REM  link against the prebuilt client SDK (src\bin\sdk). No server sources
REM  are required.
REM
REM  Build the SDK first if src\bin\sdk\lib\etherdb_client.lib is missing:
REM      scripts\build_sdk.bat
REM
REM  Output:
REM      tests\bin_win\<name>.exe
REM
REM  IMPORTANT: /utf-8 is mandatory.
REM  The test sources contain CJK text and box-drawing characters. Without
REM  /utf-8 MSVC parses them as the local ANSI code page (e.g. 936), which
REM  makes it report bogus "C2065: undeclared identifier" errors and can even
REM  swallow line endings. Same reason for chcp 65001 when *running* them.
REM
REM  Usage:
REM    scripts\build_tests_win.bat                  build the default set
REM    scripts\build_tests_win.bat full_test        build only named test(s)
REM    scripts\build_tests_win.bat --clean          wipe tests\bin_win first
REM    scripts\build_tests_win.bat --list           show the default set
REM ============================================================

cd /d "%~dp0\.."

set OUTDIR=tests\bin_win
set CLEAN=0
set LISTONLY=0
set TARGETS=

:parse
if "%~1"=="" goto parsed
if /i "%~1"=="--clean" ( set CLEAN=1 & shift & goto parse )
if /i "%~1"=="--list"  ( set LISTONLY=1 & shift & goto parse )
if /i "%~1"=="--help"  goto usage
set TARGETS=%TARGETS% %~1
shift
goto parse
:parsed

REM ---- default set (the ones referenced by the perf write-up) ----
if "%TARGETS%"=="" set TARGETS=perf_insert_test perf_insert_test_string streaming_perf_test query_perf_test full_test

if %LISTONLY%==1 (
    echo Default test set:
    for %%T in (%TARGETS%) do echo   %%T
    endlocal
    exit /b 0
)

REM ---- locate the VS2022 C++ toolchain ----
set VCVARS=
set VSDIR=
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
    for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set VSDIR=%%i
)
if defined VSDIR if exist "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" set VCVARS=!VSDIR!\VC\Auxiliary\Build\vcvars64.bat

if not defined VCVARS (
    for %%P in (
        "d:\develop\Microsoft Visual Studio\2022\Community"
        "C:\Program Files\Microsoft Visual Studio\2022\Community"
        "C:\Program Files\Microsoft Visual Studio\2022\Professional"
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise"
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
    ) do (
        if not defined VCVARS if exist "%%~P\VC\Auxiliary\Build\vcvars64.bat" set VCVARS=%%~P\VC\Auxiliary\Build\vcvars64.bat
    )
)

if not defined VCVARS (
    echo [EtherDB] ERROR: could not find vcvars64.bat ^(Visual Studio 2022 C++ tools^).
    echo [EtherDB]        Install the "Desktop development with C++" workload,
    echo [EtherDB]        or edit the search list in this script.
    endlocal
    exit /b 1
)

REM ---- SDK must exist ----
if not exist "src\bin\sdk\lib\etherdb_client.lib" (
    echo [EtherDB] ERROR: client SDK not found: src\bin\sdk\lib\etherdb_client.lib
    echo [EtherDB]        Run:  scripts\build_sdk.bat
    endlocal
    exit /b 1
)

if %CLEAN%==1 (
    if exist "%OUTDIR%" rmdir /s /q "%OUTDIR%"
)
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

echo [EtherDB] =============================================
echo [EtherDB] Building tests   (Release / x64)
echo [EtherDB] Output: %OUTDIR%\
echo [EtherDB] Toolchain: %VCVARS%
echo [EtherDB] =============================================

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [EtherDB] ERROR: failed to initialise the MSVC environment.
    endlocal
    exit /b 1
)

set FAILED=
set BUILT=0

for %%T in (%TARGETS%) do (
    if not exist "tests\%%T.cpp" (
        echo [EtherDB] SKIP  %%T  ^(tests\%%T.cpp not found^)
        set FAILED=!FAILED! %%T
    ) else (
        echo [EtherDB] CC    %%T
        cl /nologo /utf-8 /std:c++17 /O2 /EHsc /MD ^
           /I src\bin\sdk\include /I src ^
           /Fo"%OUTDIR%\%%T.obj" /Fe"%OUTDIR%\%%T.exe" ^
           "tests\%%T.cpp" src\bin\sdk\lib\etherdb_client.lib ws2_32.lib
        if errorlevel 1 (
            set FAILED=!FAILED! %%T
        ) else (
            set /a BUILT+=1
        )
    )
)

echo.
echo [EtherDB] =============================================
if defined FAILED (
    echo [EtherDB] FAILED:!FAILED!
    echo [EtherDB] Built %BUILT% target^(s^), some failed.
    endlocal
    exit /b 1
)
echo [EtherDB] OK - %BUILT% test^(s^) built into %OUTDIR%\
echo [EtherDB] =============================================
echo [EtherDB] To run them, start the server first:
echo [EtherDB]   src\bin\Release\etherdb_dserver.exe -p 7040
echo [EtherDB] Then, e.g.:
echo [EtherDB]   %OUTDIR%\full_test.exe 7040
echo [EtherDB]   %OUTDIR%\perf_insert_test.exe 7040 100 perf_data_1
echo [EtherDB]   %OUTDIR%\streaming_perf_test.exe 7040 perf_data_1 perftest20
echo [EtherDB]   %OUTDIR%\query_perf_test.exe 7040
echo [EtherDB] Note: run inside a "chcp 65001" console so UTF-8 output is readable.
echo [EtherDB] Note: query_perf_test / streaming_perf_test default to table "perf_data_1",
echo [EtherDB]       which only exists after: perf_insert_test ^<port^> ^<rounds^> perf_data_1

endlocal
exit /b 0

:usage
echo EtherDB test build script
echo   scripts\build_tests_win.bat [--clean] [--list] [--help] [test names...]
echo Output goes to tests\bin_win\
endlocal
exit /b 0
