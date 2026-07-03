@echo off
setlocal
cd /d "%~dp0"

set "SDK=C:\K210\sdk\kendryte-freertos-sdk-0.7.0"
set "TC=C:\K210\toolchain\kendryte-toolchain\bin"
set "BUILD=%CD%\build"
set "MAKE=%TC%\mingw32-make.exe"
if not exist "%MAKE%" set "MAKE=C:\msys64\mingw64\bin\mingw32-make.exe"

echo === K210 build ===
echo Repo:  %CD%
echo TC:    %TC%
echo MAKE:  %MAKE%
echo BUILD: %BUILD%
echo.

if not exist "%TC%" (
  echo ERROR: K210 toolchain not found: %TC%
  exit /b 1
)
if not exist "%MAKE%" (
  echo ERROR: mingw32-make.exe not found
  exit /b 1
)
where cmake >nul 2>nul
if errorlevel 1 (
  echo ERROR: cmake not found in PATH
  exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"

echo [cmake] configuring...
cmake -S . -B "%BUILD%" -G "MinGW Makefiles" -DCMAKE_MAKE_PROGRAM="%MAKE%" -DTOOLCHAIN="%TC%" -DSDK_ROOT="%SDK%"
if errorlevel 1 (
  echo ERROR: cmake configure failed
  exit /b 1
)

echo [make] building...
"%MAKE%" -C "%BUILD%" -j4
if errorlevel 1 (
  echo ERROR: build failed
  exit /b 1
)

if not exist "%BUILD%\robot_show.bin" (
  echo ERROR: binary missing: %BUILD%\robot_show.bin
  exit /b 1
)

echo.
echo OK: %BUILD%\robot_show.bin
exit /b 0
