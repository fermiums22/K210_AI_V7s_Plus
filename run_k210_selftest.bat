@echo off
cd /d "%~dp0"
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
echo === K210 SELFTEST ===
echo Port: %PORT% @ 921600
py -3 tools\ksd_cmd.py --port %PORT% --baud 921600 --connect-timeout 30 --cmd SELFTEST
exit /b %ERRORLEVEL%
