@echo off
setlocal enabledelayedexpansion

:: Builds the wrapper (d3d9.dll proxy) and the session reporter (NewVegasRTX-Reporter.exe).
:: Usage: build.bat [release|debug] [--name PdbName] [--comp CompDir] [--release-tag Version] [--tracy]
::
::   build.bat                          release d3d9.dll
::   build.bat debug                    debug d3d9.dll
::   build.bat --release-tag 1.0.3      release build that reports "release 1.0.3" in its log
::   build.bat --name FNV               PDB named FNV-comp.pdb
::   build.bat --comp <dir>             build the comp sources from another folder; output goes
::                                      to <dir>\..\..\build\<config>
::   build.bat --tracy                  link the Tracy profiler client

set "ROOT=%~dp0"
set "CONFIG=release"
set "NAME=remix-comp"
set "COMP_DIR=%ROOT%src\comp"
set "CUSTOM_COMP=0"
set "TRACY=0"
set "RELEASE_TAG=dev"

:: Parse args
:parse_args
if "%~1"=="" goto :args_done
if /i "%~1"=="release" ( set "CONFIG=release" & shift & goto :parse_args )
if /i "%~1"=="debug"   ( set "CONFIG=debug"   & shift & goto :parse_args )
if /i "%~1"=="--name"  ( set "NAME=%~2-comp"  & shift & shift & goto :parse_args )
if /i "%~1"=="--comp"  ( set "COMP_DIR=%~2" & set "CUSTOM_COMP=1" & shift & shift & goto :parse_args )
if /i "%~1"=="--tracy" ( set "TRACY=1" & shift & goto :parse_args )
if /i "%~1"=="--release-tag" ( set "RELEASE_TAG=%~2" & shift & shift & goto :parse_args )
echo Unknown argument: %~1
exit /b 1
:args_done

:: DirectX SDK (June 2010) for the d3dx9 headers and import libraries. Its
:: installer sets DXSDK_DIR; it is not part of this repository.
if not defined DXSDK_DIR goto :no_dxsdk
set "DXSDK=!DXSDK_DIR!"
if "!DXSDK:~-1!"=="\" set "DXSDK=!DXSDK:~0,-1!"
if not exist "!DXSDK!\Include\d3dx9.h" goto :no_dxsdk
if not exist "!DXSDK!\Lib\x86\d3dx9.lib" goto :no_dxsdk

:: Find Visual Studio
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

:: The commit this build reports. The wrapper compares it with the plugin's at run time.
set "COMMIT=unknown"
for /f %%h in ('git -C "%ROOT%." describe --always --dirty --abbrev^=7 --exclude^=* 2^>nul') do set "COMMIT=%%h"

:: Source paths in the binary (__FILE__, assert strings) are written relative to this folder.
set "TRIM=%ROOT:~0,-1%"

:: Paths
set "SRC=%ROOT%src"
set "DEPS=%ROOT%deps"
set "OUT=%ROOT%build\bin\%CONFIG%"
set "OBJ=%ROOT%build\obj\%CONFIG%"

:: With --comp the output goes to <comp>\..\..\build\<config>.
if "!CUSTOM_COMP!"=="1" (
    for %%G in ("!COMP_DIR!\..\..") do set "GAME_DIR=%%~fG"
    set "GAME_OUT=!GAME_DIR!\build\!CONFIG!"
    set "GAME_OBJ=!GAME_DIR!\build\obj\!CONFIG!"
) else (
    set "GAME_OUT=!OUT!"
    set "GAME_OBJ=!OBJ!\comp"
)

mkdir "%OUT%" 2>nul
mkdir "%OBJ%\minhook" 2>nul
mkdir "%OBJ%\miniz" 2>nul
mkdir "%OBJ%\imgui" 2>nul
mkdir "%OBJ%\shared" 2>nul
mkdir "%GAME_OUT%" 2>nul
mkdir "%GAME_OBJ%" 2>nul

:: Include paths
set "INC=/I"%SRC%" /I"%DEPS%\bridge_api" /I"%DXSDK%\Include" /I"%DEPS%\imgui" /I"%DEPS%\minhook\include" /I"%DEPS%\xxhash" /I"%DEPS%\miniz""
if "%TRACY%"=="1" set "INC=%INC% /I"%DEPS%\tracy""

