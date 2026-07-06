@echo off
setlocal EnableExtensions
cd /d "%~dp0"

chcp 65001 >nul
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
set "NO_COLOR=1"
set "CLICOLOR=0"
set "PY_COLORS=0"
set "TERM=dumb"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM8"
set "NO_BUILD=0"
set "NO_MONITOR=0"
set "KFLASH_BAUD=1500000"
set "MONITOR_BAUD=115200"
set "MONITOR_SECONDS=20"
set "APP_SLOT_OFFSET=0x00100000"
shift /1

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--no-build" (
  set "NO_BUILD=1"
  shift /1
  goto parse_args
)
if /I "%~1"=="--no-monitor" (
  set "NO_MONITOR=1"
  shift /1
  goto parse_args
)
if /I "%~1"=="--baud" (
  set "KFLASH_BAUD=%~2"
  shift /1
  shift /1
  goto parse_args
)
if /I "%~1"=="--monitor-baud" (
  set "MONITOR_BAUD=%~2"
  shift /1
  shift /1
  goto parse_args
)
if /I "%~1"=="--monitor-seconds" (
  set "MONITOR_SECONDS=%~2"
  shift /1
  shift /1
  goto parse_args
)
echo ERROR: unknown argument: %~1
exit /b 2
:args_done

set "BIN=%CD%\build_app_slot0\k210_app_slot0.bin"
set "PKG=%CD%\build_app_slot0\k210_app_slot0.kfpkg"

echo === K210 app slot0 flash ===
echo Repo:   %CD%
echo Port:   %PORT%
echo Baud:   %KFLASH_BAUD%
echo Offset: %APP_SLOT_OFFSET%
echo Method: kfpkg flash-list address, NOT kflash -A
echo Monitor baud: %MONITOR_BAUD%
echo.

if "%NO_BUILD%"=="0" (
  call build_k210_app_slot0.bat
  if errorlevel 1 exit /b 1
) else (
  if not exist "%BIN%" (
    echo ERROR: %BIN% not found. Run build_k210_app_slot0.bat first.
    exit /b 1
  )
)

py -3 -m kflash --help >nul 2>nul
if errorlevel 1 (
  echo ERROR: kflash is not installed for Windows Python.
  echo Run: py -3 -m pip install kflash
  exit /b 1
)

py -3 tools\make_k210_slot_kfpkg.py --bin "%BIN%" --address %APP_SLOT_OFFSET% --out "%PKG%"
if errorlevel 1 exit /b 1

echo [kflash] flashing slot0 package at %APP_SLOT_OFFSET%...
py -3 -m kflash -p %PORT% -b %KFLASH_BAUD% -B dan "%PKG%"
if errorlevel 1 (
  echo.
  echo ERROR: K210 app slot0 kfpkg flash failed.
  exit /b 1
)

echo.
echo OK: K210 app slot0 flashed at %APP_SLOT_OFFSET%.

if "%NO_MONITOR%"=="0" (
  if exist "..\K210_AI_V7s_Plus_Boot\monitor_boot.bat" (
    echo.
    echo === K210 app/boot monitor ===
    call "..\K210_AI_V7s_Plus_Boot\monitor_boot.bat" %PORT% %MONITOR_BAUD% %MONITOR_SECONDS%
  ) else (
    echo monitor_boot.bat not found, skipping monitor.
  )
)

exit /b 0
