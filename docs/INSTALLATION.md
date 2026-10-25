# Installation

Official **1.0** platforms: **Windows x64** and **Linux x64**. Use **Release** builds only for production (Debug builds are for development).

## Quick path (pre-built archive)

1. Download `pscript-<version>-windows-x64.zip` or `pscript-<version>-linux-x64.zip` from the release assets.
2. Verify SHA-256 using the accompanying `.sha256` file.
3. Extract the archive.
4. Add the `bin` directory to your `PATH` (or run by full path).
5. Confirm:

```text
pscript --version
pscript run examples/hello/main.phs
```

Bundled layout:

```text
pscript-<version>-<platform>/
  bin/pscript
  bin/pscript_lsp
  LICENSE
  README.md
  CHANGELOG.md
  RELEASE_METADATA.json
  SHA256SUMS.txt
  docs/
  examples/
```

### Windows notes

- Binaries are built with MSVC and may require the [Visual C++ Redistributable](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist) matching your toolset.
- Native JIT is included for **x64** (`jit_backend_x64`).

### Linux notes

- Build on Linux x64 with CMake 3.20+, C++20, OpenSSL (for crypto/TLS stdlib), and SQLite (bundled amalgamation under `deps/sqlite3/` if system SQLite is absent).
- Run `scripts/package_release.sh` after a successful Release build.

## Build from source

```text
git clone <repository>
cd PScript
cmake -S . -B build/main
cmake --build build/main --config Release
```

Executables (MSVC multi-config, default tree `build/main`): `build/main/Release/pscript.exe`, `build/main/Release/pscript_lsp.exe`. On single-config generators: `build/main/pscript`, `build/main/pscript_lsp`.

Optional: run the RC validation gate on Windows:

```powershell
.\scripts\rc_gate.ps1
```

## VS Code extension

See [language/editor-integration.md](language/editor-integration.md). Point `pscript.languageServer.path` at `bin/pscript_lsp` from your install or build output.

## Package registry

Default registry URL can be overridden with `PSCRIPT_REGISTRY`. See [package-ecosystem.md](package-ecosystem.md).
