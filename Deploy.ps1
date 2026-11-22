# Assemble the MO2 mod folder.
#
#   .\Deploy.ps1          # copy what is built
#   .\Deploy.ps1 -Build   # build the scripts and the DLL first
#
# The DLL can go out while Skyrim runs: Windows won't overwrite a DLL in use,
# but it will rename one, so the loaded copy is moved aside to
# RumourEngine.dll.old (SKSE loads only *.dll) and the new one takes effect at
# the next launch. Other files the game holds open are reported as locked and
# skipped, so everything else still goes out.

param(
    [switch]$Build,
    [string]$Target = "G:\MO2Data\SkyrimSE\mods\RumourEngine"
)

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot

if ($Build) {
    & "$Root\BuildScripts.ps1"
    if ($LASTEXITCODE -ne 0) { exit 1 }
    & "$Root\SKSE\BuildDLL.ps1"
    if ($LASTEXITCODE -ne 0) { exit 1 }
}

$files = @(
    @{ From = "$Root\SKSE\build\RumourEngine.dll"; To = "SKSE\Plugins\RumourEngine.dll" }
    @{ From = "$Root\Output\Scripts\RumourEngine.pex"; To = "Scripts\RumourEngine.pex" }
    @{ From = "$Root\Source\Scripts\RumourEngine.psc"; To = "Source\Scripts\RumourEngine.psc" }
    @{ From = "$Root\SKSE\include\RumourEngineAPI.h"; To = "SKSE\Plugins\RumourEngine\RumourEngineAPI.h" }
    @{ From = "$Root\SKSE\Plugins\RumourEngine\mills.ini"; To = "SKSE\Plugins\RumourEngine\mills.ini" }
    @{ From = "$Root\SKSE\Plugins\RumourEngine\gossips.ini"; To = "SKSE\Plugins\RumourEngine\gossips.ini" }
    @{ From = "$Root\SKSE\Plugins\RumourEngine\quests.ini"; To = "SKSE\Plugins\RumourEngine\quests.ini" }
    @{ From = "$Root\LICENSE"; To = "RumourEngine-LICENSE.txt" }
)
$layer = "$Root\SKSE\Plugins\SkyrimNet"
foreach ($f in Get-ChildItem $layer -Recurse -File) {
    $files += @{ From = $f.FullName; To = "SKSE\Plugins\SkyrimNet\" + $f.FullName.Substring($layer.Length + 1) }
}

$failed = 0
foreach ($f in $files) {
    $dest = Join-Path $Target $f.To
    if (!(Test-Path $f.From)) {
        Write-Host "  MISSING  $($f.From)" -ForegroundColor Red
        $failed++
        continue
    }
    if ((Test-Path $dest) -and (Get-FileHash $f.From).Hash -eq (Get-FileHash $dest).Hash) {
        Write-Host "  --  $($f.To)" -ForegroundColor DarkGray
        continue
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $dest -Parent) | Out-Null
    try {
        Copy-Item $f.From $dest -Force -ErrorAction Stop
        Write-Host "  ok  $($f.To)"
    } catch {
        # A loaded DLL: move it aside, in place of the last one moved aside unless the game
        # still holds that too (deployed twice in one run), then copy the new one in.
        $aside = $null
        if ($dest.EndsWith(".dll")) {
            foreach ($name in @("$dest.old", "$dest.$(Get-Date -Format yyyyMMdd-HHmmss).old")) {
                try {
                    if (Test-Path $name) { Remove-Item $name -Force -ErrorAction Stop }
                    Move-Item $dest $name -ErrorAction Stop
                    $aside = $name
                    break
                } catch {}
            }
        }
        if ($aside) {
            try {
                Copy-Item $f.From $dest -Force -ErrorAction Stop
                Write-Host "  ok  $($f.To) (the one in use is now $(Split-Path $aside -Leaf); the new one loads at the next launch)"
                continue
            } catch {
                Move-Item $aside $dest  # put the loaded one back rather than leave none
            }
        }
        Write-Host "  LOCKED  $($f.To)" -ForegroundColor Yellow
        $failed++
    }
}
if ($failed) { exit 1 }
Write-Host "Deployed to $Target" -ForegroundColor Green
