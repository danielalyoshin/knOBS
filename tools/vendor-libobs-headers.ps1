# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Vendors the libobs headers knOBS compiles against into third_party/libobs.

.DESCRIPTION
Copies the include closure of the headers knOBS uses (obs.h, util/base.h,
util/bmem.h) from obs-studio's libobs/ at the given tag. knOBS never links
libobs; the headers only supply declarations for the GetProcAddress
function table, so they must match the OBS version knOBS is tested against.

Also writes a stub for obsconfig.h, which obs-studio generates at build
time, and a README recording the tag and commit.

.PARAMETER Tag
obs-studio release tag, e.g. 32.2.2. Match the installed OBS version.

.PARAMETER Source
An existing obs-studio checkout of that tag. If omitted, the tag is
shallow-cloned into a temp folder.

.EXAMPLE
.\tools\vendor-libobs-headers.ps1 -Tag 32.2.2
#>
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [string]$Source
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $repoRoot 'third_party\libobs'
$entryHeaders = @('obs.h', 'util/base.h', 'util/bmem.h')
$generatedHeaders = @('obsconfig.h')

if (-not $Source) {
    $Source = Join-Path ([IO.Path]::GetTempPath()) "knobs-obs-studio-$Tag"
    if (-not (Test-Path (Join-Path $Source 'libobs\obs.h'))) {
        if (Test-Path $Source) { Remove-Item -Recurse -Force $Source }
        git clone --quiet --depth 1 --branch $Tag --filter=blob:none --sparse `
            https://github.com/obsproject/obs-studio.git $Source
        if ($LASTEXITCODE -ne 0) { throw "git clone of obs-studio $Tag failed" }
        git -C $Source sparse-checkout set libobs
        if ($LASTEXITCODE -ne 0) { throw 'git sparse-checkout failed' }
    }
}

# Windows PowerShell 5.1 turns redirected native stderr into a terminating
# error under 'Stop', which would hide the message below.
$ErrorActionPreference = 'Continue'
$checkedOutTag = git -C $Source describe --tags --exact-match HEAD 2>$null
$describeFailed = $LASTEXITCODE -ne 0
$ErrorActionPreference = 'Stop'
if ($describeFailed) {
    throw "$Source isn't checked out at a tag; expected $Tag"
}
if ($checkedOutTag -ne $Tag) {
    throw "$Source is at tag '$checkedOutTag', not $Tag"
}
$commit = git -C $Source rev-parse HEAD
$libobs = (Resolve-Path (Join-Path $Source 'libobs')).Path

# Walk quoted #includes. Resolve relative to the including file, then to the
# libobs root, which is how libobs's own include paths are set up.
$seen = @{}
$queue = New-Object System.Collections.Generic.Queue[string]
foreach ($h in $entryHeaders) { $queue.Enqueue($h) }
while ($queue.Count -gt 0) {
    $rel = $queue.Dequeue()
    if ($seen.ContainsKey($rel)) { continue }
    $seen[$rel] = $true
    $file = Join-Path $libobs $rel
    $dir = Split-Path -Parent $rel
    foreach ($line in [IO.File]::ReadAllLines($file)) {
        if ($line -notmatch '^\s*#\s*include\s+"([^"]+)"') { continue }
        $inc = $Matches[1]
        if ($generatedHeaders -contains $inc) { continue }
        $resolved = $null
        foreach ($base in @($dir, '')) {
            $candidate = if ($base) { Join-Path $base $inc } else { $inc }
            $full = [IO.Path]::GetFullPath((Join-Path $libobs $candidate))
            if (Test-Path -LiteralPath $full -PathType Leaf) {
                $resolved = $full.Substring($libobs.Length + 1).Replace('\', '/')
                break
            }
        }
        if (-not $resolved) { throw "$rel includes '$inc', which isn't in libobs/" }
        $queue.Enqueue($resolved)
    }
}

if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
foreach ($rel in ($seen.Keys | Sort-Object)) {
    $target = Join-Path $dest $rel
    New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $libobs $rel) -Destination $target
}

$utf8 = New-Object System.Text.UTF8Encoding $false

$obsconfig = @'
// SPDX-License-Identifier: GPL-2.0-or-later
// Stand-in for the obsconfig.h that obs-studio generates from
// libobs/obsconfig.h.in at build time. knOBS only uses declarations from the
// libobs headers, so none of the build paths or feature flags apply.
#pragma once

#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@
[IO.File]::WriteAllText((Join-Path $dest 'obsconfig.h'), $obsconfig.Replace("`r`n", "`n") + "`n", $utf8)

$headerList = ($seen.Keys | Sort-Object | ForEach-Object { "- ``$_``" }) -join "`n"
$readme = @"
# libobs headers (obs-studio $Tag)

Declarations knOBS compiles its ``GetProcAddress`` function table against.
knOBS never links libobs and never ships it; at runtime it loads the user's
installed ``obs.dll`` from a shadow copy.

- Source: https://github.com/obsproject/obs-studio/tree/$Tag/libobs
- Commit: ``$commit``
- License: GPL-2.0-or-later, as stated in each file.

Only the include closure of ``obs.h``, ``util/base.h`` and ``util/bmem.h`` is
vendored. ``obsconfig.h`` is a knOBS stub for the header obs-studio generates
at build time.

Don't edit these files. Regenerate them for another tag with:

``````powershell
.\tools\vendor-libobs-headers.ps1 -Tag <tag>
``````

Files:

$headerList
"@
[IO.File]::WriteAllText((Join-Path $dest 'README.md'), $readme.Replace("`r`n", "`n") + "`n", $utf8)

Write-Host "Vendored $($seen.Count) libobs headers from obs-studio $Tag ($commit) into $dest"
