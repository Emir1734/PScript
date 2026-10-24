# Build and validate PScript from a fixed build directory (default: build-jit-final5).
# Records executable paths and SHA-256 so results map to the current link output.
param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\build-jit-final5"),
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release",
    [switch]$SkipBuild,
    [switch]$SkipBench
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = (Resolve-Path $BuildDir -ErrorAction SilentlyContinue)
if (-not $BuildDir) {
    Write-Error "Build directory not found. Run cmake -S `"$Root`" -B <dir> first."
}
$BinDir = Join-Path $BuildDir "bin\$Config"
if (-not (Test-Path $BinDir)) {
    $alt = Join-Path $BuildDir ("bin\" + $Config.ToUpper())
    if (Test-Path $alt) { $BinDir = $alt }
}

function Get-ExePath($name) {
    $p = Join-Path $BinDir $name
    if (Test-Path $p) { return $p }
    $p2 = Join-Path $BuildDir $name
    if (Test-Path $p2) { return $p2 }
    return $null
}

if (-not $SkipBuild) {
    Write-Host "== cmake build ($Config) =="
    cmake --build $BuildDir --config $Config -j 4 `
        --target pscript_jit_opt_tests pscript_jit_deopt_tests pscript_benchmark closure_nested_repro `
        pscript_vm_tests pscript_compiler_tests pscript_jit_tests pscript_stabilization_tests
}

$artifacts = @(
    "pscript_jit_opt_tests.exe",
    "pscript_jit_deopt_tests.exe",
    "pscript_benchmark.exe",
    "closure_nested_repro.exe",
    "pscript_vm_tests.exe",
    "pscript_compiler_tests.exe",
    "pscript_jit_tests.exe",
    "pscript_stabilization_tests.exe"
)

Write-Host "`n== Binary identity ($Config) =="
foreach ($a in $artifacts) {
    $path = Get-ExePath $a
    if (-not $path) {
        Write-Warning "missing: $a"
        continue
    }
    $hash = (Get-FileHash $path -Algorithm SHA256).Hash
    Write-Host "$a`n  path=$path`n  sha256=$hash"
}

Write-Host "`n== closure_nested repro =="
$repro = Get-ExePath "closure_nested_repro.exe"
if ($repro) {
    & $repro 25000
    if ($LASTEXITCODE -ne 0) { throw "closure_nested 25000 failed exit=$LASTEXITCODE" }
    & $repro 1100
    if ($LASTEXITCODE -ne 0) { throw "closure_nested 1100 failed exit=$LASTEXITCODE" }
}

Write-Host "`n== Test suites =="
$tests = @(
    (Get-ExePath "pscript_jit_opt_tests.exe"),
    (Get-ExePath "pscript_jit_deopt_tests.exe"),
    (Get-ExePath "pscript_vm_tests.exe"),
    (Get-ExePath "pscript_compiler_tests.exe"),
    (Get-ExePath "pscript_jit_tests.exe"),
    (Get-ExePath "pscript_stabilization_tests.exe")
)
foreach ($t in $tests) {
    if (-not $t) { continue }
    Write-Host "--- $([IO.Path]::GetFileName($t)) ---"
    & $t
    if ($LASTEXITCODE -ne 0) { throw "failed: $t exit=$LASTEXITCODE" }
}

if (-not $SkipBench) {
    $bench = Get-ExePath "pscript_benchmark.exe"
    if ($bench) {
        Write-Host "`n== Benchmark sample (num_lu_divmod, 5 runs) =="
        $slugs = @("jit.exec.num_lu_divmod", "jit.exec.loops", "jit.exec.closure_nested", "jit.exec.simple_calls")
        foreach ($slug in $slugs) {
            Write-Host "--- $slug ---"
            1..5 | ForEach-Object {
                & $bench --filter $slug --warmup 2 --iterations 5 2>&1 | Select-String "warm_steady"
            }
        }
    }
}

Write-Host "`nValidation finished OK."
