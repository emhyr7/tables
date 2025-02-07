@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

rem check Visual Studio installation 
where /Q cl.exe || (
	set __VSCMD_ARG_NO_LOGO=1
	for /f "tokens=*" %%i in ('"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.VisualStudio.Workload.NativeDesktop -property installationPath') do set VS=%%i
	if "!VS!" equ "" echo ERROR: Visual Studio installation unfound && exit /b 1
	call "!VS!\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
)

rem parse commandline
set MODE=debug
if "%1" == "release" set MODE=release

rem set compiler flags
set CFLAGS=/std:c++17 /nologo /Oi /MP /GF /utf-8 /Z7 /arch:AVX512
if "%MODE%" equ "debug" (
	set CFLAGS=%CFLAGS% /Od /MDd
) else if "%MODE%" equ "release" (
	set CFLAGS=%CFLAGS% /O2 /MT
)

rem set linker flags
set LFLAGS=/DEBUG /INCREMENTAL:NO /OPT:REF /SUBSYSTEM:CONSOLE

clang-cl.exe %CFLAGS% /Fe:run 0.c -link %LFLAGS% || exit /b 1
