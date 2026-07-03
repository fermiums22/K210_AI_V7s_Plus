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
echo Put K210 into ISP mode: hold BOOT, press RESET, release BOOT.
pause

echo [kflash] flashing %BIN% to %PORT% ...
py -3 -m kflash -p %PORT% -b 1500000 -B dan "%BIN%"
if errorlevel 1 (
  echo ERROR: kflash failed
  exit /b 1
)

echo.
echo OK. Press RESET on K210 to boot the new firmware.
exit /b 0
