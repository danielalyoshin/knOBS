# SPDX-License-Identifier: GPL-2.0-or-later
<#
.SYNOPSIS
Signs files with Azure Artifact Signing.

.DESCRIPTION
Signs each file through SignTool and Microsoft's Artifact Signing plugin, with
the certificate profile the environment names, timestamps it with Microsoft's
timestamp service, and checks the signature. It signs as whoever the Azure CLI
is signed in as: az login on a PC, azure/login in CI. package.ps1 -Sign calls
it for knobs.exe, and Inno Setup calls it for the uninstaller and the
installer.

Environment:
  ARTIFACT_SIGNING_ENDPOINT  The Artifact Signing account's regional endpoint,
                             such as https://eus.codesigning.azure.net
  ARTIFACT_SIGNING_ACCOUNT   The Artifact Signing account's name
  ARTIFACT_SIGNING_PROFILE   The certificate profile's name
  ARTIFACT_SIGNING_DLIB      The x64 Azure.CodeSigning.Dlib.dll, from the
                             Microsoft.ArtifactSigning.Client package
  SIGNTOOL                   Optional: signtool.exe. By default, the newest x64
                             one in the Windows 10 SDK.

.PARAMETER Description
The name Windows shows for the signed program, such as in a SmartScreen or UAC
prompt.

.PARAMETER Url
The program's web page, stored in the signature.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
  [string]$Description,
  [string]$Url,
  [Parameter(Mandatory, ValueFromRemainingArguments)][string[]]$Files
)

$ErrorActionPreference = 'Stop'

foreach ($name in 'ARTIFACT_SIGNING_ENDPOINT', 'ARTIFACT_SIGNING_ACCOUNT', 'ARTIFACT_SIGNING_PROFILE', 'ARTIFACT_SIGNING_DLIB') {
  if (-not [Environment]::GetEnvironmentVariable($name)) { throw "$name isn't set. packaging\sign.ps1 says what it should be." }
}
if (-not (Test-Path $env:ARTIFACT_SIGNING_DLIB)) { throw "ARTIFACT_SIGNING_DLIB is $env:ARTIFACT_SIGNING_DLIB, which doesn't exist." }

$signtool = $env:SIGNTOOL
if (-not $signtool) {
  $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.*\x64\signtool.exe" -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Directory.Parent.Name } | Select-Object -Last 1 -ExpandProperty FullName
}
if (-not $signtool -or -not (Test-Path $signtool)) { throw "Couldn't find signtool.exe. Install the Windows 10 SDK, or set SIGNTOOL." }

# Only the Azure CLI's sign-in, so the plugin doesn't wait on the others.
$metadata = @{
  Endpoint = $env:ARTIFACT_SIGNING_ENDPOINT
  CodeSigningAccountName = $env:ARTIFACT_SIGNING_ACCOUNT
  CertificateProfileName = $env:ARTIFACT_SIGNING_PROFILE
  ExcludeCredentials = @('EnvironmentCredential', 'WorkloadIdentityCredential', 'ManagedIdentityCredential',
    'SharedTokenCacheCredential', 'VisualStudioCredential', 'VisualStudioCodeCredential', 'AzurePowerShellCredential',
    'AzureDeveloperCliCredential', 'InteractiveBrowserCredential')
}
$metadataFile = Join-Path ([IO.Path]::GetTempPath()) "artifact-signing-$PID.json"
[IO.File]::WriteAllText($metadataFile, ($metadata | ConvertTo-Json))

try {
  $arguments = @('sign', '/fd', 'SHA256', '/tr', 'http://timestamp.acs.microsoft.com', '/td', 'SHA256',
    '/dlib', $env:ARTIFACT_SIGNING_DLIB, '/dmdf', $metadataFile)
  if ($Description) { $arguments += '/d', $Description }
  if ($Url) { $arguments += '/du', $Url }
  & $signtool @arguments @Files
  if ($LASTEXITCODE -ne 0) { throw "SignTool couldn't sign $($Files -join ', ') (exit code $LASTEXITCODE)." }
  foreach ($file in $Files) {
    & $signtool verify /pa /q $file
    if ($LASTEXITCODE -ne 0) { throw "$file has no valid signature after signing." }
  }
} finally {
  Remove-Item -LiteralPath $metadataFile -ErrorAction SilentlyContinue
}
