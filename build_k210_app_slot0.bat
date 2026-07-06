@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "SDK=%K210_SDK%"
if "%SDK%"=="" if exist "C:\K210\sdk\kendryte-freertos-sdk-0.7.0" set "SDK=C:\K210\sdk\kendryte-freertos-sdk-0.7.0"
if "%SDK%"=="" if exist "%CD%\firmware\sdk-freertos" set "SDK=%CD%\firmware\sdk-freertos"
if "%SDK%"=="" set "SDK=C:\K210\sdk\kendryte-freertos-sdk-0.7.0"

set "TC=%K210_TC%"
if "%TC%"=="" if exist "C:\K210\toolchain\kendryte-toolchain\bin\riscv64-unknown-elf-gcc.exe" set "TC=C:\K210\toolchain\kendryte-toolchain\bin"
if "%TC%"=="" if exist "%CD%\toolchain\kendryte-toolchain\bin\riscv64-unknown-elf-gcc.exe" set "TC=%CD%\toolchain\kendryte-toolchain\bin"
if "%TC%"=="" set "TC=C:\K210\toolchain\kendryte-toolchain\bin"

set "BUILD=%CD%\build_app_slot0"
set "MAKE=%TC%\mingw32-make.exe"
if not exist "%MAKE%" set "MAKE=C:\msys64\mingw64\bin\mingw32-make.exe"
set "WIFI_CFG=%CD%\src\wifi_cfg.h"

echo === K210 app slot0 build ===
echo Repo:  %CD%
echo TC:    %TC%
echo MAKE:  %MAKE%
echo BUILD: %BUILD%
echo.

if not exist "%TC%" (
  echo ERROR: K210 toolchain not found: %TC%
  exit /b 1
)
if not exist "%MAKE%" (
  echo ERROR: mingw32-make.exe not found
  exit /b 1
)
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: cmake not found in PATH
  exit /b 1
)

if not exist "%WIFI_CFG%" (
  echo [cfg] src\wifi_cfg.h not found, creating local empty Wi-Fi config...
  > "%WIFI_CFG%" echo #pragma once
  >> "%WIFI_CFG%" echo #define WIFI_SSID ""
  >> "%WIFI_CFG%" echo #define WIFI_PASS ""
  echo [cfg] Edit src\wifi_cfg.h later if AT Wi-Fi mode is needed.
  echo.
)

py -3 tools\make_app_slot0_ld.py
if errorlevel 1 exit /b 1

if not exist "%BUILD%" mkdir "%BUILD%"

echo [cmake] configuring slot0...
cmake -S . -B "%BUILD%" -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="%MAKE%" -DTOOLCHAIN="%TC%" -DSDK_ROOT="%SDK%" -DK210_LINKER_SCRIPT="%CD%/lds/kendryte_app_slot0.ld" -DK210_APP_SLOT0=1 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1

echo [make] building slot0...
"%MAKE%" -C "%BUILD%" -j4
if errorlevel 1 exit /b 1

if not exist "%BUILD%\robot_show.bin" (
  echo ERROR: binary missing: %BUILD%\robot_show.bin
  exit /b 1
)
copy /Y "%BUILD%\robot_show.bin" "%BUILD%\k210_app_slot0.bin" >nul

echo.
echo OK: %BUILD%\k210_app_slot0.bin
echo Flash slot offset: 0x00100000
exit /b 0
