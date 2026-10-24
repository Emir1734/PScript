# Troubleshooting

## `pscript` not found

Add the `bin` directory from your extracted release (or `build/` after compiling) to `PATH`, or invoke by full path.

## Exit code `1` (compile error)

`pscript check file.phs` prints lexer/parser/compiler diagnostics with line and column. Fix the reported source location.

## Exit code `2` (CLI usage)

Run `pscript --help`. Subcommands (`run`, `check`, `init`, …) require documented arguments — see [language/developer-cli.md](language/developer-cli.md).

## Exit code `3` (runtime)

Unhandled exceptions print `Unhandled exception:` on stderr. Use `try` / `catch` in scripts or fix the underlying logic.

## JIT / performance

- JIT is enabled by default when the x64 backend is available. Observable behavior must match the VM; if you suspect a JIT bug, compare with a build where JIT is disabled (development builds) and file an issue with a minimal `.phs` reproducer.
- Loop-heavy optimizing tier may use conservative IR (see [CHANGELOG.md](../CHANGELOG.md) RC notes).

## Windows: MSVC `LNK1104` (cannot open `.exe` for writing)

Typical causes:

1. **A test or benchmark process still has the executable open** — close stray `pscript_*` runs you started from an older build tree; do not kill unrelated user processes.
2. **Linking the same path from parallel jobs** — use one build directory per workspace (`cmake -S . -B build-jit-final5`) and avoid copying benchmarks into the build root under alternate names.

Release builds place **test and benchmark executables** under `build-<name>/bin/Release/` (or `bin/RELEASE/` on MSVC). The `pscript` CLI remains in the build root. Rebuild and validate:

```powershell
cmake -S D:\PScript -B D:\PScript\build-jit-final5
cmake --build D:\PScript\build-jit-final5 --config Release --target pscript_jit_opt_tests
.\scripts\pscript_validate.ps1 -BuildDir D:\PScript\build-jit-final5
```

Record the printed SHA-256 before trusting benchmark numbers.

## LSP / VS Code

- Ensure `pscript_lsp` matches the `pscript` version (`pscript --version`).
- Set `pscript.languageServer.path` if auto-detection fails — [language/editor-integration.md](language/editor-integration.md).

## Packages

- Run commands from the project root (directory containing `pscript.toml`).
- After editing dependencies, run `pscript install` or `pscript sync`.
- Lockfile checksum mismatches: `pscript install` to refresh `.pscript/deps/`.

## Windows: missing DLL

Install the latest **Visual C++ Redistributable (x64)** for your MSVC version.

## Linux build: OpenSSL

Linux Release builds require OpenSSL development packages (`libssl-dev` on Debian/Ubuntu). Windows uses optional OpenSSL when found by CMake.

## Still stuck?

Collect: `pscript --version`, platform, command line, minimal `.phs` source, and full stderr output.
