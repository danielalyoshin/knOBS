# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Renders an SVG into a Windows icon (.ico), as assets/knobs.ico is made.

.DESCRIPTION
Headless Edge draws the SVG at each size, once on black and once on white.
The two pictures give each pixel's color and alpha exactly, whatever the
renderer does with transparency. Sizes below 256 are stored as 32-bit
bitmaps, which every Windows API reads, and 256 as a PNG.

Edge runs with a profile of its own in a temporary folder, so a running Edge
isn't touched.

.EXAMPLE
tools\render-icon.ps1 -Svg assets\knobs-app-icon.svg -Out assets\knobs.ico
#>
param(
    [Parameter(Mandatory = $true)][string]$Svg,
    [Parameter(Mandatory = $true)][string]$Out,
    [int[]]$Sizes = @(16, 20, 24, 32, 40, 48, 64, 256)
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$edge = @(
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw "Microsoft Edge isn't installed." }

$svgUri = ([System.Uri](Resolve-Path $Svg).Path).AbsoluteUri
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("render-icon-" + [guid]::NewGuid())
New-Item -ItemType Directory $work | Out-Null

# Every size side by side, a gap apart, on one page.
$gap = 8
$lefts = @()
$x = 0
foreach ($size in $Sizes) {
    $lefts += $x
    $x += $size + $gap
}
$width = $x
$height = ($Sizes | Measure-Object -Maximum).Maximum

function Render([string]$background) {
    $images = for ($i = 0; $i -lt $Sizes.Count; $i++) {
        "<img src=""$svgUri"" style=""left:$($lefts[$i])px;width:$($Sizes[$i])px;height:$($Sizes[$i])px"">"
    }
    $page = Join-Path $work "$background.html"
    $html = "<!doctype html><html><head><style>html,body{margin:0;background:$background}" +
            "img{position:absolute;top:0}</style></head><body>$($images -join '')</body></html>"
    Set-Content -Path $page -Value $html -Encoding UTF8
    $png = Join-Path $work "$background.png"
    $arguments = @('--headless', '--disable-gpu', '--hide-scrollbars', '--force-device-scale-factor=1',
                   "--user-data-dir=""$work\profile""", "--window-size=$width,$height",
                   "--screenshot=""$png""", ([System.Uri]$page).AbsoluteUri)
    Start-Process -FilePath $edge -ArgumentList $arguments -Wait -WindowStyle Hidden
    if (-not (Test-Path $png)) { throw "Edge didn't save $png." }
    return [System.Drawing.Bitmap]::FromFile($png)
}

# BGRA, top-down, straight alpha, from the same pixels on black and white.
function Pixels($onBlack, $onWhite, [int]$left, [int]$size) {
    $pixels = New-Object 'byte[]' ($size * $size * 4)
    for ($y = 0; $y -lt $size; $y++) {
        for ($x = 0; $x -lt $size; $x++) {
            $b = $onBlack.GetPixel($left + $x, $y)
            $w = $onWhite.GetPixel($left + $x, $y)
            # On black a pixel shows color * alpha; on white, that plus 1 - alpha.
            $alpha = 1 - (($w.R - $b.R) + ($w.G - $b.G) + ($w.B - $b.B)) / (3 * 255.0)
            $alpha = [math]::Min(1, [math]::Max(0, $alpha))
            $i = ($y * $size + $x) * 4
            if ($alpha -gt 0) {
                $pixels[$i] = [byte][math]::Min(255, [math]::Round($b.B / $alpha))
                $pixels[$i + 1] = [byte][math]::Min(255, [math]::Round($b.G / $alpha))
                $pixels[$i + 2] = [byte][math]::Min(255, [math]::Round($b.R / $alpha))
            }
            $pixels[$i + 3] = [byte][math]::Round($alpha * 255)
        }
    }
    return , $pixels
}

# An icon entry: a 32-bit bitmap with its mask, bottom-up, or a PNG at 256.
function Entry([byte[]]$pixels, [int]$size) {
    $stream = New-Object System.IO.MemoryStream
    if ($size -ge 256) {
        $bitmap = New-Object System.Drawing.Bitmap $size, $size, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $rect = New-Object System.Drawing.Rectangle 0, 0, $size, $size
        $data = $bitmap.LockBits($rect, 'WriteOnly', $bitmap.PixelFormat)
        for ($y = 0; $y -lt $size; $y++) {
            [System.Runtime.InteropServices.Marshal]::Copy($pixels, $y * $size * 4,
                [IntPtr]($data.Scan0.ToInt64() + $y * $data.Stride), $size * 4)
        }
        $bitmap.UnlockBits($data)
        $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
        $bitmap.Dispose()
        return , $stream.ToArray()
    }
    $maskStride = [int][math]::Floor(($size + 31) / 32) * 4
    $writer = New-Object System.IO.BinaryWriter $stream
    $writer.Write([uint32]40); $writer.Write([int32]$size); $writer.Write([int32]($size * 2))
    $writer.Write([uint16]1); $writer.Write([uint16]32); $writer.Write([uint32]0)
    $writer.Write([uint32]($size * $size * 4 + $maskStride * $size))
    $writer.Write([int32]0); $writer.Write([int32]0); $writer.Write([uint32]0); $writer.Write([uint32]0)
    for ($y = $size - 1; $y -ge 0; $y--) { $writer.Write($pixels, $y * $size * 4, $size * 4) }
    for ($y = $size - 1; $y -ge 0; $y--) {
        $row = New-Object 'byte[]' $maskStride
        for ($x = 0; $x -lt $size; $x++) {
            if ($pixels[($y * $size + $x) * 4 + 3] -eq 0) { $row[[math]::Floor($x / 8)] = $row[[math]::Floor($x / 8)] -bor (0x80 -shr ($x % 8)) }
        }
        $writer.Write($row)
    }
    $writer.Flush()
    return , $stream.ToArray()
}

try {
    $onBlack = Render 'black'
    $onWhite = Render 'white'
    if ($onBlack.Width -lt $width -or $onBlack.Height -lt $height) {
        throw "Edge drew $($onBlack.Width)x$($onBlack.Height), not $($width)x$height."
    }
    $entries = for ($i = 0; $i -lt $Sizes.Count; $i++) {
        , (Entry (Pixels $onBlack $onWhite $lefts[$i] $Sizes[$i]) $Sizes[$i])
    }
    $onBlack.Dispose()
    $onWhite.Dispose()

    $file = New-Object System.IO.MemoryStream
    $writer = New-Object System.IO.BinaryWriter $file
    $writer.Write([uint16]0); $writer.Write([uint16]1); $writer.Write([uint16]$Sizes.Count)
    $offset = 6 + 16 * $Sizes.Count
    for ($i = 0; $i -lt $Sizes.Count; $i++) {
        $dimension = $Sizes[$i] % 256  # 0 means 256.
        $writer.Write([byte]$dimension); $writer.Write([byte]$dimension); $writer.Write([byte]0); $writer.Write([byte]0)
        $writer.Write([uint16]1); $writer.Write([uint16]32)
        $writer.Write([uint32]$entries[$i].Length); $writer.Write([uint32]$offset)
        $offset += $entries[$i].Length
    }
    foreach ($entry in $entries) { $writer.Write($entry) }
    $writer.Flush()
    [System.IO.File]::WriteAllBytes((Join-Path (Get-Location) $Out), $file.ToArray())
    Write-Output "Wrote $Out ($($Sizes -join ', ') px)."
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
