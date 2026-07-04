@echo off
setlocal EnableExtensions
cd /d "%~dp0\.."

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "BRANCH=%~2"
if "%BRANCH%"=="" set "BRANCH=main"

echo === Restore K210 project firmware ===
echo Repo:   %CD%
echo Port:   %PORT%
echo Branch: %BRANCH%
echo.

git fetch origin %BRANCH%
if errorlevel 1 exit /b 1

git switch -f %BRANCH%
if errorlevel 1 exit /b 1

git reset --hard origin/%BRANCH%
if errorlevel 1 exit /b 1

call build_k210.bat
if errorlevel 1 exit /b 1

call flash_k210.bat %PORT% --no-build
if errorlevel 1 exit /b 1

echo.
echo OK: project firmware restored from %BRANCH%.
exit /b 0
