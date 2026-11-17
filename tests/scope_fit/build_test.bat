@echo off
setlocal

:: Builds and runs the scope fit test (x86): synthetic meshes, then, with
:: arguments, the comparison against lens files over mesh soups (see
:: scope_fit_test.cpp). Output goes to build\test_scope_fit\, never into the
:: wrapper or plugin DLLs.
::
:: Usage: tests\scope_fit\build_test.bat [--soups <dir> --lenses <dir>]

set "HERE=%~dp0"
for %%G in ("%HERE%..\..") do set "ROOT=%%~fG\"
set "SRC=%ROOT%src"
set "OUT=%ROOT%build\test_scope_fit"

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
    /Fo"%OUT%\\" /Fe"%OUT%\scope_fit_test.exe" ^
    "%HERE%scope_fit_test.cpp" "%SRC%\comp\game\scope_fit.cpp"
if errorlevel 1 goto :fail

"%OUT%\scope_fit_test.exe" %*
exit /b %errorlevel%

:fail
echo === test build FAILED ===
exit /b 1
