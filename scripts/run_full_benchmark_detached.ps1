# Launch full pscript_benchmark suite outside the caller job (no long foreground wait).
param(
    [string]$Root = "",
    [string]$JsonPath = "",
    [string]$StdoutLog = "",
    [string]$StderrLog = "",
    [string]$PidFile = "",
    [string]$ExitFile = "",
    [string]$Exe = "",
    [int]$Warmup = 2,
    [int]$Iterations = 7
)

if (-not $Root) { $Root = Split-Path $PSScriptRoot -Parent }
$ArtifactBench = Join-Path $Root "artifacts\benchmarks"
New-Item -ItemType Directory -Force -Path $ArtifactBench | Out-Null
if (-not $JsonPath) { $JsonPath = Join-Path $ArtifactBench "bench_baseline_full_2026-10-20.json" }
if (-not $StdoutLog) { $StdoutLog = Join-Path $ArtifactBench "bench_baseline_full_2026-10-20_stdout.log" }
if (-not $StderrLog) { $StderrLog = Join-Path $ArtifactBench "bench_baseline_full_2026-10-20_stderr.log" }
if (-not $PidFile) { $PidFile = Join-Path $ArtifactBench "bench_baseline_full_2026-10-20.pid" }
if (-not $ExitFile) { $ExitFile = Join-Path $ArtifactBench "bench_baseline_full_2026-10-20.exit" }
if (-not $Exe) {
    $Exe = Join-Path $Root "build\main\bin\RELEASE\pscript_benchmark.exe"
    if (-not (Test-Path $Exe)) { $Exe = Join-Path $Root "build\main\bin\Release\pscript_benchmark.exe" }
    if (-not (Test-Path $Exe)) { $Exe = Join-Path $Root "build\release\bin\RELEASE\pscript_benchmark.exe" }
}

$ErrorActionPreference = "Stop"
if (-not (Test-Path $Exe)) { throw "Missing benchmark exe: $Exe" }

$existing = Get-Process pscript_benchmark -ErrorAction SilentlyContinue
if ($existing) {
    throw "pscript_benchmark already running (PIDs: $($existing.Id -join ',')). Do not start a duplicate."
}

if (Test-Path $PidFile) {
    $oldPid = [int](Get-Content $PidFile -Raw).Trim()
    if (Get-Process -Id $oldPid -ErrorAction SilentlyContinue) {
        throw "PID file points to active process $oldPid"
    }
}

Remove-Item $ExitFile -ErrorAction SilentlyContinue
$startUtc = (Get-Date).ToUniversalTime().ToString("o")

# cmd.exe performs the redirection. CREATE_NEW_PROCESS_GROUP ignores the Ctrl+C
# (exit 0xC000013A) the agent console delivered to the earlier Start-Process run.
$cmdPath = [System.IO.Path]::ChangeExtension($PidFile, ".cmd")
@(
    "@echo off"
    "setlocal"
    "set PSCRIPT_BENCH_PROGRESS=1"
    "`"$Exe`" --json `"$JsonPath`" --warmup $Warmup --iterations $Iterations 1>`"$StdoutLog`" 2>`"$StderrLog`""
    "echo exit_code=%ERRORLEVEL%> `"$ExitFile`""
    "exit /b %ERRORLEVEL%"
) | Set-Content -Path $cmdPath -Encoding ascii

if (-not ("PScript.GroupProcess" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
namespace PScript {
    public static class GroupProcess {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct STARTUPINFO {
            public int cb; public string lpReserved; public string lpDesktop; public string lpTitle;
            public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
            public short wShowWindow, cbReserved2; public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
        }
        [StructLayout(LayoutKind.Sequential)]
        public struct PROCESS_INFORMATION { public IntPtr hProcess, hThread; public int dwProcessId, dwThreadId; }
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool CreateProcess(string app, string cmd, IntPtr pa, IntPtr ta, bool inherit, uint flags, IntPtr env, string dir, ref STARTUPINFO si, out PROCESS_INFORMATION pi);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        public static int Start(string commandLine, string workDir) {
            var si = new STARTUPINFO();
            si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            PROCESS_INFORMATION pi;
            string cmdExe = System.IO.Path.Combine(Environment.SystemDirectory, "cmd.exe");
            uint flags = 0x00000200 | 0x08000000; // CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW
            if (!CreateProcess(cmdExe, commandLine, IntPtr.Zero, IntPtr.Zero, false, flags, IntPtr.Zero, workDir, ref si, out pi))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            int pid = pi.dwProcessId;
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return pid;
        }
    }
}
"@
}
$cmdExe = Join-Path $env:SystemRoot "System32\cmd.exe"
$commandLine = "`"$cmdExe`" /d /c `"$cmdPath`""
$createdPid = [PScript.GroupProcess]::Start($commandLine, $Root)
$created = [pscustomobject]@{ ProcessId = $createdPid; ReturnValue = 0 }

$created.ProcessId | Set-Content -Path $PidFile -Encoding ascii -NoNewline
@(
    "started_utc=$startUtc"
    "pid=$($created.ProcessId)"
    "exe=$Exe"
    "json=$JsonPath"
    "stdout=$StdoutLog"
    "stderr=$StderrLog"
    "exit=$ExitFile"
    "warmup=$Warmup"
    "iterations=$Iterations"
    "command=$commandLine"
) | Set-Content -Path ($PidFile + ".meta") -Encoding utf8

Write-Output "started pid=$($created.ProcessId) json=$JsonPath"
