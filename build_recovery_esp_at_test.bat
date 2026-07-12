@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "TC=%K210_TC%"
if "%TC%"=="" set "TC=C:\K210\toolchain\kendryte-toolchain\bin"
set "MAKE=%TC%\mingw32-make.exe"
if not exist "%MAKE%" set "MAKE=C:\msys64\mingw64\bin\mingw32-make.exe"
set "BUILD=%CD%\build_recovery_esp_at_test"
set "AT_BIN=%~1"
if "%AT_BIN%"=="" set "AT_BIN=%CD%\sdcard\esp_flash\esp8285_at.bin"
if not exist "%AT_BIN%" exit /b 2
set "AT_BIN_CMAKE=%AT_BIN:\=/%"
cmake -S firmware_v2\recovery_esp -B "%BUILD%" -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="%MAKE%" -DTOOLCHAIN="%TC%" -DRECOVERY_AT_TEST=ON -DESP_AT_FULL_BIN="%AT_BIN_CMAKE%" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
if errorlevel 1 exit /b 1
"%MAKE%" -C "%BUILD%" -j4 k210_esp_recovery_v2
if errorlevel 1 exit /b 1
if not exist "%BUILD%\k210_esp_recovery_v2.bin" exit /b 1
echo K210_ESP_AT_TEST_BUILD_OK %BUILD%\k210_esp_recovery_v2.bin
