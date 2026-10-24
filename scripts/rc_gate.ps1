#Requires -Version 5.1
<#
  Release Candidate validation gate (Windows): Release build + full CTest + optional package.
#>
param(
    [string] $BuildDir = 'build',
    [switch] $SkipPackage
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $RepoRoot
$BuildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $RepoRoot $BuildDir }

Write-Host '=== RC Gate: Configure ==='
cmake -S $RepoRoot -B $BuildPath

Write-Host '=== RC Gate: Release build (all default targets) ==='
cmake --build $BuildPath --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host '=== RC Gate: CTest (Release) ==='
ctest --test-dir $BuildPath -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host '=== RC Gate: Benchmark smoke (Release, jit filter) ==='
$Bench = Join-Path $BuildPath 'pscript_bench_release.exe'
if (Test-Path $Bench) {
    & $Bench --filter 'jit.exec.kernel' --warmup 0 --iterations 1 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Benchmark smoke failed' }
}

if (-not $SkipPackage) {
    Write-Host '=== RC Gate: Package Release artifacts ==='
    & (Join-Path $PSScriptRoot 'package_release.ps1') -Configuration Release -BuildDir $BuildDir -SkipBuild
}

Write-Host '=== RC Gate: PASSED ==='
