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
set CFLAGS=/std:c++17 /EHsc /MD /nologo /Oi /MP /GF /Z7 /arch:AVX512
if "%MODE%" equ "debug" (
	set CFLAGS=%CFLAGS% /Od
) else if "%MODE%" equ "release" (
	set CFLAGS=%CFLAGS% /O2
)

rem set linker flags
rem set LFLAGS=/DEFAULTLIB:MSVCRT

clang-cl.exe -fuse-ld=lld %CFLAGS% /Fe:run main.cpp /link %LFLAGS% || exit /b 1
