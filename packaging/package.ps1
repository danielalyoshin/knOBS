# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Packages a Release build of knobs as an installer and a portable zip.

.DESCRIPTION
Takes knobs.exe from build\x64\Release (build it first: cmake --build --preset
release), or the exe given, and writes to build\package:
  <name>-<version>-setup.exe      the installer, built with Inno Setup 6
  <name>-<version>-portable.zip   the portable copy: a folder with knobs.exe and
                                  portable_mode.txt, which keeps its data beside it
  SHA256SUMS.txt                  the checksums of both
The name comes from src\app_info.h and the version from CMakeLists.txt. The
exe's own version has to match.

.PARAMETER Exe
The knobs.exe to package. By default, build\x64\Release's.

.PARAMETER Sign
Signs knobs.exe before it's packaged, and the installer and its uninstaller,
with SignScript.

.PARAMETER SignScript
What signs: a script that takes -Description, -Url and the files to sign. By
default, sign.ps1, which uses Azure Artifact Signing and says what it needs.

.PARAMETER Iscc
Inno Setup's compiler. By default, ISCC.exe on PATH or in Inno Setup 6's usual
folders.

.PARAMETER NoInstaller
Writes only the portable zip.
#>
param(
  [string]$Exe,
  [switch]$Sign,
  [string]$SignScript = "$PSScriptRoot\sign.ps1",
  [string]$Iscc,
  [switch]$NoInstaller
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$name = [regex]::Match((Get-Content -Raw "$root\src\app_info.h"), '#define KNOBS_DISPLAY_NAME "([^"]+)"').Groups[1].Value
$version = [regex]::Match((Get-Content -Raw "$root\CMakeLists.txt"), 'project\(\w+ VERSION (\d+\.\d+\.\d+)').Groups[1].Value
if (-not $name -or -not $version) { throw "Couldn't read the name from src\app_info.h or the version from CMakeLists.txt." }

$url = 'https://github.com/danielalyoshin/knobs'
if (-not $Exe) { $Exe = "$root\build\x64\Release\$name.exe" }
if (-not (Test-Path $Exe)) { throw "$Exe doesn't exist. Build it first: cmake --build --preset release" }
$exeVersion = (Get-Item $Exe).VersionInfo.ProductVersion
if ($exeVersion -ne $version) { throw "$Exe is version $exeVersion, but CMakeLists.txt says $version. Build again." }
if ($Sign -and -not (Test-Path $SignScript)) { throw "$SignScript doesn't exist." }

$out = "$root\build\package"
$stage = "$out\stage\$name"
Remove-Item -Recurse -Force "$out" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null

# The text files, with the name and version filled in, and Windows line endings.
function Write-Text([string]$from, [string]$to) {
  $text = (Get-Content -Raw $from) -replace '@NAME@', $name -replace '@VERSION@', $version
  [IO.File]::WriteAllText($to, ($text -replace "`r?`n", "`r`n"), [Text.UTF8Encoding]::new($false))
}
Copy-Item $Exe "$stage\$name.exe"
# Signed before it's packaged, so the zip and the installer carry the signed exe.
if ($Sign) { & $SignScript -Description $name -Url $url "$stage\$name.exe" }
Write-Text "$root\LICENSE" "$stage\LICENSE.txt"
Write-Text "$PSScriptRoot\NOTICE.txt" "$stage\NOTICE.txt"

# The portable zip: the same files, the marker, and one folder to unzip.
$portable = "$out\portable\$name"
New-Item -ItemType Directory -Force $portable | Out-Null
Copy-Item "$stage\*" $portable
Write-Text "$PSScriptRoot\portable_mode.txt" "$portable\portable_mode.txt"
$zip = "$out\$name-$version-portable.zip"
# Each entry named by hand: Windows PowerShell's Compress-Archive and .NET
# Framework's ZipFile write backslashes, where the zip format has forward
# slashes.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, [IO.Compression.ZipArchiveMode]::Create)
try {
  foreach ($file in Get-ChildItem -File $portable) {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, "$name/$($file.Name)",
      [IO.Compression.CompressionLevel]::Optimal)
  }
} finally {
  $archive.Dispose()
}
$artifacts = @($zip)

if (-not $NoInstaller) {
  if (-not $Iscc) {
    $found = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    $Iscc = if ($found) { $found.Source } else {
      @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    }
  }
  if (-not $Iscc -or -not (Test-Path $Iscc)) { throw "Couldn't find Inno Setup 6's ISCC.exe. Install it, or pass -Iscc." }
  # KnobsAppId: knobs's own AppId, which a test build without it doesn't get.
  $defines = @("/DAppName=$name", "/DKnobsAppId", "/DAppVersion=$version", "/DStageDir=$stage", "/DOutputDir=$out")
  if ($Sign) {
    # Inno Setup signs the uninstaller and the installer with this command:
    # $q is a quote and $f the quoted file.
    $defines += '/DSign', ('/Ssigntool=powershell.exe -NoProfile -ExecutionPolicy Bypass -File $q' + $SignScript +
      '$q -Description ' + $name + ' -Url ' + $url + ' $f')
  }
  & $Iscc /Q @defines "$PSScriptRoot\knobs.iss"
  if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed (exit code $LASTEXITCODE)." }
  $artifacts += "$out\$name-$version-setup.exe"
}

$sums = foreach ($file in $artifacts) {
  "{0}  {1}" -f (Get-FileHash -Algorithm SHA256 $file).Hash.ToLower(), (Split-Path -Leaf $file)
}
[IO.File]::WriteAllText("$out\SHA256SUMS.txt", (($sums -join "`n") + "`n"))
Remove-Item -Recurse -Force "$out\stage", "$out\portable"

foreach ($file in $artifacts + "$out\SHA256SUMS.txt") {
  "{0,-40} {1,8:N1} MB" -f (Split-Path -Leaf $file), ((Get-Item $file).Length / 1MB)
}
