param(
    [string]$OutDir = "sdcard\show"
)

Add-Type -AssemblyName System.Drawing

$Root = Split-Path $PSScriptRoot -Parent
$Faces = Join-Path $Root "docs\src\faces"
$Sounds = Join-Path $Root "docs\src\sound"
$Out = Join-Path $Root $OutDir
New-Item -ItemType Directory -Force $Out | Out-Null

$Emotions = @(
    "neutral", "happy", "laughing", "excited", "love",
    "curious", "surprised", "sad", "angry", "sleepy"
)

function Convert-Face($Src, $Dst) {
    $bmp0 = [System.Drawing.Bitmap]::FromFile($Src)
    try {
        if ($bmp0.Width -ne 320 -or $bmp0.Height -ne 240) {
            $bmp = New-Object System.Drawing.Bitmap 320, 240
            $g = [System.Drawing.Graphics]::FromImage($bmp)
            $g.DrawImage($bmp0, 0, 0, 320, 240)
            $g.Dispose()
        } else {
            $bmp = New-Object System.Drawing.Bitmap $bmp0
        }
    } finally {
        $bmp0.Dispose()
    }

    $bytes = New-Object byte[] (320 * 240 * 2)
    $i = 0
    for ($y = 0; $y -lt 240; $y++) {
        for ($x = 0; $x -lt 320; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $v = (($c.R -band 0xF8) -shl 8) -bor (($c.G -band 0xFC) -shl 3) -bor ($c.B -shr 3)
            $bytes[$i++] = [byte]($v -band 0xff)
            $bytes[$i++] = [byte](($v -shr 8) -band 0xff)
        }
    }
    $bmp.Dispose()
    [IO.File]::WriteAllBytes($Dst, $bytes)
}

function Read-U16LE($b, $i) { return [int]($b[$i] -bor ($b[$i + 1] -shl 8)) }
function Read-U32LE($b, $i) { return [int64]($b[$i] -bor ($b[$i + 1] -shl 8) -bor ($b[$i + 2] -shl 16) -bor ($b[$i + 3] -shl 24)) }

function Write-Envelope($Src, $Dst) {
    & py (Join-Path $PSScriptRoot "make_wav_env.py") $Src $Dst
    if ($LASTEXITCODE -ne 0) {
        throw "envelope generation failed for $Src"
    }
}

foreach ($emo in $Emotions) {
    Convert-Face (Join-Path $Faces "f_$emo.jpg") (Join-Path $Out "f_$emo.rgb")
    Convert-Face (Join-Path $Faces "f_${emo}_t.jpg") (Join-Path $Out "f_${emo}_t.rgb")
    Copy-Item (Join-Path $Sounds "e_$emo.wav") (Join-Path $Out "e_$emo.wav") -Force
    Write-Envelope (Join-Path $Sounds "e_$emo.wav") (Join-Path $Out "e_$emo.env")
    Write-Host ("{0,-10} ok" -f $emo)
}

Write-Host "wrote $Out"
Write-Host "Copy the contents of sdcard\ to the root of the microSD card."
