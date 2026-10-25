# PScript

PScript is a general-purpose language: **`.phs` source → register bytecode → register VM**, with an optional **native x64 JIT**. Implementation: **C++20**, **CMake**.

**Current stable release:** `1.0.0`.

Local benchmark logs and reports belong under [`artifacts/`](artifacts/README.md), not the repository root.

## Quick start

```text
pscript --version
pscript run examples/hello/main.phs
```

See [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) and [docs/INSTALLATION.md](docs/INSTALLATION.md).

## Documentation

| Document | Description |
|----------|-------------|
| [GETTING_STARTED.md](docs/GETTING_STARTED.md) | First steps, projects, tooling |
| [INSTALLATION.md](docs/INSTALLATION.md) | Windows/Linux x64, binaries, build from source |
| [LANGUAGE_SPEC.md](docs/LANGUAGE_SPEC.md) | Language specification |
| [language/stdlib.md](docs/language/stdlib.md) | Standard library reference |
| [language/developer-cli.md](docs/language/developer-cli.md) | `pscript` CLI |
| [package-ecosystem.md](docs/package-ecosystem.md) | `pscript.toml`, lockfile, registry |
| [COMPATIBILITY.md](docs/COMPATIBILITY.md) | 1.0 API contract & versioning |
| [MIGRATION.md](docs/MIGRATION.md) | 0.1.x → 1.0, deprecation policy |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | Common issues |
| [CHANGELOG.md](CHANGELOG.md) | Release history |

## Build (developers)

```text
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Release artifacts (Windows x64):

```powershell
.\scripts\rc_gate.ps1
```

## Editor support

VS Code extension and `pscript_lsp` — [docs/language/editor-integration.md](docs/language/editor-integration.md).

## License

[MIT](LICENSE)
