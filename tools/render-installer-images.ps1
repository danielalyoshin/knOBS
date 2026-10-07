# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Renders the installer's wizard images from the logo's SVG masters.

.DESCRIPTION
Writes assets\installer\:
  wizard-<width>.png  the large image, on the "Completing setup" page: the
                      stacked lockup, centered on the knob's light graphite
                      (#e8eaee)
  small-<size>.png    the small image, top right on the other pages: the app
                      icon, on a transparent ground
at each size Inno Setup 6 asks for, from 100% to 250% display scaling. Setup
picks the one that fits.

As tools\render-icon.ps1 does: headless Edge draws each SVG once on black and
once on white, with 8x8 device pixels to each of the image's, and each block
is averaged into one pixel, with its alpha from the two pictures. Edge runs
with a profile of its own in a temporary folder, so a running Edge isn't
touched.

.EXAMPLE
tools\render-installer-images.ps1
#>
param([string]$Out = (Join-Path $PSScriptRoot '..\assets\installer'))

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class SvgRender {
    static byte[] Read(Bitmap bitmap, Rectangle rect) {
        BitmapData data = bitmap.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        byte[] pixels = new byte[rect.Width * rect.Height * 4];
        for (int y = 0; y < rect.Height; y++) {
            Marshal.Copy(new IntPtr(data.Scan0.ToInt64() + (long)y * data.Stride), pixels, y * rect.Width * 4, rect.Width * 4);
        }
        bitmap.UnlockBits(data);
        return pixels;
    }

    static byte Channel(double value) { return (byte)Math.Round(Math.Min(255.0, Math.Max(0.0, value))); }

    // The picture `width` x `height` at `left` (in the image's pixels), from
    // the same pixels on black and on white, drawn `scale` x `scale` to each
    // pixel: BGRA, top-down, straight alpha, each pixel its block's average.
    public static byte[] Pixels(Bitmap onBlack, Bitmap onWhite, int left, int width, int height, int scale) {
        Rectangle rect = new Rectangle(left * scale, 0, width * scale, height * scale);
        byte[] black = Read(onBlack, rect);
        byte[] white = Read(onWhite, rect);
        int side = width * scale;
        byte[] pixels = new byte[width * height * 4];
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                double alpha = 0, b = 0, g = 0, r = 0;
                for (int sy = y * scale; sy < (y + 1) * scale; sy++) {
                    for (int sx = x * scale; sx < (x + 1) * scale; sx++) {
                        int i = (sy * side + sx) * 4;
                        double a = 1 - ((white[i] - black[i]) + (white[i + 1] - black[i + 1]) +
                                        (white[i + 2] - black[i + 2])) / (3 * 255.0);
                        alpha += Math.Min(1.0, Math.Max(0.0, a));
                        b += black[i];
                        g += black[i + 1];
                        r += black[i + 2];
                    }
                }
                int o = (y * width + x) * 4;
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

    // `pixels` (width x height, straight alpha) drawn at (x, y) on a canvas
    // of `background`, or on a transparent one when `background` is null.
    public static Bitmap Compose(byte[] pixels, int width, int height, int canvasWidth, int canvasHeight, int x, int y,
                                 Color? background) {
        Bitmap canvas = new Bitmap(canvasWidth, canvasHeight, PixelFormat.Format32bppArgb);
        BitmapData data = canvas.LockBits(new Rectangle(0, 0, canvasWidth, canvasHeight), ImageLockMode.ReadWrite,
                                          PixelFormat.Format32bppArgb);
        byte[] out_ = new byte[canvasWidth * canvasHeight * 4];
        for (int i = 0; i < canvasWidth * canvasHeight; i++) {
            if (background.HasValue) {
                out_[i * 4] = background.Value.B; out_[i * 4 + 1] = background.Value.G;
                out_[i * 4 + 2] = background.Value.R; out_[i * 4 + 3] = 255;
            }
        }
        for (int py = 0; py < height; py++) {
            for (int px = 0; px < width; px++) {
                int cx = x + px, cy = y + py;
                if (cx < 0 || cy < 0 || cx >= canvasWidth || cy >= canvasHeight) continue;
                int s = (py * width + px) * 4, d = (cy * canvasWidth + cx) * 4;
                double a = pixels[s + 3] / 255.0, under = out_[d + 3] / 255.0;
                double alpha = a + under * (1 - a);
                for (int c = 0; c < 3; c++) {
                    out_[d + c] = alpha > 0 ? Channel((pixels[s + c] * a + out_[d + c] * under * (1 - a)) / alpha) : (byte)0;
                }
                out_[d + 3] = Channel(alpha * 255);
            }
        }
        Marshal.Copy(out_, 0, data.Scan0, out_.Length);
        canvas.UnlockBits(data);
        return canvas;
    }
}
'@

$supersample = 8
# Inno Setup 6's image areas at 100%, 125%, 150%, 175%, 200%, 225% and 250%.
$wizardSizes = @(@(202, 386), @(269, 515), @(336, 643), @(403, 772), @(430, 824), @(498, 953), @(534, 1022))
$smallSizes = @(58, 77, 97, 116, 124, 143, 159)
$panel = [System.Drawing.ColorTranslator]::FromHtml('#e8eaee')
# The lockup's width on the large image, and its center's height, as fractions of the image.
$lockup = "$root\assets\knobs-lockup-stacked-dark.svg"
$lockupWidth = 0.72
$lockupCenter = 0.44
$viewBox = [regex]::Match((Get-Content -Raw $lockup), 'viewBox="0 0 ([\d.]+) ([\d.]+)"')
$lockupAspect = [double]$viewBox.Groups[1].Value / [double]$viewBox.Groups[2].Value

$edge = @(
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw "Microsoft Edge isn't installed." }
$tag = "render-installer-" + [guid]::NewGuid()
$work = Join-Path ([System.IO.Path]::GetTempPath()) $tag
New-Item -ItemType Directory $work | Out-Null

function Stop-Edge {
    Get-CimInstance Win32_Process -Filter "Name = 'msedge.exe'" |
        Where-Object { $_.CommandLine -like "*$tag*" } |
        ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
}

# Draws `$boxes` (each @(width, height)) of one SVG side by side, a gap apart,
# on `$background`, and returns the bitmap and each box's left edge.
function Render([string]$svg, $boxes, [string]$background) {
    $uri = ([System.Uri](Resolve-Path $svg).Path).AbsoluteUri
    $gap = 8; $x = 0; $lefts = @(); $images = @()
    foreach ($box in $boxes) {
        $images += "<img src=""$uri"" style=""left:${x}px;width:$($box[0])px;height:$($box[1])px"">"
        $lefts += $x
        $x += $box[0] + $gap
    }
    $height = ($boxes | ForEach-Object { $_[1] } | Measure-Object -Maximum).Maximum
    $page = Join-Path $work "$([IO.Path]::GetFileNameWithoutExtension($svg))-$background.html"
    Set-Content -Path $page -Encoding UTF8 -Value ("<!doctype html><html><head><style>html,body{margin:0;background:$background}" +
        "img{position:absolute;top:0}</style></head><body>$($images -join '')</body></html>")
    $png = [IO.Path]::ChangeExtension($page, '.png')
    $arguments = @('--headless', '--disable-gpu', '--hide-scrollbars', "--force-device-scale-factor=$supersample",
                   "--user-data-dir=""$work\profile""", "--window-size=$x,$height",
                   "--screenshot=""$png""", ([System.Uri]$page).AbsoluteUri)
    $process = Start-Process -FilePath $edge -ArgumentList $arguments -PassThru -WindowStyle Hidden
    # Edge sometimes stays running after it has saved the picture.
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
    return @{ Bitmap = [System.Drawing.Bitmap]::FromFile($png); Lefts = $lefts }
}

function Render-Pixels([string]$svg, $boxes) {
    $black = Render $svg $boxes 'black'
    $white = Render $svg $boxes 'white'
    $pixels = for ($i = 0; $i -lt $boxes.Count; $i++) {
        , [SvgRender]::Pixels($black.Bitmap, $white.Bitmap, $black.Lefts[$i], $boxes[$i][0], $boxes[$i][1], $supersample)
    }
    $black.Bitmap.Dispose(); $white.Bitmap.Dispose()
    return , $pixels
}

try {
    New-Item -ItemType Directory -Force $Out | Out-Null

    $markBoxes = foreach ($size in $wizardSizes) {
        $w = [int][math]::Round($size[0] * $lockupWidth)
        , @($w, [int][math]::Round($w / $lockupAspect))
    }
    $marks = Render-Pixels $lockup $markBoxes
    for ($i = 0; $i -lt $wizardSizes.Count; $i++) {
        $width = $wizardSizes[$i][0]; $height = $wizardSizes[$i][1]
        $box = $markBoxes[$i]
        $x = [int][math]::Round(($width - $box[0]) / 2)
        $y = [int][math]::Round($height * $lockupCenter - $box[1] / 2)
        $image = [SvgRender]::Compose($marks[$i], $box[0], $box[1], $width, $height, $x, $y, $panel)
        $image.Save((Join-Path $Out "wizard-$width.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $image.Dispose()
    }

    $iconBoxes = foreach ($size in $smallSizes) { , @($size, $size) }
    $icons = Render-Pixels "$root\assets\knobs-app-icon.svg" $iconBoxes
    for ($i = 0; $i -lt $smallSizes.Count; $i++) {
        $size = $smallSizes[$i]
        $image = [SvgRender]::Compose($icons[$i], $size, $size, $size, $size, 0, 0, $null)
        $image.Save((Join-Path $Out "small-$size.png"), [System.Drawing.Imaging.ImageFormat]::Png)
        $image.Dispose()
    }
    Write-Output "Wrote $($wizardSizes.Count) wizard images and $($smallSizes.Count) small images to $Out."
} finally {
    Stop-Edge
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
