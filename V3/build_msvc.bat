@echo off
rem ============================================================
rem  Windows direct build script (no CMake required)
rem  Prereq: Visual Studio with "Desktop development with C++".
rem  If VCVARS below does not match your machine, edit it to your
rem  local vcvars64.bat path, or open "x64 Native Tools Command
rem  Prompt" first (it already ran vcvars64.bat) and run this
rem  script from there.
rem  NOTE: keep this file ASCII-only -- cmd.exe parses batch
rem  files in the OEM codepage; non-ASCII comments can corrupt
rem  the parsing (e.g. GBK trail bytes swallowing line feeds).
rem ============================================================
set "VCVARS=D:\programming\visual studio\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo [ERROR] vcvars64.bat not found: %VCVARS%
    echo Please edit build_msvc.bat and set VCVARS to your local path.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

rem Switch to the directory of this script (project root)
cd /d %~dp0
if not exist build_msvc mkdir build_msvc
cd build_msvc

echo === compiling unit_test ===
cl /nologo /EHsc /W4 /O2 /utf-8 /std:c++14 /I ..\include ^
    ..\src\ThreadCache.cpp ..\src\CentralCache.cpp ..\src\PageCache.cpp ^
    ..\tests\UnitTest.cpp /Fe:unit_test.exe
if errorlevel 1 exit /b 1

echo === compiling perf_test ===
cl /nologo /EHsc /W4 /O2 /utf-8 /std:c++14 /I ..\include ^
    ..\src\ThreadCache.cpp ..\src\CentralCache.cpp ..\src\PageCache.cpp ^
    ..\tests\PerformanceTest.cpp /Fe:perf_test.exe
if errorlevel 1 exit /b 1

echo === BUILD OK ===