:: Lib search path
set "LIBPATH=/LIBPATH:"%DXSDK%\Lib\x86""

:: Common compiler flags
set "CF=/nologo /std:c++latest /W4 /MP /EHsc /Zm100"
set "CF=%CF% /wd4239 /wd4369 /wd4505 /wd4996 /wd5311 /wd6001 /wd6385 /wd6386 /wd26812"
set "CF=%CF% /D_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS /D_WINDOWS /DWIN32 /DFNVR_COMMIT=%COMMIT% /DFNVR_RELEASE=%RELEASE_TAG% /d1trimfile:"%TRIM%""

:: Config-specific flags
if /i "%CONFIG%"=="release" (
    set "CF=%CF% /O2 /GL /Gy /Oi /GF /MT /Zi /DNDEBUG /WX"
    set "LF=/LTCG /OPT:REF /OPT:ICF"
) else (
    set "CF=%CF% /Od /MTd /ZI /DDEBUG /D_DEBUG"
    set "LF="
)

:: Tracy defines (added to every TU when --tracy)
if "%TRACY%"=="1" (
    set "CF=%CF% /DTRACY_ENABLE /DTRACY_ON_DEMAND"
)

echo.
echo === remix-comp build (d3d9.dll proxy) ===
echo Config:  %CONFIG%
echo DXSDK:   %DXSDK%
echo Commit:  %COMMIT%
echo Release: %RELEASE_TAG%
echo Output:  %GAME_OUT%\d3d9.dll
echo CompDir: %COMP_DIR%
echo Tracy:   %TRACY%
echo.

:: -------------------------------------------------------
:: Step 0: tracy (C++ static lib, only when --tracy)
:: -------------------------------------------------------
if "%TRACY%"=="1" (
    echo [0/4] tracy
    mkdir "%OBJ%\tracy" 2>nul
    if /i "%CONFIG%"=="release" (
        cl /nologo /c /W0 /MP /MT /O2 /EHsc ^
            /DTRACY_ENABLE /DTRACY_ON_DEMAND ^
            /I"%DEPS%\tracy" ^
            /Fo"%OBJ%\tracy\\" ^
            "%DEPS%\tracy\TracyClient.cpp"
        if errorlevel 1 goto :fail
    ) else (
        cl /nologo /c /W0 /MP /MTd /Od /EHsc /Zi ^
            /DTRACY_ENABLE /DTRACY_ON_DEMAND ^
            /I"%DEPS%\tracy" ^
            /Fo"%OBJ%\tracy\\" ^
            "%DEPS%\tracy\TracyClient.cpp"
        if errorlevel 1 goto :fail
    )
    lib /nologo /OUT:"%OUT%\tracy.lib" "%OBJ%\tracy\*.obj"
    if errorlevel 1 goto :fail
)

:: -------------------------------------------------------
:: Step 1: minhook (C static lib)
:: -------------------------------------------------------
echo [1/6] minhook
set "MH_SRC=%DEPS%\minhook\src"
cl /nologo /c /W0 /MP /MT /O2 /d1trimfile:"%TRIM%" ^
    /I"%DEPS%\minhook\include" ^
    /Fo"%OBJ%\minhook\\" ^
    "%MH_SRC%\buffer.c" ^
    "%MH_SRC%\hook.c" ^
    "%MH_SRC%\trampoline.c" ^
    "%MH_SRC%\hde\hde32.c" ^
    "%MH_SRC%\hde\hde64.c"
if errorlevel 1 goto :fail
lib /nologo /OUT:"%OUT%\minhook.lib" "%OBJ%\minhook\*.obj"
if errorlevel 1 goto :fail

