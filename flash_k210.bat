@echo off
setlocal
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "BIN=%CD%\build\robot_show.bin"

echo === K210 flash ===
echo Repo: %CD%
echo Port: %PORT%
echo.

call build_k210.bat
if errorlevel 1 exit /b 1

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
py -3 -c "import serial, sys; p=sys.argv[1]; ports=[x.device.upper() for x in serial.tools.list_ports.comports()]; print('found=' + ', '.join(ports) if ports else 'found=none'); sys.exit(0 if p.upper() in ports else 2)" %PORT%
if errorlevel 1 (
  echo ERROR: selected port %PORT% was not found.
  echo Pick one of the COM ports listed above and run: flash_k210.bat COMx
  exit /b 1
)

echo.
echo [kflash] using DTR/RTS auto-reset/boot on %PORT% (-B dan).
echo [kflash] Do not hold BOOT/RESET manually unless auto-boot fails.
echo [kflash] flashing %BIN% to %PORT% ...
py -3 -m kflash -p %PORT% -b 1500000 -B dan "%BIN%"
if errorlevel 1 (
  echo ERROR: kflash failed
  echo If this is a sync/timeout error, check the COM port and try again.
  echo Only then try manual BOOT+RESET fallback.
  exit /b 1
)

echo.
echo OK: K210 flashed. If the app does not start automatically, press RESET once.
exit /b 0