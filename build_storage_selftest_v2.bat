@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "TC=%K210_TC%"
if "%TC%"=="" set "TC=C:\K210\toolchain\kendryte-toolchain\bin"
set "MAKE=%TC%\mingw32-make.exe"
if not exist "%MAKE%" set "MAKE=C:\msys64\mingw64\bin\mingw32-make.exe"
set "BUILD=%CD%\build_storage_selftest_v2"
cmake -S firmware_v2\storage_selftest -B "%BUILD%" -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="%MAKE%" -DTOOLCHAIN="%TC%" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1
"%MAKE%" -C "%BUILD%" -j4 k210_storage_selftest_v2
if errorlevel 1 exit /b 1
if not exist "%BUILD%\k210_storage_selftest_v2.bin" exit /b 1
echo K210_STORAGE_SELFTEST_V2_BUILD_OK %BUILD%\k210_storage_selftest_v2.bin
