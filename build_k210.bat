@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "TC=%K210_TC%"
if "%TC%"=="" set "TC=C:\K210\toolchain\kendryte-toolchain\bin"
set "NINJA=C:\msys64\mingw64\bin\ninja.exe"
set "BUILD=%CD%\build"

if not exist "%NINJA%" (
  echo ERROR: ninja not found: %NINJA%
  exit /b 2
)

cmake -S firmware_v2\kstream_slave -B "%BUILD%" -G Ninja -DCMAKE_MAKE_PROGRAM="%NINJA%" -DTOOLCHAIN="%TC%"
if errorlevel 1 exit /b 1
cmake --build "%BUILD%" --parallel
if errorlevel 1 exit /b 1

if not exist "%BUILD%\k210_kstream_slave_v2.bin" exit /b 1
echo K210_BUILD_OK %BUILD%\k210_kstream_slave_v2.bin
