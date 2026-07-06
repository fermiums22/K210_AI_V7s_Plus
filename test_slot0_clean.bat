@echo off
setlocal EnableExtensions
cd /d "%~dp0"

chcp 65001 >nul
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM8"

echo === K210 app slot0 CLEAN test ===
echo Repo: %CD%
echo Port: %PORT%
echo.
echo This removes build_app_slot0 before building. Use after CMake/linker/compile flag changes.
echo.

git fetch origin main
if errorlevel 1 exit /b 1
git switch -f main
if errorlevel 1 exit /b 1
git reset --hard origin/main
if errorlevel 1 exit /b 1

if exist build_app_slot0 (
  echo [clean] removing build_app_slot0 ...
  rmdir /s /q build_app_slot0
)

call build_k210_app_slot0.bat
if errorlevel 1 exit /b 1

call flash_k210_app_slot0.bat %PORT% --no-build --baud 1500000 --monitor-baud 115200 --monitor-seconds 45
exit /b %ERRORLEVEL%
