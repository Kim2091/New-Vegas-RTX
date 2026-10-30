@echo off
setlocal

:: Builds and runs the call_chain test (x86). Output goes to build\test\,
:: never into the wrapper or plugin DLLs.
::
:: Usage: tests\hook_chain\build_test.bat

set "HERE=%~dp0"
for %%G in ("%HERE%..\..") do set "ROOT=%%~fG\"
set "SRC=%ROOT%src"
set "MH=%ROOT%deps\minhook"
set "OUT=%ROOT%build\test"

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

cl /nologo /c /W0 /O2 /MT /Fo"%OUT%\\" "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde32.c"
if errorlevel 1 goto :fail

:: The test's std_include.hpp stands in for the wrapper's precompiled header.
cl /nologo /std:c++latest /W4 /WX /EHsc /MT /O2 /I"%HERE%." /I"%SRC%" /I"%MH%\include" ^
    /Fo"%OUT%\\" /Fe"%OUT%\hook_chain_test.exe" ^
    "%HERE%hook_chain_test.cpp" "%SRC%\comp\game\call_chain.cpp" ^
    "%OUT%\buffer.obj" "%OUT%\hook.obj" "%OUT%\trampoline.obj" "%OUT%\hde32.obj"
if errorlevel 1 goto :fail

"%OUT%\hook_chain_test.exe"
exit /b %errorlevel%

:fail
echo === test build FAILED ===
exit /b 1
