# Move gitignored bench/report/log files from repo root into artifacts/.
param(
    [string]$Root = ""
)

$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Split-Path $PSScriptRoot -Parent }

$benchDir = Join-Path $Root "artifacts\benchmarks"
$reportDir = Join-Path $Root "artifacts\reports"
$logDir = Join-Path $Root "artifacts\logs"
$scratchDir = Join-Path $Root "artifacts\scratch"
foreach ($dir in @($benchDir, $reportDir, $logDir, $scratchDir)) {
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
}

$keep = @(
    ".gitignore", "CHANGELOG.md", "CMakeLists.txt", "LICENSE", "README.md"
)

$moved = 0
Get-ChildItem -Path $Root -File -Force | ForEach-Object {
    if ($keep -contains $_.Name) { return }
    $name = $_.Name
    $dest = $null
    if ($name -match '^(bench_|bench\.csv)') { $dest = $benchDir }
    elseif ($name -match '_report(\.md|_data\.txt)$' -or $name -match '_final_report' -or $name -match '^optimization_reliability_' -or $name -match '^ultimate_quality_' -or $name -match '^pscript_1_0_' -or $name -match '^closure_nested_divmod_') { $dest = $reportDir }
    elseif ($name -match '\.(log|pid|pid\.meta|exit)$' -or $name -match '^_' -or $name -match '^tmp_' -or $name -eq 'test.phs' -or $name -match '^vm_tests_') { $dest = $logDir }
    elseif ($name -match '\.(cmd|json)$' -and $name -match '^bench_') { $dest = $benchDir }
    elseif ($name -match '^set_h_benchmark\.txt$') { $dest = $benchDir }
    else { return }

    $target = Join-Path $dest $name
    if (Test-Path $target) { Remove-Item -Force $target }
    Move-Item -LiteralPath $_.FullName -Destination $target
    $moved++
}

Write-Output "Moved $moved file(s) from repo root into artifacts/"
