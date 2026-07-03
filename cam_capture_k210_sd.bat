@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"

set "OUT=%~2"
if "%OUT%"=="" set "OUT=cam/capture.rgb565"

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

echo === CAM_CAPTURE K210 SD through KSD ===
echo Port: %PORT%
echo File: %OUT%
echo.

%PY% tools\ksd_command.py --port %PORT% --baud 921600 --command "CAM_CAPTURE %OUT%" --timeout 60
exit /b %ERRORLEVEL%
