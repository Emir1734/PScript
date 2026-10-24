# PScript 1.0.0-rc.1 Release Notes

**Release candidate — not the final 1.0.0 stable tag.**

## Verify your download

- File: `pscript-1.0.0-rc.1-<platform>.zip`
- Check `.sha256` sidecar before extracting.
- Inside the bundle: `SHA256SUMS.txt` lists individual files.

## Smoke test

```text
bin/pscript --version    → 1.0.0-rc.1
bin/pscript run examples/hello/main.phs
```

## RC policy

After **rc.1**, only bug fixes, security fixes, release-blocking corrections, documentation, and packaging changes — **no new language or stdlib features** until **1.0.0** final.

## Validation performed (Windows x64)

- Release configuration build
- Full CTest suite (Release)
- Benchmark smoke (`jit.exec.kernel`)
- `scripts/package_release.ps1` artifact layout + checksums

Linux x64 artifacts: build with `scripts/package_release.sh` on a Linux x64 host (not verified in environments without WSL/Linux).

## Documentation bundle

Included under `docs/` in the archive: installation, getting started, compatibility contract, CLI, stdlib reference, editor/LSP, troubleshooting, migration.

## VS Code

Extension source: `editors/vscode/` in the source distribution. Install from VSIX or F5 development workflow per `docs/language/editor-integration.md`.
