@echo off
setlocal

:: Builds and runs the shared mesh key test (x86). Output goes to
:: build\test_shared_mesh_keys\, never into the wrapper or plugin DLLs.
::
:: Usage: tests\shared_mesh_keys\build_test.bat

set "HERE=%~dp0"
for %%G in ("%HERE%..\..") do set "ROOT=%%~fG\"
set "OUT=%ROOT%build\test_shared_mesh_keys"

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

cl /nologo /std:c++latest /W4 /EHsc /MT /O2 /I"%ROOT%src" /I"%ROOT%deps\xxhash" ^
    /Fo"%OUT%\\" /Fe"%OUT%\shared_mesh_keys_test.exe" ^
    "%HERE%shared_mesh_keys_test.cpp"
if errorlevel 1 goto :fail

"%OUT%\shared_mesh_keys_test.exe"
exit /b %errorlevel%

:fail
echo === test build FAILED ===
exit /b 1
