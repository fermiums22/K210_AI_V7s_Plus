@echo off
setlocal
cd /d "%~dp0"
set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
echo === K210 stack diagnostic ===
echo Repo: %CD%
echo Port: %PORT%
echo.
git fetch origin --prune || exit /b 1
git checkout main || exit /b 1
git pull --ff-only origin main || exit /b 1
call build_k210.bat || exit /b 1
call flash_k210.bat %PORT% --no-build || exit /b 1
echo.
echo Flash done. Opening monitor. Logs: logs\k210_monitor_*.log
echo.
call monitor_k210.bat %PORT% 921600
