# Changelog

All notable user-facing changes are documented here. Versioning follows [Semantic Versioning](https://semver.org/). Public API rules: [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md).

## [Unreleased]

### Tooling

- Repository housekeeping: local bench/report/log outputs consolidated under `artifacts/`; `scripts/archive_root_artifacts.ps1` sweeps the repo root.
- VS Code extension **1.0.0** (aligned with PScript 1.0.0): grammar adds `defer`, `map`, `set`, `task`, `tuple` type names.

## [1.0.0] — 2025-10-02

First **stable** release. Same public contract and behavior as **1.0.0-rc.1**, with stable version metadata (`pscript --version` → `1.0.0`), final Windows x64 distribution artifacts, and package test isolation fix retained.

See [docs/release/RELEASE_NOTES-1.0.0.md](docs/release/RELEASE_NOTES-1.0.0.md).

## [1.0.0-rc.1] — 2025-10-02

First **Release Candidate** for PScript 1.0. No new language features after this tag; only bug fixes, security fixes, documentation, and packaging changes until **1.0.0** final.

### Highlights

- Stable public contract documented (`docs/COMPATIBILITY.md`).
- Register VM with optional native **x64 JIT** (baseline + optimizing tiers; VM parity guarded by tests).
- Developer CLI: `run`, `check`, `init`, package manager commands, `fmt`, `lint`, `repl`, `profile`.
- Standard library modules per `docs/language/stdlib.md`.
- Package projects: `pscript.toml`, `pscript.lock`, `.pscript/deps/`.
- LSP server (`pscript_lsp`) and VS Code extension under `editors/vscode/`.

### Distribution

- Windows x64 Release binaries via `scripts/package_release.ps1`.
- Linux x64 Release script: `scripts/package_release.sh` (build on Linux CI or host).

### Known RC limitations

- Optimizing JIT IR passes are skipped for loop-bearing functions (safe IR until loop-aware opts land); see JIT release notes in development docs.
- Pre-release version string: `1.0.0-rc.1` (`pscript --version`).

## [0.1.0] — development

Internal development series before the 1.0 contract freeze.