:: -------------------------------------------------------
:: Step 2: miniz (C static lib: the session zips' deflate)
:: -------------------------------------------------------
:: src\comp\game\zip_store.cpp includes miniz.h with the same defines.
echo [2/6] miniz
set "MZ_CRT=/MT /O2"
if /i not "%CONFIG%"=="release" set "MZ_CRT=/MTd /Od"
cl /nologo /c /W0 %MZ_CRT% /d1trimfile:"%TRIM%" ^
    /DMINIZ_NO_STDIO /DMINIZ_NO_TIME /DMINIZ_NO_ARCHIVE_APIS /DMINIZ_NO_ZLIB_COMPATIBLE_NAMES ^
    /Fo"%OBJ%\miniz\\" ^
    "%DEPS%\miniz\miniz.c"
if errorlevel 1 goto :fail
lib /nologo /OUT:"%OUT%\miniz.lib" "%OBJ%\miniz\*.obj"
if errorlevel 1 goto :fail

:: -------------------------------------------------------
:: Step 3: imgui (C++ static lib, no PCH)
:: -------------------------------------------------------
echo [3/6] imgui
set "IM=%DEPS%\imgui"
cl /nologo /c /W0 /MP /std:c++latest /MT /O2 /EHsc /d1trimfile:"%TRIM%" ^
    /I"%IM%" ^
    /Fo"%OBJ%\imgui\\" ^
    "%IM%\imgui.cpp" ^
    "%IM%\imgui_demo.cpp" ^
    "%IM%\imgui_draw.cpp" ^
    "%IM%\imgui_tables.cpp" ^
    "%IM%\imgui_widgets.cpp" ^
    "%IM%\backends\imgui_impl_dx9.cpp" ^
    "%IM%\backends\imgui_impl_win32.cpp" ^
    "%IM%\misc\cpp\imgui_stdlib.cpp"
if errorlevel 1 goto :fail
lib /nologo /OUT:"%OUT%\imgui.lib" "%OBJ%\imgui\*.obj"
if errorlevel 1 goto :fail

:: -------------------------------------------------------
:: Step 4: _shared (C++ static lib with PCH)
:: -------------------------------------------------------
echo [4/6] _shared

:: Create PCH
cl /nologo /c %CF% %INC% ^
    /Yc"std_include.hpp" /Fp"%OBJ%\shared\shared.pch" ^
    /Fo"%OBJ%\shared\std_include.obj" ^
    "%SRC%\shared\std_include.cpp"
if errorlevel 1 goto :fail

:: Compile sources
cl /nologo /c %CF% %INC% ^
    /Yu"std_include.hpp" /Fp"%OBJ%\shared\shared.pch" ^
    /Fo"%OBJ%\shared\\" ^
    "%SRC%\shared\common\config.cpp" ^
    "%SRC%\shared\common\ffp_state.cpp" ^
    "%SRC%\shared\common\flags.cpp" ^
    "%SRC%\shared\common\imgui_helper.cpp" ^
    "%SRC%\shared\common\loader.cpp" ^
    "%SRC%\shared\common\log.cpp" ^
    "%SRC%\shared\common\remix_api.cpp" ^
    "%SRC%\shared\globals.cpp" ^
    "%SRC%\shared\utils\utils.cpp"
if errorlevel 1 goto :fail
lib /nologo /OUT:"%OUT%\_shared.lib" "%OBJ%\shared\*.obj"
if errorlevel 1 goto :fail

:: -------------------------------------------------------
:: Step 5: comp DLL (C++ with PCH, links everything)
:: -------------------------------------------------------
echo [5/6] %NAME% (d3d9.dll)

:: Create PCH
cl /nologo /c %CF% %INC% /I"%COMP_DIR%\.." ^
    /Yc"std_include.hpp" /Fp"%GAME_OBJ%\comp.pch" ^
    /Fo"%GAME_OBJ%\std_include.obj" ^
    "%COMP_DIR%\std_include.cpp"
if errorlevel 1 goto :fail

:: Every .cpp under the comp folder except std_include.cpp.
set "COMP_SRCS="
for /r "%COMP_DIR%" %%f in (*.cpp) do (
    if /i not "%%~nxf"=="std_include.cpp" (
        set "COMP_SRCS=!COMP_SRCS! "%%f""
    )
)

cl /nologo /c %CF% %INC% /I"%COMP_DIR%\.." ^
    /Yu"std_include.hpp" /Fp"%GAME_OBJ%\comp.pch" ^
    /Fo"%GAME_OBJ%\\" ^
    %COMP_SRCS%
if errorlevel 1 goto :fail

:: Link DLL as d3d9.dll proxy (exports via .def, no d3d9.lib import)
set "TRACY_LIBS="
if "%TRACY%"=="1" set "TRACY_LIBS="%OUT%\tracy.lib" dbghelp.lib ws2_32.lib"
link /nologo /DLL /SUBSYSTEM:WINDOWS /DEBUG /PDBCompress /PDBALTPATH:%%_PDB%% %LF% %LIBPATH% ^
    /DEF:"%ROOT%d3d9.def" ^
    /OUT:"%GAME_OUT%\d3d9.dll" ^
    /PDB:"%GAME_OUT%\%NAME%.pdb" ^
    "%GAME_OBJ%\*.obj" ^
    "%OUT%\_shared.lib" ^
    "%OUT%\imgui.lib" ^
    "%OUT%\minhook.lib" ^
    "%OUT%\miniz.lib" ^
    %TRACY_LIBS% ^
    d3dx9.lib dxguid.lib psapi.lib user32.lib gdi32.lib shell32.lib advapi32.lib ole32.lib
if errorlevel 1 goto :fail

:: -------------------------------------------------------
:: Step 6: NewVegasRTX-Reporter.exe (32-bit, next to d3d9.dll)
:: -------------------------------------------------------
:: It compiles the wrapper's session zip sources itself, without the PCH: its
:: own std_include.hpp is found first through /I.
echo [6/6] NewVegasRTX-Reporter.exe
set "ROBJ=%OBJ%\reporter"
mkdir "%ROBJ%" 2>nul

:: The version resource's numbers from the release tag ("1.0.4-rc3" -> 1,0,4,0; "dev" -> 0,0,0,0).
set "RV_MAJ=0"
set "RV_MIN=0"
set "RV_PAT=0"
for /f "tokens=1-3 delims=.-+" %%a in ("%RELEASE_TAG%") do (
    set /a "RV_MAJ=%%a+0" >nul 2>&1
    set /a "RV_MIN=%%b+0" >nul 2>&1
    set /a "RV_PAT=%%c+0" >nul 2>&1
)
(
    echo #define FNVR_VERSION_NUM !RV_MAJ!,!RV_MIN!,!RV_PAT!,0
    echo #define FNVR_VERSION_STR "!RELEASE_TAG!"
) > "%ROBJ%\reporter_version.h"
rc /nologo /I"%ROBJ%" /fo"%ROBJ%\reporter.res" "%SRC%\reporter\reporter.rc"
if errorlevel 1 goto :fail

cl /nologo /c %CF% /I"%SRC%\reporter" /I"%SRC%" /I"%DEPS%\miniz" ^
    /Fo"%ROBJ%\\" /Fd"%ROBJ%\vc.pdb" ^
    "%SRC%\reporter\reporter.cpp" ^
    "%SRC%\comp\game\session_zip.cpp" ^
    "%SRC%\comp\game\zip_store.cpp"
if errorlevel 1 goto :fail

link /nologo /SUBSYSTEM:WINDOWS /DEBUG /PDBALTPATH:%%_PDB%% %LF% ^
    /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker' uiAccess='false'" ^
    /OUT:"%GAME_OUT%\NewVegasRTX-Reporter.exe" ^
    /PDB:"%GAME_OUT%\NewVegasRTX-Reporter.pdb" ^
    "%ROBJ%\*.obj" ^
    "%ROBJ%\reporter.res" ^
    "%OUT%\miniz.lib" ^
    user32.lib shell32.lib advapi32.lib
if errorlevel 1 goto :fail

:: Copy the ini to the output folder. With --comp, an ini next to that build folder wins.
if "!CUSTOM_COMP!"=="1" (
    if exist "!GAME_DIR!\remix-comp.ini" (
        copy /y "!GAME_DIR!\remix-comp.ini" "%GAME_OUT%\remix-comp.ini" >nul
    ) else if exist "%ROOT%remix-comp.ini" (
        copy /y "%ROOT%remix-comp.ini" "%GAME_OUT%\remix-comp.ini" >nul
    )
) else (
    if exist "%ROOT%remix-comp.ini" (
        copy /y "%ROOT%remix-comp.ini" "%GAME_OUT%\remix-comp.ini" >nul
    )
)

echo.
echo === Build succeeded: %GAME_OUT%\d3d9.dll, %GAME_OUT%\NewVegasRTX-Reporter.exe ===
exit /b 0

:no_dxsdk
echo ERROR: DirectX SDK (June 2010) not found.
echo   DXSDK_DIR = "!DXSDK_DIR!"
echo   Install the June 2010 DirectX SDK, or set DXSDK_DIR to a folder that has
echo   Include\d3dx9.h and Lib\x86\d3dx9.lib.
exit /b 1

:fail
echo.
echo === Build FAILED ===
exit /b 1
