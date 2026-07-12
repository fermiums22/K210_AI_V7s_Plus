@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "TC=%K210_TC%"
if "%TC%"=="" set "TC=C:\K210\toolchain\kendryte-toolchain\bin"
set "MAKE=%TC%\mingw32-make.exe"
if not exist "%MAKE%" set "MAKE=C:\msys64\mingw64\bin\mingw32-make.exe"
set "BUILD=%CD%\build_recovery_esp_v2"
set "ESP_BUILD=%CD%\..\K210_ESP_SPI_WIFI\esp8266_rtos_v2\bridge\build"

if not exist "%ESP_BUILD%\bootloader\bootloader.bin" (
  echo ERROR: ESP v2 bootloader image missing. Build K210_ESP_SPI_WIFI first.
  exit /b 2
)
if not exist "%ESP_BUILD%\partitions_1mb_singleapp.bin" (
  echo ERROR: ESP v2 partition image missing.
  exit /b 2
)
if not exist "%ESP_BUILD%\kesp-klink-bridge-v2.bin" (
  echo ERROR: ESP v2 application image missing.
  exit /b 2
)
if not exist "%MAKE%" (
  echo ERROR: mingw32-make missing: %MAKE%
  exit /b 2
)

set "ESP_BUILD_CMAKE=%ESP_BUILD:\=/%"
cmake -S firmware_v2\recovery_esp -B "%BUILD%" -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="%MAKE%" -DTOOLCHAIN="%TC%" -DESP_V2_BUILD_DIR="%ESP_BUILD_CMAKE%" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1

"%MAKE%" -C "%BUILD%" -j4 k210_esp_recovery_v2
if errorlevel 1 exit /b 1

if not exist "%BUILD%\k210_esp_recovery_v2.bin" (
  echo ERROR: recovery binary missing.
  exit /b 1
)
echo K210_ESP_RECOVERY_V2_BUILD_OK %BUILD%\k210_esp_recovery_v2.bin
exit /b 0
