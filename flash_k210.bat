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