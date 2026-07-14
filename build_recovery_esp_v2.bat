@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "TC=%K210_TC%"
if "%TC%"=="" set "TC=C:\K210\toolchain\kendryte-toolchain\bin"
set "MAKE=%TC%\mingw32-make.exe"
if not exist "%MAKE%" set "MAKE=C:\msys64\mingw64\bin\mingw32-make.exe"
set "BUILD=%CD%\build_recovery_esp_v2"
set "ESP_BUILD=%CD%\..\K210_ESP_SPI_WIFI\firmware\build"
set "ESP_SDK=%ESP_SDK_ROOT%"
if "%ESP_SDK%"=="" if exist "D:\w_space\esp8266_sdk\ESP8266_RTOS_SDK\components\esp8266\firmware\esp_init_data_default.bin" set "ESP_SDK=D:\w_space\esp8266_sdk"
if "%ESP_SDK%"=="" set "ESP_SDK=C:\ESP8266\sdk"
set "RECOVERY_FORCE=%RECOVERY_FORCE_FLASH%"
if "%RECOVERY_FORCE%"=="" set "RECOVERY_FORCE=OFF"

if not exist "%ESP_BUILD%\bootloader\bootloader.bin" (
  echo ERROR: ESP v2 bootloader image missing. Build K210_ESP_SPI_WIFI first.
  exit /b 2
)
if not exist "%ESP_BUILD%\partitions_1mb_ota.bin" (
  echo ERROR: ESP v2 partition image missing.
  exit /b 2
)
if not exist "%ESP_BUILD%\esp8285-sta-klink.app1.bin" (
  echo ERROR: ESP v2 application image missing.
  exit /b 2
)
if not exist "%MAKE%" (
  echo ERROR: mingw32-make missing: %MAKE%
  exit /b 2
)

set "ESP_BUILD_CMAKE=%ESP_BUILD:\=/%"
set "ESP_BOOT_CMAKE=%ESP_BUILD%\bootloader\bootloader.bin"
set "ESP_PART_CMAKE=%ESP_BUILD%\partitions_1mb_ota.bin"
set "ESP_APP_CMAKE=%ESP_BUILD%\esp8285-sta-klink.app1.bin"
set "ESP_OTA_DATA_CMAKE=%ESP_BUILD%\ota_data_initial.bin"
set "ESP_PHY_CMAKE=%ESP_SDK%\ESP8266_RTOS_SDK\components\esp8266\firmware\esp_init_data_default.bin"
set "ESP_BLANK_CMAKE=%ESP_SDK%\ESP8266_RTOS_SDK\components\esp8266\firmware\blank.bin"
set "ESP_BOOT_CMAKE=%ESP_BOOT_CMAKE:\=/%"
set "ESP_PART_CMAKE=%ESP_PART_CMAKE:\=/%"
set "ESP_APP_CMAKE=%ESP_APP_CMAKE:\=/%"
set "ESP_OTA_DATA_CMAKE=%ESP_OTA_DATA_CMAKE:\=/%"
set "ESP_PHY_CMAKE=%ESP_PHY_CMAKE:\=/%"
set "ESP_BLANK_CMAKE=%ESP_BLANK_CMAKE:\=/%"
cmake -S firmware_v2\recovery_esp -B "%BUILD%" -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="%MAKE%" -DTOOLCHAIN="%TC%" -DESP_V2_BUILD_DIR="%ESP_BUILD_CMAKE%" -DESP_BOOT_BIN="%ESP_BOOT_CMAKE%" -DESP_PART_BIN="%ESP_PART_CMAKE%" -DESP_APP_BIN="%ESP_APP_CMAKE%" -DESP_OTA_DATA_BIN="%ESP_OTA_DATA_CMAKE%" -DESP_PHY_INIT_BIN="%ESP_PHY_CMAKE%" -DESP_SYS_PARAM_BLANK_BIN="%ESP_BLANK_CMAKE%" -DRECOVERY_FORCE_FLASH=%RECOVERY_FORCE% -DRECOVERY_BOOT_MARKER="BOOT kesp-slave-klink-v2" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1

"%MAKE%" -C "%BUILD%" -j4 k210_esp_recovery_v2
if errorlevel 1 exit /b 1

if not exist "%BUILD%\k210_esp_recovery_v2.bin" (
  echo ERROR: recovery binary missing.
  exit /b 1
)
echo K210_ESP_RECOVERY_V2_BUILD_OK %BUILD%\k210_esp_recovery_v2.bin
exit /b 0
