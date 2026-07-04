@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0\.."

set "PORT=%~1"
if "%PORT%"=="" set "PORT=COM12"
set "IMAGE=%~2"
set "KFLASH_BAUD=1500000"

if not "%IMAGE%"=="" goto image_ready

echo [maixpy] No image path provided. Searching local tree...
set "COUNT=0"
set "CAND="
for /f "delims=" %%F in ('dir /b /s *.bin *.kfpkg 2^>nul') do call :consider "%%F"

if "%COUNT%"=="0" (
  echo.
  echo ERROR: no MaixPy/MicroPython .bin/.kfpkg image found in this repo checkout.
  echo Put the demo firmware here, for example:
  echo   firmware\maixpy\maixpy.bin
  echo Then run:
  echo   tools\flash_maixpy_demo.bat %PORT% firmware\maixpy\maixpy.bin
  exit /b 1
)

if not "%COUNT%"=="1" (
  echo.
  echo ERROR: more than one candidate found. Run again with the exact image path:
  echo   tools\flash_maixpy_demo.bat %PORT% path\to\maixpy.bin
  exit /b 1
)

set "IMAGE=%CAND%"
goto image_ready

:consider
set "F=%~1"
set "FL=!F!"
set "MATCH=0"
echo !FL! | findstr /i /c:"maix" >nul && set "MATCH=1"
echo !FL! | findstr /i /c:"micropython" >nul && set "MATCH=1"
echo !FL! | findstr /i /c:"mpy" >nul && set "MATCH=1"
echo !FL! | findstr /i /c:"demo" >nul && set "MATCH=1"
if "!MATCH!"=="1" (
  set /a COUNT+=1
  set "CAND=!F!"
  echo   !COUNT!^) !F!
)
exit /b 0

:image_ready
if not exist "%IMAGE%" (
  echo ERROR: image not found: %IMAGE%
  exit /b 1
)

py -3 -m kflash --help >nul 2>nul
if errorlevel 1 (
  echo ERROR: kflash is not installed for Windows Python.
  echo Run: py -3 -m pip install kflash
  exit /b 1
)

echo === Flash MaixPy/MicroPython demo ===
echo Repo:  %CD%
echo Port:  %PORT%
echo Image: %IMAGE%
echo Baud:  %KFLASH_BAUD%
echo.
py -3 -m serial.tools.list_ports

echo.
echo [kflash] flashing demo image...
py -3 -m kflash -p %PORT% -b %KFLASH_BAUD% -B dan "%IMAGE%"
if errorlevel 1 (
  echo ERROR: kflash failed
  exit /b 1
)

echo.
echo OK: demo image flashed. Press RESET once if MaixPy does not start.
echo To run the camera demo over REPL:
echo   py -3 tools\run_maixpy_camera_demo.py --port %PORT%
exit /b 0
