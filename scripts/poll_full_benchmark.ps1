# Short status check for detached full benchmark; writes exit code when process ends.
param(
    [string]$Root = "",
    [string]$PidFile = "",
    [string]$ExitFile = "",
    [string]$JsonPath = "",
    [string]$StderrLog = "",
    [int]$ExpectedRecords = 624
)

if (-not $Root) { $Root = Split-Path $PSScriptRoot -Parent }
if (-not $PidFile) { $PidFile = Join-Path $Root "bench_baseline_full_2026-10-20.pid" }
if (-not $ExitFile) { $ExitFile = Join-Path $Root "bench_baseline_full_2026-10-20.exit" }
if (-not $JsonPath) { $JsonPath = Join-Path $Root "bench_baseline_full_2026-10-20.json" }
if (-not $StderrLog) { $StderrLog = Join-Path $Root "bench_baseline_full_2026-10-20_stderr.log" }

$ErrorActionPreference = "SilentlyContinue"
if (-not (Test-Path $PidFile)) {
    Write-Output "state=no_pid_file"
    exit 2
}
$wrapperPid = [int](Get-Content $PidFile -Raw).Trim()
$benchProc = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -eq 'pscript_benchmark.exe' -and $_.CommandLine -like "*$JsonPath*" } |
    Select-Object -First 1
$proc = if ($benchProc) { Get-Process -Id $benchProc.ProcessId -ErrorAction SilentlyContinue }
        else { Get-Process -Id $wrapperPid -ErrorAction SilentlyContinue }
$benchPid = if ($benchProc) { $benchProc.ProcessId } else { $wrapperPid }
if ($proc -and ($benchProc -or $proc.ProcessName -eq 'powershell')) {
    $cpu = [math]::Round($proc.CPU, 1)
    $min = [math]::Round(((Get-Date) - $proc.StartTime).TotalMinutes, 1)
    $tail = @()
    if (Test-Path $StderrLog) {
        $tail = Get-Content $StderrLog -Tail 3 -ErrorAction SilentlyContinue
    }
    Write-Output "state=running pid=$benchPid cpu_sec=$cpu runtime_min=$min"
    if ($tail) { Write-Output ("progress_tail=" + ($tail -join " | ")) }
    exit 0
}

if (-not (Test-Path $ExitFile)) {
    Write-Output "state=not_running pid=$benchPid exit_file=missing"
    exit 3
}

$jsonOk = $false
$count = 0
if (Test-Path $JsonPath) {
    try {
        $doc = Get-Content $JsonPath -Raw | ConvertFrom-Json
        $count = @($doc.results).Count
        $jsonOk = $true
    } catch {
        $jsonOk = $false
    }
}
$exitLine = if (Test-Path $ExitFile) { (Get-Content $ExitFile -Raw).Trim() } else { "exit_code=unknown" }
Write-Output "state=finished pid=$benchPid $exitLine json_present=$jsonOk record_count=$count expected=$ExpectedRecords"
if ($jsonOk -and $count -eq $ExpectedRecords) { exit 0 }
if ($jsonOk -and $count -ne $ExpectedRecords) { exit 1 }
exit 3
