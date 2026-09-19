@echo off
call "D:\programming\visual studio\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

cd /d D:\code\kamacode\memory-pool\v2
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