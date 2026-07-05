@echo off
cd /d "%~dp0"
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
if not exist logs mkdir logs
py -3 tools\ksd_cmd.py --port %PORT% --baud 921600 --connect-timeout 30 --cmd "CAM_CAPTURE capture.rgb565" --get capture.rgb565 --out logs\capture.rgb565
exit /b %ERRORLEVEL%
