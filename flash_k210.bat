@echo off
setlocal EnableExtensions
cd /d "%~dp0"

chcp 65001 >nul
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "NO_BUILD=0"
set "KFLASH_BAUD=1500000"
shift /1

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="--no-build" (
  set "NO_BUILD=1"
  shift /1
  goto parse_args
)
if /I "%~1"=="--baud" (
  set "KFLASH_BAUD=%~2"
  shift /1
  shift /1
  goto parse_args
)
echo ERROR: unknown argument: %~1
exit /b 2
:args_done

set "BIN=%CD%\build\k210_kstream_slave_v2.bin"

echo === K210 flash ===
echo Repo: %CD%
echo Port: %PORT%
echo Baud: %KFLASH_BAUD%
echo.

rem Enable ANSI/VT escape handling in the current Windows console.  kflash uses
rem ANSI on both stdout and stderr, so enable both handles and set the registry
rem default for newly opened classic cmd windows too.
reg add HKCU\Console /v VirtualTerminalLevel /t REG_DWORD /d 1 /f >nul 2>nul
py -3 -c "import ctypes; k=ctypes.windll.kernel32; ENABLE=4; handles=(-11,-12); mode=ctypes.c_uint(); [k.SetConsoleMode(k.GetStdHandle(h), (k.GetConsoleMode(k.GetStdHandle(h), ctypes.byref(mode)) and (mode.value|ENABLE)) or (mode.value|ENABLE)) for h in handles]" >nul 2>nul

if "%NO_BUILD%"=="0" (
  call build_k210.bat
  if errorlevel 1 exit /b 1
) else (
  if not exist "%BIN%" (
    echo ERROR: %BIN% not found. Run build_k210.bat first.
    exit /b 1
  )
)

py -3 -m kflash --help >nul 2>nul
if errorlevel 1 (
  echo.
  echo ERROR: kflash is not installed for Windows Python.
  echo Run: py -3 -m pip install kflash
  exit /b 1
)

echo.
echo [ports] Available serial ports:
py -3 -m serial.tools.list_ports
if errorlevel 1 (
  echo [ports] pyserial list_ports failed, fallback to Windows mode command:
  mode
)

echo.
echo [ports] Checking selected port %PORT% ...
py -3 -c "import sys; import serial.tools.list_ports as lp; p=sys.argv[1].upper(); ports=[x.device.upper() for x in lp.comports()]; print('found=' + ', '.join(ports) if ports else 'found=none'); sys.exit(0 if p in ports else 2)" %PORT%
if errorlevel 1 (
  echo ERROR: selected port %PORT% was not found.
  echo Pick one of the COM ports listed above and run: flash_k210.bat COMx
  exit /b 1
)

echo.
echo [kflash] using DTR/RTS auto-reset/boot on %PORT% (-B dan).
echo [kflash] Do not hold BOOT/RESET manually unless auto-boot fails.
echo [kflash] flashing %BIN% to %PORT% at %KFLASH_BAUD% ...
py -3 -m kflash -p %PORT% -b %KFLASH_BAUD% -B dan "%BIN%"
if errorlevel 1 (
  echo ERROR: kflash failed
  echo If this is a sync/timeout error, check the COM port and try again.
  echo Only then try manual BOOT+RESET fallback.
  exit /b 1
)

echo.
echo OK: K210 flashed. If the app does not start automatically, press RESET once.
exit /b 0
