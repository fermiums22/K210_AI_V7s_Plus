@echo off
setlocal EnableExtensions
cd /d "%~dp0"

chcp 65001 >nul
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM8"

set "KFLASH_BAUD=1500000"
set "MONITOR_BAUD=115200"
set "MONITOR_SECONDS=45"

echo === K210 app slot0 FAST test ===
echo Repo:       %CD%
echo Port:       %PORT%
echo Flash baud: %KFLASH_BAUD%
echo Monitor:    %MONITOR_BAUD% %MONITOR_SECONDS%s
echo.
echo NOTE: no clean here. Existing build_app_slot0 is reused for incremental build.
echo.

git fetch origin main
if errorlevel 1 exit /b 1
git switch -f main
if errorlevel 1 exit /b 1
git reset --hard origin/main
if errorlevel 1 exit /b 1

if not exist build_app_slot0 (
  echo [build] build_app_slot0 missing: first run will configure/build once.
) else (
  echo [build] reusing build_app_slot0 for incremental build.
)

call build_k210_app_slot0.bat
if errorlevel 1 exit /b 1

call flash_k210_app_slot0.bat %PORT% --no-build --baud %KFLASH_BAUD% --monitor-baud %MONITOR_BAUD% --monitor-seconds %MONITOR_SECONDS%
exit /b %ERRORLEVEL%
