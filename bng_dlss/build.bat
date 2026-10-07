@echo off
rem ============================================================
rem  bng_dlss build script
rem  Edit the four paths below for your machine, then double-click.
rem  Output: build\bng_dlss.addon64
rem ============================================================
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "RESHADE_INC=D:\dev\reshade68\include"
set "IMGUI_INC=D:\dev\imgui"
set "DLSS=D:\dev\DLSS"

if not defined VSCMD_VER call "%VCVARS%" >nul
if not exist "%~dp0build" mkdir "%~dp0build"
cd /d "%~dp0build"

cl /nologo /std:c++17 /O2 /EHsc /LD /utf-8 /I "%RESHADE_INC%" /I "%IMGUI_INC%" /I "%DLSS%\include" "%~dp0src\bng_dlss.cpp" /Fe:bng_dlss.addon64 /link /LIBPATH:"%DLSS%\lib\Windows_x86_64\x64"
if errorlevel 1 (
  echo.
  echo [!] BUILD FAILED
) else (
  echo.
  echo ===== OK: build\bng_dlss.addon64 =====
  echo Copy it next to BeamNG.drive.x64.exe ^(the Bin64 folder^).
)
echo.
pause
