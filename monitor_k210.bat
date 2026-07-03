@echo off
setlocal EnableExtensions
cd /d "%~dp0"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "BAUD=%~2"
if "%BAUD%"=="" set "BAUD=921600"

echo === K210 UART monitor ===
echo Repo: %CD%
echo Port: %PORT%
echo Baud: %BAUD%
echo.

where py >nul 2>nul
if %ERRORLEVEL%==0 (
  set "PY=py -3"
) else (
  set "PY=python"
)

%PY% tools\monitor_k210.py --port %PORT% --baud %BAUD%
exit /b %ERRORLEVEL%
