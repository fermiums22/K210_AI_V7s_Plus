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

rem CMake treats backslashes inside linker flag strings as escapes.
rem Keep Windows file checks above, but pass only forward-slash paths into CMake.
set "SRC_CMAKE=%CD:\=/%"
set "BUILD_CMAKE=%BUILD:\=/%"
set "SDK_CMAKE=%SDK:\=/%"
set "TC_CMAKE=%TC:\=/%"
set "MAKE_CMAKE=%MAKE:\=/%"
set "APP_SLOT0_LD=%SRC_CMAKE%/lds/kendryte_app_slot0.ld"

echo === K210 app slot0 build ===
echo Repo:  %CD%
echo TC:    %TC%
echo MAKE:  %MAKE%
echo BUILD: %BUILD%
echo LD:    %APP_SLOT0_LD%
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
cmake -S . -B "%BUILD_CMAKE%" -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="%MAKE_CMAKE%" -DTOOLCHAIN="%TC_CMAKE%" -DSDK_ROOT="%SDK_CMAKE%" -DK210_LINKER_SCRIPT="%APP_SLOT0_LD%" -DK210_APP_SLOT0=1 -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1

rem The slot linker script contains _app_image_size.  CMake/MinGW does not
rem always relink when only this generated linker script changes, so remove
rem the final executable/bin only.  Objects and libraries stay incremental.
if exist "%BUILD%\robot_show" del /q "%BUILD%\robot_show"
if exist "%BUILD%\robot_show.bin" del /q "%BUILD%\robot_show.bin"
if exist "%BUILD%\k210_app_slot0.bin" del /q "%BUILD%\k210_app_slot0.bin"

echo [make] relinking slot0 target...
"%MAKE%" -C "%BUILD%" -j4 robot_show
if errorlevel 1 exit /b 1

if not exist "%BUILD%\robot_show.bin" (
  echo ERROR: binary missing: %BUILD%\robot_show.bin
  exit /b 1
)
copy /Y "%BUILD%\robot_show.bin" "%BUILD%\k210_app_slot0.bin" >nul

py -3 tools\print_slot0_header.py "%BUILD%\k210_app_slot0.bin"
if errorlevel 1 exit /b 1

echo.
echo OK: %BUILD%\k210_app_slot0.bin
echo Flash slot offset: 0x00100000
exit /b 0
