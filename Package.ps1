# Build the release zip, Output\RumourEngine-<version>.zip, straight from the build: the files
# Deploy.ps1 ships, copied into an empty staging folder (Output\package\RumourEngine) and zipped
# from there, so nothing in the MO2 mod folder can slip in or go out stale.
#
#   .\Package.ps1           # package what is built
#   .\Package.ps1 -Build    # build the scripts and the DLL first
#
# The version is the layer manifest's. Entries use forward slashes, as mod managers expect.

param(
    [switch]$Build
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

$manifest = Get-Content "$Root\SKSE\Plugins\SkyrimNet\external\maeyanie.rumourengine\manifest.json" -Raw | ConvertFrom-Json
$version = $manifest.version

$stage = "$Root\Output\package\RumourEngine"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

$deployArgs = @{ Target = $stage }
if ($Build) { $deployArgs.Build = $true }
& "$Root\Deploy.ps1" @deployArgs
if ($LASTEXITCODE -ne 0) { exit 1 }

$out = "$Root\Output\RumourEngine-$version.zip"
if (Test-Path $out) { Remove-Item $out -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $out, [System.IO.Compression.CompressionLevel]::Optimal, $false)
Write-Host "Packaged $out ($([math]::Round((Get-Item $out).Length / 1KB)) KB)" -ForegroundColor Green
