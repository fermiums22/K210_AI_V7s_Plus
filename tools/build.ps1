param(
    [switch]$Flash,
    [string]$Port = "COM12"
)

$SDK   = "C:\K210\sdk\kendryte-freertos-sdk-0.7.0"
$TC    = "C:\K210\toolchain\kendryte-toolchain\bin"
$ROOT  = Split-Path $PSScriptRoot -Parent
$BUILD = "$ROOT\build"
$MAKE  = "$TC\mingw32-make.exe"
if (-not (Test-Path $MAKE)) {
    $cmd = Get-Command mingw32-make.exe -ErrorAction SilentlyContinue
    if ($cmd) { $MAKE = $cmd.Source }
}
if (-not (Test-Path $MAKE)) {
    $MAKE = "C:\msys64\mingw64\bin\mingw32-make.exe"
}

Set-Location $ROOT

# ── Configure ────────────────────────────────────────────────────────────────
Write-Host "[cmake] configuring..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force $BUILD | Out-Null
& cmake -S . -B $BUILD `
    -G "MinGW Makefiles" `
    "-DCMAKE_MAKE_PROGRAM=$MAKE" `
    "-DTOOLCHAIN=$TC" `
    "-DSDK_ROOT=$SDK" 2>&1
if ($LASTEXITCODE -ne 0) { Write-Error "cmake configure failed"; exit 1 }

# ── Build ────────────────────────────────────────────────────────────────────
Write-Host "[make] building..." -ForegroundColor Cyan
& "$MAKE" -C $BUILD -j4 2>&1
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

$BIN = "$BUILD\robot_show.bin"
Write-Host "[ok] binary: $BIN  ($([math]::Round((Get-Item $BIN).Length/1KB,1)) KB)" -ForegroundColor Green

# ── Flash ────────────────────────────────────────────────────────────────────
if ($Flash) {
    Write-Host "[flash] flashing to $Port ..." -ForegroundColor Yellow
    Write-Host "  Hold BOOT button, press RESET, release BOOT, then press Enter"
    Read-Host
    py -m kflash -p $Port -b 1500000 -B dan $BIN
    if ($LASTEXITCODE -ne 0) { Write-Error "flash failed"; exit 1 }
    Write-Host "[flash] done. Press RESET to boot." -ForegroundColor Green
}
