@echo off
setlocal

rem SiN VR build -- 32-bit only. SinEpisodes.exe is a 32-bit process, so every
rem DLL we inject must be x86 regardless of what this machine is.

rem Find vcvars32.bat rather than hardcoding it. vswhere ships with every VS
rem 2017+ installer and knows about all editions, so this works for Build Tools,
rem Community, Professional and Enterprise without anyone editing this file --
rem which the previous hardcoded path required on every new machine.
rem
rem NOTE: keep the quotes on every set/echo below. The (x86) in these paths will
rem otherwise terminate the enclosing if-block early.
rem Kept flat -- assign in the loop, test afterwards -- rather than nesting the
rem for/f inside an if-block. Both forms were tested and both work; flat is just
rem one less thing to reason about given every path here contains "(x86)".
rem
rem Do NOT be fooled by this on the way past:
rem     'vswhere.exe' is not recognized as an internal or external command
rem That comes from inside Microsoft's own vcvars32.bat, not from the lookup
rem below. It is harmless -- vcvars32 still returns 0 and sets the environment
rem correctly -- and it appears whether or not this script uses vswhere at all.
set "VSPATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto :no_vswhere
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
:no_vswhere

set "VCVARS="
if defined VSPATH set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo ERROR: vcvars32.bat not found. Looked via vswhere and at:
    echo   "%VCVARS%"
    echo Install the "Desktop development with C++" workload:
    echo   winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    exit /b 1
)
echo Using MSVC from "%VCVARS%"

call "%VCVARS%" >nul
if errorlevel 1 (
    echo ERROR: vcvars32 failed.
    exit /b 1
)

set ROOT=%~dp0
set OUT=%ROOT%build
set OBJ=%OUT%\obj
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"

rem OpenVR: headers only. openvr_api.dll is loaded at runtime via GetProcAddress,
rem so we deliberately do NOT link openvr_api.lib -- that would make sinvr.dll
rem refuse to load whenever the DLL is missing, taking head tracking and the
rem hook down together.
set "OPENVR=%ROOT%..\openvr-master"
if not exist "%OPENVR%\headers\openvr.h" (
    echo ERROR: openvr.h not found at:
    echo   "%OPENVR%\headers\openvr.h"
    echo Edit OPENVR in build.bat to point at your openvr checkout.
    exit /b 1
)

set CFLAGS=/nologo /c /EHa /O2 /MT /W3 /std:c++17 /DWIN32 /D_WINDOWS /DNDEBUG /D_CRT_SECURE_NO_WARNINGS
set LDFLAGS=/nologo /DLL /MACHINE:X86

echo.
echo === sinvr.dll (mod logic) ===
cl %CFLAGS% /Fo"%OBJ%\sinvr_main.obj" "%ROOT%src\sinvr\dllmain.cpp"
if errorlevel 1 goto :fail

cl %CFLAGS% /I"%OPENVR%\headers" /Fo"%OBJ%\openvr_backend.obj" "%ROOT%src\sinvr\vr\openvr_backend.cpp"
if errorlevel 1 goto :fail

cl %CFLAGS% /Fo"%OBJ%\d3d9_present_hook.obj" "%ROOT%src\sinvr\render\d3d9_present_hook.cpp"
if errorlevel 1 goto :fail

cl %CFLAGS% /Fo"%OBJ%\stereo.obj" "%ROOT%src\sinvr\render\stereo.cpp"
if errorlevel 1 goto :fail

rem No d3d9.lib: Direct3DCreate9 and Direct3DCreateVR9 are both resolved at
rem runtime via GetProcAddress, so sinvr.dll keeps zero load-time dependencies.
link %LDFLAGS% /OUT:"%OUT%\sinvr.dll" "%OBJ%\sinvr_main.obj" "%OBJ%\openvr_backend.obj" ^
    "%OBJ%\d3d9_present_hook.obj" "%OBJ%\stereo.obj" kernel32.lib user32.lib
if errorlevel 1 goto :fail

echo.
echo === dinput8.dll (proxy) ===
cl %CFLAGS% /Fo"%OBJ%\proxy_main.obj" "%ROOT%src\proxy\dllmain.cpp"
if errorlevel 1 goto :fail

link %LDFLAGS% /DEF:"%ROOT%src\proxy\dinput8.def" /OUT:"%OUT%\dinput8.dll" ^
    "%OBJ%\proxy_main.obj" kernel32.lib user32.lib
if errorlevel 1 goto :fail

rem The launcher is a normal console exe rather than an injected DLL, and it is
rem what gives the game its 4 GB address space -- the PE flag is read by the
rem loader at process creation, long before dinput8.dll can pull the mod in, so
rem there is no runtime equivalent and it has to happen here.
echo.
echo === sinvr_launcher.exe (4 GB launcher) ===
rem /I openvr headers: the launcher asks the runtime for the headset's
rem recommended render size so the game window matches it. Headers only --
rem openvr_api.dll is still loaded at runtime, so the launcher starts even
rem with no runtime installed.
cl %CFLAGS% /I"%OPENVR%\headers" /Fo"%OBJ%\launcher_main.obj" "%ROOT%src\launcher\main.cpp"
if errorlevel 1 goto :fail

link /nologo /MACHINE:X86 /SUBSYSTEM:CONSOLE /OUT:"%OUT%\sinvr_launcher.exe" ^
    "%OBJ%\launcher_main.obj" kernel32.lib user32.lib advapi32.lib gdi32.lib
if errorlevel 1 goto :fail

rem The OpenVR action manifest and its default bindings. Staged into build\ with
rem everything else so "copy build\ next to the exe" stays the whole deploy step
rem -- the mod looks for actions\sinvr_actions.json beside SinEpisodes.exe and
rem degrades to no controller input if it is missing.
if not exist "%OUT%\actions" mkdir "%OUT%\actions"
copy /Y "%ROOT%actions\*.json" "%OUT%\actions\" >nul
if errorlevel 1 (
    echo ERROR: could not stage the action manifest into "%OUT%\actions"
    goto :fail
)

rem Optional content overrides. Staged so they are versioned with the mod rather
rem than living only as a hand-edit in someone's game folder, but NOT applied --
rem they change game content, so applying one is the user's decision. See
rem content\README.md.
if not exist "%OUT%\content" mkdir "%OUT%\content"
xcopy /Y /E /I /Q "%ROOT%content" "%OUT%\content" >nul
if errorlevel 1 (
    echo ERROR: could not stage content overrides into "%OUT%\content"
    goto :fail
)

echo.
echo === built ===
echo   %OUT%\sinvr.dll
echo   %OUT%\dinput8.dll
echo   %OUT%\sinvr_launcher.exe
echo   %OUT%\actions\  (OpenVR action manifest + default bindings)
echo.
echo Copy all three DLLs, sinvr_launcher.exe and the actions\ FOLDER next to
echo SinEpisodes.exe on the VR rig, then start the game with sinvr_launcher.exe
echo rather than through Steam.
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
