param(
    [string]$JsonPath,
    [string]$StdoutLog,
    [string]$StderrLog,
    [string]$ExitFile,
    [string]$Exe,
    [int]$Warmup,
    [int]$Iterations
)
$ErrorActionPreference = "Stop"
$env:PSCRIPT_BENCH_PROGRESS = "1"

if (-not ("PScript.DetachedProcess" -as [type])) {
    Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace PScript {
    public static class DetachedProcess {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        public struct STARTUPINFO {
            public int cb;
            public string lpReserved;
            public string lpDesktop;
            public string lpTitle;
            public int dwX;
            public int dwY;
            public int dwXSize;
            public int dwYSize;
            public int dwXCountChars;
            public int dwYCountChars;
            public int dwFillAttribute;
            public int dwFlags;
            public short wShowWindow;
            public short cbReserved2;
            public IntPtr lpReserved2;
            public IntPtr hStdInput;
            public IntPtr hStdOutput;
            public IntPtr hStdError;
        }

        [StructLayout(LayoutKind.Sequential)]
        public struct PROCESS_INFORMATION {
            public IntPtr hProcess;
            public IntPtr hThread;
            public int dwProcessId;
            public int dwThreadId;
        }

        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern bool CreateProcess(
            string lpApplicationName,
            string lpCommandLine,
            IntPtr lpProcessAttributes,
            IntPtr lpThreadAttributes,
            bool bInheritHandles,
            uint dwCreationFlags,
            IntPtr lpEnvironment,
            string lpCurrentDirectory,
            ref STARTUPINFO lpStartupInfo,
            out PROCESS_INFORMATION lpProcessInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr hObject);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetExitCodeProcess(IntPtr hProcess, out uint lpExitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetHandleInformation(IntPtr hObject, uint dwMask, uint dwFlags);

        const uint CREATE_NEW_PROCESS_GROUP = 0x00000200;
        const uint CREATE_NO_WINDOW = 0x08000000;
        const uint CREATE_UNICODE_ENVIRONMENT = 0x00000400;
        const uint STARTF_USESTDHANDLES = 0x00000100;
        const uint HANDLE_FLAG_INHERIT = 0x00000001;
        const uint INFINITE = 0xFFFFFFFF;

        static IntPtr BuildEnvironment() {
            var entries = Environment.GetEnvironmentVariables();
            var sb = new StringBuilder();
            foreach (System.Collections.DictionaryEntry entry in entries) {
                sb.Append(entry.Key);
                sb.Append('=');
                sb.Append(entry.Value);
                sb.Append('\0');
            }
            sb.Append('\0');
            return Marshal.StringToHGlobalUni(sb.ToString());
        }

        public static int Run(string exe, string arguments, string workDir, string stdoutPath, string stderrPath, out int processId) {
            var stdout = System.IO.File.Create(stdoutPath);
            var stderr = System.IO.File.Create(stderrPath);
            IntPtr env = IntPtr.Zero;
            try {
                SetHandleInformation(stdout.SafeFileHandle.DangerousGetHandle(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                SetHandleInformation(stderr.SafeFileHandle.DangerousGetHandle(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
                var si = new STARTUPINFO();
                si.cb = Marshal.SizeOf(typeof(STARTUPINFO));
                si.dwFlags = (int)STARTF_USESTDHANDLES;
                si.hStdInput = IntPtr.Zero;
                si.hStdOutput = stdout.SafeFileHandle.DangerousGetHandle();
                si.hStdError = stderr.SafeFileHandle.DangerousGetHandle();
                env = BuildEnvironment();
                string cmd = "\"" + exe + "\" " + arguments;
                PROCESS_INFORMATION pi;
                // New process group ignores the Ctrl+C that the agent console sends when a tool call returns.
                uint flags = CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
                if (!CreateProcess(exe, cmd, IntPtr.Zero, IntPtr.Zero, true, flags, env, workDir, ref si, out pi)) {
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                }
                processId = pi.dwProcessId;
                CloseHandle(pi.hThread);
                WaitForSingleObject(pi.hProcess, INFINITE);
                uint code;
                if (!GetExitCodeProcess(pi.hProcess, out code)) {
                    CloseHandle(pi.hProcess);
                    throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
                }
                CloseHandle(pi.hProcess);
                unchecked { return (int)code; }
            } finally {
                if (env != IntPtr.Zero) Marshal.FreeHGlobal(env);
                stdout.Dispose();
                stderr.Dispose();
            }
        }
    }
}
"@
}

Remove-Item $StdoutLog, $StderrLog -ErrorAction SilentlyContinue
$workDir = Split-Path $JsonPath -Parent
$arguments = "--json `"$JsonPath`" --warmup $Warmup --iterations $Iterations"
$childPid = 0
$code = [PScript.DetachedProcess]::Run($Exe, $arguments, $workDir, $StdoutLog, $StderrLog, [ref]$childPid)
"exit_code=$code child_pid=$childPid finished_utc=$((Get-Date).ToUniversalTime().ToString('o'))" |
    Set-Content -Path $ExitFile -Encoding ascii
exit $code
