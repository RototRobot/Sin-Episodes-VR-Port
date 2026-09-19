@echo off
setlocal

rem Builds the SiN VR DXVK fork as 32-bit d3d9.dll.
rem
rem SinEpisodes.exe is a 32-bit process, so this MUST be an x86 build -- hence
rem vcvars32 rather than vcvars64.
rem
rem meson and ninja came from pip, which did not create launcher exes, so both
rem are invoked through the Python that owns them.

rem All three of these used to be hardcoded absolute paths, which meant editing
rem this file on every new machine. They are found instead.

rem ---- MSVC ------------------------------------------------------------------
rem vswhere ships with every VS 2017+ installer and knows about all editions.
rem Keep the quotes: the (x86) in these paths ends an if-block early without them.
rem Kept flat for the same reason as build.bat, and with the same warning: the
rem "'vswhere.exe' is not recognized" line that appears during the build comes
rem from inside Microsoft's vcvars32.bat, not from this lookup. Harmless.
set "VSPATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
:no_vswhere

set "VCVARS="
if defined VSPATH set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvars32 not found. Install the "Desktop development with C++" workload.
    exit /b 1
)

rem ---- Python ----------------------------------------------------------------
rem meson and ninja come from pip and have no launcher exes, so both are invoked
rem through the Python that owns them. Prefer the py launcher, then PATH.
rem
rem Note the WindowsApps python.exe is a Microsoft Store stub that installs
rem nothing and exits -- so "python is on PATH" is not enough on a fresh machine,
rem and `py -3` is checked first for that reason.
rem Captured through a temp file rather than `for /f ... in (`cmd`)`. Batch ends
rem the in-clause at the first unescaped ")" even inside backticks, so any
rem python -c "...print(x)..." breaks the parse with a cryptic
rem   cc`) was unexpected at this time.
rem Verified the hard way; set /p has no such hazard.
set "PYPROBE=%TEMP%\sinvr_pypath.txt"
del "%PYPROBE%" >nul 2>&1
py -3 -c "import sys; sys.stdout.write(sys.executable)" > "%PYPROBE%" 2>nul
set "PY="
if exist "%PYPROBE%" set /p PY=<"%PYPROBE%"
if not defined PY (
    python -c "import sys; sys.stdout.write(sys.executable)" > "%PYPROBE%" 2>nul
    if exist "%PYPROBE%" set /p PY=<"%PYPROBE%"
)
del "%PYPROBE%" >nul 2>&1
if not defined PY (
    echo ERROR: no working Python found. Install it and the build deps:
    echo   winget install Python.Python.3.12
    echo   py -3 -m pip install meson ninja
    exit /b 1
)
echo Using Python "%PY%"

rem meson and ninja must actually be importable, not merely "Python exists".
"%PY%" -c "import mesonbuild" >nul 2>&1
if errorlevel 1 (
    echo ERROR: meson is not installed for "%PY%". Run:
    echo   "%PY%" -m pip install meson ninja
    exit /b 1
)

rem ninja ships its exe inside the site-packages payload, with no launcher, so
rem its directory has to go on PATH for meson to find it.
set "NINJAPROBE=%TEMP%\sinvr_ninjapath.txt"
del "%NINJAPROBE%" >nul 2>&1
"%PY%" -c "import os,ninja,sys; sys.stdout.write(os.path.join(os.path.dirname(ninja.__file__),'data','bin'))" > "%NINJAPROBE%" 2>nul
set "NINJA_DIR="
if exist "%NINJAPROBE%" set /p NINJA_DIR=<"%NINJAPROBE%"
del "%NINJAPROBE%" >nul 2>&1

call "%VCVARS%" >nul 2>&1
if defined NINJA_DIR set "PATH=%NINJA_DIR%;%PATH%"

rem DXVK compiles its helper shaders at build time (meson.build:190) and needs
rem glslang or glslangValidator. Look in the usual places so it does not matter
rem which route was used to install it.
set "GLSLANG_DIR="

rem 1. Already on PATH?
where glslangValidator >nul 2>&1 && goto :glslang_ok
where glslang >nul 2>&1 && goto :glslang_ok

rem 2. Vulkan SDK (newest wins -- the for loop leaves the last match set).
for /d %%D in ("C:\VulkanSDK\*") do (
    if exist "%%D\Bin\glslangValidator.exe" set "GLSLANG_DIR=%%D\Bin"
)

rem 3. Standalone Khronos zip extracted into tools\glslang next to this script.
if not defined GLSLANG_DIR (
    if exist "%~dp0tools\glslang\bin\glslangValidator.exe" set "GLSLANG_DIR=%~dp0tools\glslang\bin"
)

if defined GLSLANG_DIR (
    echo Using glslang from "%GLSLANG_DIR%"
    set "PATH=%GLSLANG_DIR%;%PATH%"
    goto :glslang_ok
)

echo.
echo ERROR: glslangValidator not found. DXVK cannot compile its shaders without it.
echo.
echo Either:
echo   a^) install the LunarG Vulkan SDK ^(also useful for validation layers^), or
echo   b^) grab glslang-master-windows-x64-Release.zip from
echo      https://github.com/KhronosGroup/glslang/releases and extract it to:
echo        "%~dp0tools\glslang"
echo      so that "%~dp0tools\glslang\bin\glslangValidator.exe" exists.
echo.
exit /b 1

:glslang_ok

set "ROOT=%~dp0dxvk-sinvr"
set "BUILD=%ROOT%\build-win32"

if "%1"=="clean" (
    echo Removing "%BUILD%"
    if exist "%BUILD%" rmdir /s /q "%BUILD%"
)

if not exist "%BUILD%" (
    echo === meson setup ===
    rem SiN only ever calls D3D9. d3d8 additionally fails to build here because
    rem it needs d3d8.h from the old DirectX SDK, which the Windows SDK does not
    rem ship; the rest are switched off to keep the build small and fast.
    "%PY%" -m mesonbuild.mesonmain setup --backend ninja --buildtype release ^
        -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false ^
        --prefix "%ROOT%\install" "%BUILD%" "%ROOT%"
    if errorlevel 1 goto :fail
)

echo.
echo === ninja build ===
"%PY%" -m mesonbuild.mesonmain compile -C "%BUILD%"
if errorlevel 1 goto :fail

rem Stage it next to the mod DLLs. Without this the freshly built d3d9.dll stays
rem buried in build-win32 while build\d3d9.dll keeps whatever was there before --
rem so a DXVK change silently never reaches the test rig, and the symptom is
rem "the fix did nothing" rather than anything that looks like a build problem.
set "OUT=%~dp0build"
if not exist "%OUT%" mkdir "%OUT%"
copy /Y "%BUILD%\src\d3d9\d3d9.dll" "%OUT%\d3d9.dll" >nul
if errorlevel 1 (
    echo ERROR: could not copy d3d9.dll into "%OUT%"
    goto :fail
)

echo.
echo === built ===
echo   %OUT%\d3d9.dll
echo.
echo Copy it next to SinEpisodes.exe along with sinvr.dll and dinput8.dll.
exit /b 0

:fail
echo.
echo DXVK BUILD FAILED
exit /b 1
