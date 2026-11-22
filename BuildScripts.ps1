# RumourEngine Papyrus build: Source/Scripts/*.psc -> Output/Scripts/*.pex
#
# RumourEngine.psc holds only global natives and needs nothing beyond the
# vanilla scripts. The SKSE-extended headers still come first, as in
# FactionEngine's build: the game's own Data/Source/Scripts copies are plain
# vanilla and would hide the SKSE additions if they won.

$ErrorActionPreference = "Stop"
$Root = $PSScriptRoot
$SourceDir = "$Root\Source\Scripts"
$OutputDir = "$Root\Output\Scripts"
$Game = "G:\SteamLibrary\steamapps\common\Skyrim Special Edition"
$Compiler = "$Game\Papyrus Compiler\PapyrusCompiler.exe"
$Toolkit = "D:\Devel\spookys-automod-toolkit\spookys-automod-toolkit"

if (!(Test-Path $Compiler)) {
    Write-Host "ERROR: Papyrus compiler not found at $Compiler (the Creation Kit must be installed)" -ForegroundColor Red
    exit 1
}

$imports = @(
    "$Toolkit\skyrim-script-headers"
    "$Game\Data\Source\Scripts"
    $SourceDir
)
$missing = $imports | Where-Object { !(Test-Path $_) }
if ($missing) {
    Write-Host "ERROR: missing import directories:" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$failed = @()
foreach ($psc in Get-ChildItem $SourceDir -Filter "*.psc") {
    Write-Host "Compiling $($psc.BaseName)..." -ForegroundColor Cyan
    & $Compiler $psc.FullName -import="$($imports -join ';')" -output="$OutputDir" -flags="TESV_Papyrus_Flags.flg"
    if ($LASTEXITCODE -ne 0) { $failed += $psc.BaseName }
}
if ($failed.Count -gt 0) {
    Write-Host "FAILED: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "SUCCESS: scripts compiled to $OutputDir" -ForegroundColor Green
