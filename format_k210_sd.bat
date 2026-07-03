@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo === FORMAT K210 SD through KSD ===
echo Port: %PORT%
echo WARNING: this destroys all files on the K210 SD card.
echo.

%PY% tools\ksd_command.py --port %PORT% --baud 921600 --command FORMAT_SD --timeout 180
exit /b %ERRORLEVEL%
