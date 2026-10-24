# PScript 1.0.0 Release Notes

**Stable release** — public API frozen per [COMPATIBILITY.md](../COMPATIBILITY.md).

## Verify your download

- File: `pscript-1.0.0-<platform>.zip`
- Verify SHA-256 using the `.sha256` sidecar before extracting.
- Bundle includes `SHA256SUMS.txt` for individual files.

## Smoke test

```text
bin/pscript --version    → 1.0.0
bin/pscript --help
bin/pscript run examples/hello/main.phs
```

## What's in 1.0.0

Everything documented for **1.0.0-rc.1** (language, register VM, x64 JIT, stdlib, package manager, CLI, LSP, formatter, linter) with stable versioning and final release artifacts.

## Platforms

- **Windows x64:** Pre-built Release zip in `dist/`.
- **Linux x64:** Build on Linux with `scripts/package_release.sh` (verify on your host).

## Upgrading from rc.1

No intentional API or behavior changes. Update binaries; `pscript --version` and `sys.version` report `1.0.0`.
