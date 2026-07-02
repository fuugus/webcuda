@echo off
setlocal enabledelayedexpansion
rem webcuda Windows build wrapper. Double-click or run from any shell:
rem
rem   build.bat            build the Windows binary (-> dist-windows\)
rem   build.bat --test     additionally run the self-test
rem
rem Sets up the MSVC x64 environment (vcvars64) and CC/CXX=cl, locates Git
rem Bash, then delegates to: build.sh windows
cd /d "%~dp0"

rem ---- MSVC environment (skip when already in a Native Tools prompt) -------
where cl.exe >nul 2>nul
if not errorlevel 1 goto msvc_ready

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto vs_scan
rem !delayed! expansion: a %%-expanded path here would inject the ')' from
rem "(x86)" into the parser and break the for-set
for /f "usebackq delims=" %%i in (`""!VSWHERE!" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul"`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
if defined VCVARS if exist "%VCVARS%" goto msvc_call

:vs_scan

rem vswhere misses some Build Tools installs; scan the usual locations
set "VCVARS="
for %%d in (
  "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Community"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Professional"
  "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools"
  "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools"
) do if not defined VCVARS if exist "%%~d\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%%~d\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS goto no_msvc

:msvc_call
echo [build.bat] MSVC env: %VCVARS%
rem 2>nul: vcvars probes vswhere and is noisy when the VS Installer dir is
rem stale; success is judged by cl.exe becoming available, not by its output
call "%VCVARS%" >nul 2>nul
where cl.exe >nul 2>nul
if errorlevel 1 goto no_msvc

:msvc_ready
rem pin the compiler: other toolchains on PATH (e.g. Strawberry gcc) must not win
set "CC=cl"
set "CXX=cl"

rem ---- Git Bash (NOT WSL's System32\bash.exe) -------------------------------
set "BASH=%ProgramFiles%\Git\bin\bash.exe"
if exist "%BASH%" goto bash_ready
set "BASH=%LocalAppData%\Programs\Git\bin\bash.exe"
if exist "%BASH%" goto bash_ready
set "GITEXE="
for /f "delims=" %%i in ('where git.exe 2^>nul') do if not defined GITEXE set "GITEXE=%%i"
if not defined GITEXE goto no_bash
set "BASH=%GITEXE:\cmd\git.exe=\bin\bash.exe%"
if exist "%BASH%" goto bash_ready
goto no_bash

:bash_ready
"%BASH%" build.sh windows %*
exit /b %errorlevel%

:no_msvc
echo [build.bat] ERROR: MSVC not found. Install "Visual Studio Build Tools"
echo             (C++ x64 tools workload), or run this from a "x64 Native
echo             Tools Command Prompt".
exit /b 1

:no_bash
echo [build.bat] ERROR: Git Bash not found (build.sh needs it). Install Git
echo             for Windows: https://gitforwindows.org
exit /b 1
