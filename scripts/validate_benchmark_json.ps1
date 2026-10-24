param(
    [Parameter(Mandatory)]
    [string]$JsonPath,
    [string]$ReferencePath = "D:\PScript\bench_setW_full_2026-10-18.json",
    [int]$ExpectedRecords = 624
)

$ErrorActionPreference = "Stop"
$doc = Get-Content $JsonPath -Raw | ConvertFrom-Json
$results = @($doc.results)
$issues = @()
if ($results.Count -ne $ExpectedRecords) {
    $issues += "record_count=$($results.Count) expected=$ExpectedRecords"
}
$bad = $results | Where-Object { $_.sample_count -lt 1 -or $_.median_ns -lt 0 }
if ($bad) { $issues += "invalid_samples=$($bad.Count)" }

$benchVer = $doc.benchmark_version
if ($null -ne $benchVer -and $benchVer -notin @(1, 2)) {
    $issues += "unsupported_benchmark_version=$benchVer"
}

$dupes = $results | Group-Object name, category, tier, phase | Where-Object { $_.Count -gt 1 }
if ($dupes) { $issues += "duplicate_keys=$($dupes.Count)" }

if (Test-Path $ReferencePath) {
    $ref = Get-Content $ReferencePath -Raw | ConvertFrom-Json
    $refKeys = $ref.results | ForEach-Object { "$($_.name)|$($_.category)|$($_.tier)|$($_.phase)" } | Sort-Object
    $newKeys = $results | ForEach-Object { "$($_.name)|$($_.category)|$($_.tier)|$($_.phase)" } | Sort-Object
    $missing = Compare-Object $refKeys $newKeys | Where-Object { $_.SideIndicator -eq '<=' }
    $extra = Compare-Object $refKeys $newKeys | Where-Object { $_.SideIndicator -eq '=>' }
    if ($missing) { $issues += "missing_vs_setW=$($missing.Count)" }
    if ($extra) { $issues += "extra_vs_setW=$($extra.Count)" }
}

if ($issues) {
    Write-Output "VALIDATION_FAIL $($issues -join '; ')"
    exit 1
}
$gateFail = @($results | Where-Object { $_.median_ns -ge 100000000 })
Write-Output "VALIDATION_PASS records=$($results.Count) warmup=$($doc.warmup) iterations=$($doc.iterations) above_100ms_gate=$($gateFail.Count)"
exit 0
