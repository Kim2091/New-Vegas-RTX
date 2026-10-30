@echo off
setlocal

:: Builds and runs the sampler test (x86). Output goes to build\test_sampler\,
:: never into the wrapper or plugin DLLs.
::
:: Usage: tests\sampler\build_test.bat

set "HERE=%~dp0"
for %%G in ("%HERE%..\..") do set "ROOT=%%~fG\"
set "SRC=%ROOT%src"
set "OUT=%ROOT%build\test_sampler"

set "VCVARS="
for %%p in (
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%p set "VCVARS=%%~p"
)
if "%VCVARS%"=="" ( echo ERROR: Visual Studio 2022 not found & exit /b 1 )
call "%VCVARS%" x86 >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvarsall.bat failed & exit /b 1 )

mkdir "%OUT%" 2>nul

:: The test's std_include.hpp stands in for the wrapper's precompiled header.
cl /nologo /std:c++latest /W4 /WX /EHsc /MT /O2 /I"%HERE%." /I"%SRC%" ^
    /Fo"%OUT%\\" /Fe"%OUT%\sampler_test.exe" ^
    "%HERE%sampler_test.cpp" "%SRC%\comp\game\sampler.cpp" psapi.lib
if errorlevel 1 goto :fail

"%OUT%\sampler_test.exe"
exit /b %errorlevel%

:fail
echo === test build FAILED ===
exit /b 1
