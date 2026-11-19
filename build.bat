@echo off
rem Builds CoopMod.asi (32-bit). If the game folder is the parent (..\speed.exe), also installs it
rem into ..\scripts. Set COOPMOD_VERSION to stamp a version (CI does this).
setlocal
cd /d "%~dp0"

rem Use the compiler already on PATH (CI / Developer Command Prompt x86), else find vcvars32.
where cl >nul 2>nul && goto :have_cl
set VCVARS=G:\Programs\Visual Studio\Community\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat
)
call "%VCVARS%" >nul || (echo vcvars32 not found & exit /b 1)
:have_cl

if not exist build mkdir build
set VERDEF=
if defined COOPMOD_VERSION set VERDEF=/DCOOPMOD_VERSION=\"%COOPMOD_VERSION%\"

cl /nologo /O2 /MT /EHsc /std:c++17 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS %VERDEF% ^
   /Fo"build\\" /Fd"build\\" ^
   src\main.cpp src\log.cpp src\config.cpp src\net.cpp src\overlay.cpp src\mem.cpp src\game.cpp src\input.cpp ^
   /LD /Fe"build\CoopMod.asi" ^
   /link /SAFESEH:NO /DYNAMICBASE:NO ws2_32.lib user32.lib gdi32.lib dxguid.lib || exit /b 1

if not exist "..\speed.exe" (echo Built build\CoopMod.asi & exit /b 0)
copy /y "build\CoopMod.asi" "..\scripts\CoopMod.asi" >nul || (echo copy failed - is the game running? & exit /b 1)
if not exist "..\scripts\CoopMod.ini" copy /y "CoopMod.ini" "..\scripts\CoopMod.ini" >nul
echo Built and installed scripts\CoopMod.asi
