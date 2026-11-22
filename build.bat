@echo off
rem Builds MWTogether.asi (32-bit). If the game folder is the parent (..\speed.exe), also installs it
rem into ..\scripts. Set MWT_VERSION to stamp a version (CI does this). Set MWT_RELEASE=1 for a release
rem build (tools\release.ps1 and CI do this): without it the test commands and debug tools
rem ([Debug] TestKeys, scripts\MWTogether.cmd) are compiled in (MWT_DEVTOOLS).
setlocal
cd /d "%~dp0"

rem Use the compiler already on PATH (CI / Developer Command Prompt x86), else find vcvars32.
where cl >nul 2>nul && goto :have_cl
set VCVARS=G:\Programs\Visual Studio\Community\VC\Auxiliary\Build\vcvars32.bat
if not exist "%VCVARS%" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat
)
call "%VCVARS%" >nul || (echo vcvars32 not found & exit /b 1)
:have_cl

if not exist build mkdir build
set VERDEF=
set RCDEF=
if defined MWT_VERSION (
  set VERDEF=/DMWT_VERSION=\"%MWT_VERSION%\"
  rem file details: "1.6.18" or "0.0.0-abc1234" -> 1,6,18,0 / 0,0,0,0
  for /f "tokens=1-3 delims=.-" %%a in ("%MWT_VERSION%") do set RCDEF=/d MWT_VERSION_STR=\"%MWT_VERSION%\" /d MWT_VERSION_NUM=%%a,%%b,%%c,0
)
set DEVDEF=/DMWT_DEVTOOLS
if "%MWT_RELEASE%"=="1" set DEVDEF=

rc /nologo %RCDEF% /fo "build\version.res" src\version.rc || exit /b 1

rem /Gy + /OPT:REF: code nothing calls is left out. /OPT:NOICF: identical functions stay separate
rem (with /OPT:ICF the game crashed at 0x46578C loading career free roam). /DYNAMICBASE: the plugin is
rem relocatable (the game's addresses are constants, ours are fixed up by the loader). /SAFESEH: only
rem our own registered exception handlers.
cl /nologo /O2 /Gy /MT /EHsc /std:c++17 /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS %VERDEF% %DEVDEF% ^
   /Fo"build\\" /Fd"build\\" ^
   src\main.cpp src\log.cpp src\config.cpp src\net.cpp src\overlay.cpp src\mem.cpp src\game.cpp src\input.cpp src\pad.cpp src\together.cpp src\nativeui.cpp src\logo.cpp ^
   build\version.res ^
   /LD /Fe"build\MWTogether.asi" ^
   /link /SAFESEH /DYNAMICBASE /NXCOMPAT /OPT:REF /OPT:NOICF ws2_32.lib user32.lib gdi32.lib dxguid.lib shell32.lib iphlpapi.lib xinput9_1_0.lib || exit /b 1
if defined DEVDEF (echo Dev build: test commands included) else (echo Release build: no test commands)

if not exist "..\speed.exe" (echo Built build\MWTogether.asi & exit /b 0)
rem renamed from CoopMod: remove the old plugin (both would load) and keep its settings
if exist "..\scripts\CoopMod.asi" del "..\scripts\CoopMod.asi"
if not exist "..\scripts\MWTogether.ini" if exist "..\scripts\CoopMod.ini" copy /y "..\scripts\CoopMod.ini" "..\scripts\MWTogether.ini" >nul
copy /y "build\MWTogether.asi" "..\scripts\MWTogether.asi" >nul || (echo copy failed - is the game running? & exit /b 1)
if not exist "..\scripts\MWTogether.ini" copy /y "MWTogether.ini" "..\scripts\MWTogether.ini" >nul
echo Built and installed scripts\MWTogether.asi
