# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Renders an SVG into a Windows icon (.ico), as assets/knobs.ico is made.

.DESCRIPTION
Headless Edge draws the SVG at each size, once on black and once on white,
with 8x8 device pixels to each of the icon's. The two pictures give each
pixel's color and alpha exactly, whatever the renderer does with
transparency, and each 8x8 block is averaged into one pixel of the icon, so
edges are smooth where shapes meet each other as well as where they meet
the background. Sizes below 256 are stored as 32-bit bitmaps, which every
Windows API reads, and 256 as a PNG.

The default sizes are small icons (16 px at 100%, as in the tray) and large
ones (32 px, as in dialogs) at display scales from 100% to 300%, and 256 for
Explorer. 96 is also the notifications' icon at 100%.

Edge runs with a profile of its own in a temporary folder, so a running Edge
isn't touched.

.EXAMPLE
tools\render-icon.ps1 -Svg assets\knobs-app-icon.svg -Out assets\knobs.ico
#>
param(
    [Parameter(Mandatory = $true)][string]$Svg,
    [Parameter(Mandatory = $true)][string]$Out,
    [int[]]$Sizes = @(16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 256)
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class IconRender {
    // A square of a picture's pixels: BGRA, top-down.
    static byte[] Read(Bitmap bitmap, int left, int side) {
        BitmapData data = bitmap.LockBits(new Rectangle(left, 0, side, side), ImageLockMode.ReadOnly,
                                          PixelFormat.Format32bppArgb);
        byte[] pixels = new byte[side * side * 4];
        for (int y = 0; y < side; y++) {
            Marshal.Copy(new IntPtr(data.Scan0.ToInt64() + (long)y * data.Stride), pixels, y * side * 4, side * 4);
        }
        bitmap.UnlockBits(data);
        return pixels;
    }

    static byte Channel(double value) { return (byte)Math.Round(Math.Min(255.0, Math.Max(0.0, value))); }

    // The icon `size` pixels square at `left`, from the same pixels on black
    // and on white, drawn with `scale` x `scale` pixels to each of the icon's:
    // BGRA, top-down, straight alpha. Each pixel is its block's average,
    // taken premultiplied.
    public static byte[] Pixels(Bitmap onBlack, Bitmap onWhite, int left, int size, int scale) {
        int side = size * scale;
        byte[] black = Read(onBlack, left * scale, side);
        byte[] white = Read(onWhite, left * scale, side);
        byte[] pixels = new byte[size * size * 4];
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                double alpha = 0, b = 0, g = 0, r = 0;
                for (int sy = y * scale; sy < (y + 1) * scale; sy++) {
                    for (int sx = x * scale; sx < (x + 1) * scale; sx++) {
                        int i = (sy * side + sx) * 4;
                        // On black a pixel shows color * alpha; on white, that plus 1 - alpha.
                        double a = 1 - ((white[i] - black[i]) + (white[i + 1] - black[i + 1]) +
                                        (white[i + 2] - black[i + 2])) / (3 * 255.0);
                        alpha += Math.Min(1.0, Math.Max(0.0, a));
                        b += black[i];
                        g += black[i + 1];
                        r += black[i + 2];
                    }
                }
                int o = (y * size + x) * 4;
                pixels[o + 3] = Channel(alpha / (scale * scale) * 255);
                if (pixels[o + 3] > 0) {
                    pixels[o] = Channel(b / alpha);
                    pixels[o + 1] = Channel(g / alpha);
                    pixels[o + 2] = Channel(r / alpha);
                }
            }
        }
        return pixels;
    }
}
'@

$supersample = 8
$edge = @(
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw "Microsoft Edge isn't installed." }

$svgUri = ([System.Uri](Resolve-Path $Svg).Path).AbsoluteUri
$tag = "render-icon-" + [guid]::NewGuid()
$work = Join-Path ([System.IO.Path]::GetTempPath()) $tag
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

# The Edge processes this script started.
function Stop-Edge {
    Get-CimInstance Win32_Process -Filter "Name = 'msedge.exe'" |
        Where-Object { $_.CommandLine -like "*$tag*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

function Render([string]$background) {
    $images = for ($i = 0; $i -lt $Sizes.Count; $i++) {
        "<img src=""$svgUri"" style=""left:$($lefts[$i])px;width:$($Sizes[$i])px;height:$($Sizes[$i])px"">"
    }
    $page = Join-Path $work "$background.html"
    $html = "<!doctype html><html><head><style>html,body{margin:0;background:$background}" +
            "img{position:absolute;top:0}</style></head><body>$($images -join '')</body></html>"
    Set-Content -Path $page -Value $html -Encoding UTF8
    $png = Join-Path $work "$background.png"
    $arguments = @('--headless', '--disable-gpu', '--hide-scrollbars', "--force-device-scale-factor=$supersample",
                   "--user-data-dir=""$work\profile""", "--window-size=$width,$height",
                   "--screenshot=""$png""", ([System.Uri]$page).AbsoluteUri)
    $process = Start-Process -FilePath $edge -ArgumentList $arguments -PassThru -WindowStyle Hidden
    # Edge sometimes stays running after it has saved the picture, so it's
    # stopped once the picture has stopped growing.
    $deadline = (Get-Date).AddMinutes(2)
    $saved = -1
    while (-not $process.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 250
        if (Test-Path $png) {
            $length = (Get-Item $png).Length
            if ($length -gt 0 -and $length -eq $saved) { break }
            $saved = $length
        }
    }
    Stop-Edge
    if (-not (Test-Path $png)) { throw "Edge didn't save $png." }
    return [System.Drawing.Bitmap]::FromFile($png)
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
    if ($onBlack.Width -lt $width * $supersample -or $onBlack.Height -lt $height * $supersample -or
        $onWhite.Width -ne $onBlack.Width -or $onWhite.Height -ne $onBlack.Height) {
        throw "Edge drew $($onBlack.Width)x$($onBlack.Height) and $($onWhite.Width)x$($onWhite.Height), " +
              "not $($width * $supersample)x$($height * $supersample)."
    }
    $entries = for ($i = 0; $i -lt $Sizes.Count; $i++) {
        , (Entry ([IconRender]::Pixels($onBlack, $onWhite, $lefts[$i], $Sizes[$i], $supersample)) $Sizes[$i])
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
    Stop-Edge
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
