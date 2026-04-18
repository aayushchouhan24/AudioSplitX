param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$BuildDir = "build",
    [string]$OutDir = "dist"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-CMake {
    $cmd = Get-Command "cmake.exe" -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\17\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\17\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }

    throw "cmake.exe not found on PATH or Visual Studio locations."
}

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$buildPath = Join-Path $repoRoot $BuildDir
$outPath = Join-Path $repoRoot $OutDir

New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
New-Item -ItemType Directory -Force -Path $outPath | Out-Null

$cmake = Resolve-CMake

Write-Host "Configuring project..."
& $cmake -S $repoRoot -B $buildPath -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE"
}

Write-Host "Building audiosplitx-ui..."
& $cmake --build $buildPath --config $Configuration --target audiosplitx-ui
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

$appExe = Join-Path $buildPath "$Configuration\Audio Split-X.exe"
if (-not (Test-Path -LiteralPath $appExe)) {
    throw "Built app not found: $appExe"
}

$destExe = Join-Path $outPath "Audio Split-X.exe"
try {
    Copy-Item -LiteralPath $appExe -Destination $destExe -Force
    $finalOutput = $destExe
} catch {
    $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $fallbackExe = Join-Path $outPath "Audio Split-X-$timestamp.exe"
    Copy-Item -LiteralPath $appExe -Destination $fallbackExe -Force
    $finalOutput = $fallbackExe
    Write-Warning "Primary output is locked (likely running). Wrote fallback build to: $fallbackExe"
}

Write-Host ""
Write-Host "Software build created: $finalOutput"
