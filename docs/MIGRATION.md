# Migration & deprecation

PScript **1.0** public API rules are defined in [COMPATIBILITY.md](COMPATIBILITY.md).

## Migrating from 0.1.x development builds to 1.0

Development versions reported **`0.1.0`** in `pscript --version`. Release candidates and 1.0 use **`1.0.0-rc.*`** then **`1.0.0`**.

### Source (`.phs`)

No intentional breaking syntax changes between late **0.1.x** development snapshots and **1.0.0-rc.1**. Programs that compiled and ran on 0.1.x should continue to work. If something regresses, treat it as a release-blocking bug.

### CLI

- Prefer explicit `pscript run file.phs`; bare `pscript file.phs` remains supported.
- Package commands (`init`, `add`, `install`, …) are the supported project workflow; `init` creates `pscript.toml` + `src/main.phs`, not a single loose `main.phs` only.

### Documentation

- **fmt** and **lint** are implemented (exit code **4** for “not available” is obsolete).
- Package manager is part of 1.0; stdlib doc no longer implies “no package manager”.

### Version strings

Update scripts or tests that hard-coded `0.1.0`. `import sys; print(sys.version)` matches `pscript --version`.

## Deprecation policy (1.x)

- Deprecated APIs remain for at least one minor series; removal only in **2.0.0**.
- Deprecations include replacement guidance in release notes and docs.
- See [COMPATIBILITY.md](COMPATIBILITY.md) §3.

## Editions

PScript does **not** use Rust-style editions. Source compatibility is maintained across **1.x** unless an API is formally deprecated.
