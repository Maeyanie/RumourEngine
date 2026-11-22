# RumourEngine DLL Build Script
# Configures (if needed) and builds the SKSE DLL using MSVC + Ninja + vcpkg.
#
# Usage:
#   .\BuildDLL.ps1              # Build only (incremental, auto-safe thread count)
#   .\BuildDLL.ps1 -Clean       # Delete build dir and reconfigure from scratch
#   .\BuildDLL.ps1 -Configure   # Force reconfigure without cleaning
#   .\BuildDLL.ps1 -Threads 8   # Override thread count (auto-capped by RAM)
#
# Dependencies come from the git registries pinned in vcpkg-configuration.json
# (microsoft/vcpkg + colorglass for commonlibsse-ng), so no local vcpkg clone is
# needed -- the vcpkg tool bundled with Visual Studio is sufficient. First run
# builds CommonLibSSE-NG from source (~8 min); after that it is served from the
# binary cache in %LOCALAPPDATA%\vcpkg\archives.

param(
    [switch]$Clean,
    [switch]$Configure,
    [int]$Threads = 0
)

$ErrorActionPreference = "Stop"
$SKSEDir = $PSScriptRoot

# ── Locate Visual Studio ──
# vswhere ships at a fixed location regardless of where VS itself was installed.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path $vswhere)) {
    Write-Host "ERROR: vswhere.exe not found -- is Visual Studio installed?" -ForegroundColor Red
    exit 1
}
$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (!$vsPath) {
    Write-Host "ERROR: No VS install with the C++ toolset found." -ForegroundColor Red
    exit 1
}
Write-Host "Visual Studio: $vsPath" -ForegroundColor Cyan

$CMake = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Ninja = "$vsPath\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$VcVars = "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
foreach ($tool in @($CMake, $Ninja, $VcVars)) {
    if (!(Test-Path $tool)) {
        Write-Host "ERROR: missing required tool: $tool" -ForegroundColor Red
        Write-Host "Install the 'C++ CMake tools for Windows' VS component." -ForegroundColor Yellow
        exit 1
    }
}

# ── Safe thread calculation ──
# CommonLibSSE-NG template-heavy compilation uses ~2-3 GB RAM per cl.exe instance.
# Running too many in parallel exhausts RAM and freezes the system.
$ramGB = [Math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$ramAvailGB = [Math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB)
$cpuCores = [Environment]::ProcessorCount

$ramSafeThreads = [Math]::Max(1, [Math]::Floor(($ramAvailGB - 8) / 3))
$cpuSafeThreads = [Math]::Max(1, [Math]::Floor($cpuCores / 4))

if ($Threads -le 0) {
    $Threads = [Math]::Min($ramSafeThreads, $cpuSafeThreads)
} else {
    $Threads = [Math]::Min($Threads, $ramSafeThreads)
}
$Threads = [Math]::Max(1, [Math]::Min(8, $Threads))

Write-Host "System: ${ramGB} GB RAM total, ${ramAvailGB} GB available, ${cpuCores} cores" -ForegroundColor Cyan
Write-Host "Threads: $Threads (RAM-safe: $ramSafeThreads, CPU-safe: $cpuSafeThreads)" -ForegroundColor Cyan

# vcpkg uses its own parallelism when building dependencies. Without this it
# defaults to all cores, which can wedge the machine on CommonLibSSE-NG.
$env:VCPKG_MAX_CONCURRENCY = $Threads

# Lower priority BEFORE configure -- vcpkg compiles heavy template code during
# the configure step, so all cl.exe children need to inherit BelowNormal.
$proc = [System.Diagnostics.Process]::GetCurrentProcess()
$originalPriority = $proc.PriorityClass
$proc.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal

try {
    # ── Import the MSVC environment ──
    # NOTE: vcvars64 sets VCPKG_ROOT to the VS-bundled vcpkg. That is intentional
    # here -- do not pre-set VCPKG_ROOT above this line expecting it to survive.
    cmd /c "`"$VcVars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($matches[1])" -Value $matches[2] -ErrorAction SilentlyContinue
        }
    }

    $buildDir = "$SKSEDir\build"

    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host "Cleaning build directory..." -ForegroundColor Cyan
        Remove-Item -Recurse -Force $buildDir
    }

    $needsConfigure = $Clean -or $Configure -or !(Test-Path "$buildDir\build.ninja")
    if ($needsConfigure) {
        Write-Host "Configuring CMake..." -ForegroundColor Cyan
        # Each -D must be one quoted token: PowerShell otherwise splits values
        # containing a dot, turning "=3.5" into "=3" plus a stray ".5" argument.
        $cmArgs = @(
            "-S", $SKSEDir,
            "-B", $buildDir,
            "-G", "Ninja",
            "-DCMAKE_MAKE_PROGRAM=$Ninja",
            "-DCMAKE_BUILD_TYPE=Release",
            "-DCMAKE_TOOLCHAIN_FILE=$($env:VCPKG_ROOT)\scripts\buildsystems\vcpkg.cmake",
            "-DVCPKG_TARGET_TRIPLET=x64-windows-static-md"
        )
        & $CMake @cmArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Host "ERROR: CMake configure failed" -ForegroundColor Red
            exit 1
        }
    }

    Write-Host "Building ($Threads threads, below-normal priority)..." -ForegroundColor Cyan
    & $CMake --build $buildDir --config Release -- "-j$Threads"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "ERROR: Build failed" -ForegroundColor Red
        exit 1
    }
}
finally {
    $proc.PriorityClass = $originalPriority
}

# ── Report output ──
$dll = Get-ChildItem "$SKSEDir\build" -Filter *.dll -File | Select-Object -First 1
if ($dll) {
    Write-Host "SUCCESS: $($dll.Name) ($($dll.Length) bytes)" -ForegroundColor Green
    if ($env:SKYRIM_MODS_FOLDER) {
        Write-Host "Deployed by CMake to `$env:SKYRIM_MODS_FOLDER" -ForegroundColor Green
    } else {
        Write-Host "Set SKYRIM_MODS_FOLDER to auto-deploy on build." -ForegroundColor Yellow
    }
} else {
    Write-Host "ERROR: no DLL produced" -ForegroundColor Red
    exit 1
}
