@echo off
setlocal

rem openxr_probe -- 32-bit ONLY. A 64-bit build reads the 64-bit OpenXR runtime
rem and proves nothing about SiN, so the probe refuses to run if it is not x86.
rem
rem Same vcvars32 discovery as build.bat, for the same reason: vswhere ships with
rem every VS 2017+ installer, so this works on Build Tools, Community,
rem Professional and Enterprise without anyone editing a hardcoded path.
rem
rem Keep the quotes on every set/echo. The (x86) in these paths will otherwise
rem terminate the enclosing if-block early.
rem
rem Ignore this on the way past -- it comes from inside Microsoft's own
rem vcvars32.bat and is harmless:
rem     'vswhere.exe' is not recognized as an internal or external command

set "VSPATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
:no_vswhere

set "VCVARS="
if defined VSPATH set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvars32.bat not found.
    exit /b 1
)
call "%VCVARS%" >nul
if errorlevel 1 (
    echo ERROR: vcvars32 failed.
    exit /b 1
)

set "HERE=%~dp0"
set "ROOT=%HERE%..\.."

rem Vulkan headers come from the DXVK tree we already have -- no SDK install.
set "VULKAN_INC=%ROOT%\dxvk-sinvr\include\vulkan\include"
if not exist "%VULKAN_INC%\vulkan\vulkan.h" (
    echo ERROR: vulkan.h not found at:
    echo   "%VULKAN_INC%\vulkan\vulkan.h"
    exit /b 1
)

rem OpenXR: headers only, from a Khronos OpenXR-SDK checkout beside the project
rem -- exactly how build.bat consumes openvr-master. Nothing is linked against
rem them: the probe reaches the runtime through xrNegotiateLoaderRuntimeInterface
rem itself, so no openxr_loader.dll is needed either (SteamVR ships none for
rem 32-bit anyway).
rem
rem Discovered rather than hardcoded so an SDK version bump does not need this
rem file edited -- the release folder carries its version in its name. Set the
rem OPENXR_SDK environment variable to override.
set "OPENXR_INC="
if defined OPENXR_SDK if exist "%OPENXR_SDK%\include\openxr\openxr.h" set "OPENXR_INC=%OPENXR_SDK%\include"
if not defined OPENXR_INC (
    for /d %%d in ("%ROOT%\..\OpenXR-SDK*") do (
        if exist "%%~fd\include\openxr\openxr.h" set "OPENXR_INC=%%~fd\include"
    )
)
if not defined OPENXR_INC (
    echo ERROR: the OpenXR SDK headers were not found.
    echo Looked for an OpenXR-SDK* folder beside the project, i.e.
    echo   "%ROOT%\..\OpenXR-SDK-release-X.Y.Z\include\openxr\openxr.h"
    echo.
    echo Get it from the Khronos OpenXR-SDK repo, or point OPENXR_SDK at yours:
    echo   set OPENXR_SDK=C:\path\to\OpenXR-SDK
    exit /b 1
)
echo Using OpenXR headers from "%OPENXR_INC%"

if not exist "%HERE%build" mkdir "%HERE%build"

echo === openxr_probe.exe (x86) ===
cl /nologo /EHsc /O2 /MT /W3 /std:c++17 /DWIN32 /D_WINDOWS /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /I"%VULKAN_INC%" /I"%OPENXR_INC%" ^
   /Fo"%HERE%build\\" /Fe"%HERE%build\openxr_probe.exe" ^
   "%HERE%probe.cpp" ^
   /link /MACHINE:X86 /SUBSYSTEM:CONSOLE kernel32.lib user32.lib advapi32.lib ole32.lib
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo === built ===
echo   %HERE%build\openxr_probe.exe
echo.
echo Run it with SteamVR up and the headset awake:
echo   "%HERE%build\openxr_probe.exe"
echo.
echo It defaults to the game folder's d3d9.dll. Override with:
echo   openxr_probe.exe --d3d9 "C:\path\to\d3d9.dll"
