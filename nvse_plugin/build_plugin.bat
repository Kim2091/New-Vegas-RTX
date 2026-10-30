@echo off
setlocal enabledelayedexpansion

:: fnv_remix_nvse.dll build script (the xNVSE engine-monitor plugin, 32-bit)
:: Usage: build_plugin.bat [release|debug]
::
:: Output: ..\build\bin\<config>\fnv_remix_nvse.dll + .pdb + fnv_remix_nvse.ini
:: Deploy to <game>\Data\NVSE\Plugins\, always together with the d3d9.dll built
:: from the same commit.

set "PLUGIN=%~dp0"
for %%R in ("%PLUGIN%..") do set "ROOT=%%~fR\"
set "CONFIG=release"

:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="release" ( set "CONFIG=release" & shift & goto :parse_args )
if /i "%~1"=="debug"   ( set "CONFIG=debug"   & shift & goto :parse_args )
echo Unknown argument: %~1
exit /b 1
:args_done

:: Find Visual Studio (same order as build.bat)
set "VCVARS="
for %%p in (
    "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
) do (
    if exist %%p set "VCVARS=%%~p"
)
if "%VCVARS%"=="" (
    echo ERROR: Visual Studio 2022 not found
    exit /b 1
)
call "%VCVARS%" x86 >nul 2>&1
if errorlevel 1 ( echo ERROR: vcvarsall.bat failed & exit /b 1 )

:: The commit the DLL reports (the wrapper compares it in its link message)
set "COMMIT=unknown"
for /f %%h in ('git -C "%ROOT%." describe --always --dirty --abbrev^=7 --exclude^=* 2^>nul') do set "COMMIT=%%h"

set "OUT=%ROOT%build\bin\%CONFIG%"
set "OBJ=%ROOT%build\obj\%CONFIG%\nvse_plugin"
mkdir "%OUT%" 2>nul
mkdir "%OBJ%" 2>nul
del /q "%OBJ%\*.obj" 2>nul

:: Includes: the plugin's sources and src\ for shared\fnv_remix_plugin_api.h
set "INC=/I"%PLUGIN%src" /I"%ROOT%src""
:: Source paths in the binary are written relative to the wrapper folder.
set "TRIM=%ROOT:~0,-1%"
set "CF=/nologo /std:c++latest /W4 /WX /EHsc /MP /DWIN32 /D_WINDOWS /DFNVR_COMMIT=%COMMIT% /d1trimfile:"%TRIM%""
if /i "%CONFIG%"=="release" (
    set "CF=%CF% /O2 /MT /Zi /DNDEBUG"
) else (
    set "CF=%CF% /Od /MTd /Zi /D_DEBUG"
)

echo.
echo === fnv_remix_nvse build (xNVSE plugin) ===
echo Config:  %CONFIG%
echo Commit:  %COMMIT%
echo Output:  %OUT%\fnv_remix_nvse.dll
echo.

set "SRCS="
for %%f in ("%PLUGIN%src\*.cpp") do set "SRCS=!SRCS! "%%f""

cl /c %CF% %INC% /Fo"%OBJ%\\" /Fd"%OBJ%\vc.pdb" %SRCS%
if errorlevel 1 goto :fail

link /nologo /DLL /SUBSYSTEM:WINDOWS /DEBUG /PDBALTPATH:%%_PDB%% /OPT:REF /OPT:ICF ^
    /DEF:"%PLUGIN%exports.def" ^
    /OUT:"%OUT%\fnv_remix_nvse.dll" ^
    /PDB:"%OUT%\fnv_remix_nvse.pdb" ^
    "%OBJ%\*.obj" ^
    psapi.lib user32.lib kernel32.lib
if errorlevel 1 goto :fail

copy /y "%PLUGIN%fnv_remix_nvse.ini" "%OUT%\fnv_remix_nvse.ini" >nul

echo.
echo === Build succeeded: %OUT%\fnv_remix_nvse.dll ===
exit /b 0

:fail
echo.
echo === Build FAILED ===
exit /b 1
