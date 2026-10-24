# Developer CLI (Tooling Bölüm 1)

The `pscript` executable is the developer entry point for running and checking programs. This document is part of the **1.0 public contract** (see [Compatibility & API Freeze](../COMPATIBILITY.md)).

## Commands

| Command | Description |
|---------|-------------|
| `pscript run <file.phs> [args...]` | Run a script (same as legacy `pscript <file.phs> [args...]`) |
| `pscript check <file.phs>` | Lex, parse, and compile without executing |
| `pscript init [name]` | Create a project (`pscript.toml`, `src/main.phs`) |
| `pscript add <pkg> [constraint]` | Add a dependency to `pscript.toml` |
| `pscript remove <pkg>` | Remove a dependency from manifest, lockfile, and `.pscript/deps/` |
| `pscript install [--dev]` | Resolve and install dependencies into `.pscript/deps/` |
| `pscript update [pkg]` | Re-resolve all dependencies, or one named package |
| `pscript sync` | Verify installed trees match `pscript.lock` (no network) |
| `pscript audit` | Read-only local dependency integrity audit |
| `pscript package` | Build a source package under `dist/` (unpacked tree) |
| `pscript pack [--force]` | Build `dist/<name>-<version>/package.tar.gz` for distribution |
| `pscript publish [--dry-run] [token]` | Publish to the registry, or validate only with `--dry-run` |
| `pscript register <username>` | Create a registry account (remote HTTP registry; password prompt) |
| `pscript login <username>` | Obtain a bearer token and save registry credentials locally |
| `pscript logout` | Revoke the server session and delete local registry credentials |
| `pscript whoami` | Print the username for the saved registry token |
| `pscript search <query>` | Search the remote registry package index |
| `pscript list` | List installed dependencies from `pscript.lock` (read-only) |
| `pscript tree` | Show dependency tree from the lockfile (read-only) |
| `pscript metadata` | Show manifest and lockfile metadata (no secrets) |
| `pscript lib new <name> [dir]` | Scaffold a library project (`src/<module>.phs`, no `main.phs`) |
| `pscript fmt [--write\|-w] <file>` | Layout-preserving formatter (indent/whitespace) |
| `pscript lint <file>` | Analysis + linter diagnostics |
| `pscript repl` | Minimal interactive REPL |
| `pscript profile run …` / `merge` / `report` | Runtime profiling and PGO artifacts |
| `pscript test` | Run project tests under `tests/` (see below) |
| `pscript doctor` | Read-only health check for the runtime and current project |
| `pscript explain <code>` | Short explanation of a diagnostic code (`lexer`, `parser`, `compile`, …) |

Global flags: `--help` / `-h`, `--version` / `-V`.

`pscript run` accepts `--pgo-profile <path>` to apply profile-guided bytecode optimizations before execution.

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | Success |
| `1` | Lexer/parser/compiler error (including `check`) |
| `2` | CLI usage error (unknown option, missing arguments) |
| `3` | Runtime / unhandled VM exception |

`process.exit(n)` from a script still returns `n`.

For `pscript test`, a failed assertion or runtime test error uses exit code `3`; compile errors in a test file use `1`. When no `.phs` files exist under `tests/`, or no `test_*` functions are found, the command exits with `2` (usage) so an empty tree is not reported as success.

`pscript doctor` exits `0` when there are no blocking errors (warnings are allowed). Manifest, lockfile, or dependency mismatches are `ERROR` and exit `3`. Doctor never installs packages or edits project files.

`pscript explain` exits `0` for a known code and `2` for missing or unknown codes.

## Project tests (`pscript test`)

From a project root (directory containing `pscript.toml`), `pscript test` discovers every `tests/**/*.phs` file, compiles each file, and runs functions whose names start with `test_` using the same discovery path as the `testing` standard library (`testing.run()`). Files without `test_*` functions are skipped with a note; if no tests run in any file, the command fails.

Example `tests/smoke.phs`:

```phs
import testing

func test_smoke():
    testing.equal(1, 1)
```

## Project layout

`pscript init` scaffolds a package-style project (manifest + `src/main.phs`). User modules and imports resolve relative to the project working directory and `.pscript/deps/` as described in [Package & Ecosystem](../package-ecosystem.md).

## Packaging (`pack` / `publish --dry-run`)

`pscript pack` stages `pscript.toml`, optional `README.md`, and `src/**/*.phs` (excluding `.pscript/`, `dist/`, and `.git/`), then writes `package.tar.gz` under `dist/<name>-<version>/`. If that archive already exists, the command fails unless you pass `--force`. On success it prints the archive path, package id, file count, and SHA-256 of the staged tree.

`pscript publish --dry-run` runs the same validation and staging checks as pack (without writing the archive to `dist/` permanently) and prints what would be published. It does **not** contact a registry or claim remote publish success.

Without `--dry-run`, `pscript publish` uploads to the registry URL in `PSCRIPT_REGISTRY` when a token is available (`pscript publish <token>`, `PSCRIPT_PUBLISH_TOKEN`, or credentials from `login`). Failures are reported with a non-zero exit code; there is no silent success path.

Example:

```text
pscript pack
# dist/my_lib-0.1.0/package.tar.gz
# my_lib@0.1.0  3 file(s)  sha256:...

pscript publish --dry-run
# Dry run only — no registry upload was performed.
```

Exit code `1` indicates manifest or package validation errors; `2` is reserved for CLI usage problems.

## Dependency management (`remove` / `update` / `sync` / `audit`)

`pscript remove <pkg>` re-resolves the project, then atomically updates `pscript.toml` and `pscript.lock` (separate replacements, not one OS transaction), and only then deletes install directories under `.pscript/deps/` that are no longer locked. If the lockfile update or prune step fails, the CLI attempts to restore the previous manifest and lockfile from an in-memory backup. Unknown package names exit `2`; resolution failures exit `1` before any metadata changes.

`pscript update` re-resolves dependencies using existing SemVer constraints (`install_project`). With no package name, all locked packages may change version; with `<pkg>`, that dependency must appear in the manifest and the command prints `old -> new` when possible.

`pscript sync` requires `pscript.lock`, verifies checksums and on-disk installs, and does not fetch from the network. Exit `1` if the lockfile is missing or verification fails.

`pscript audit` reports `ERROR`, `WARN`, and `INFO` lines for manifest/lockfile drift, missing installs, checksum mismatches, and unsafe install paths. It does not scan for CVEs or modify files. Blocking errors exit `3`; warnings alone exit `0`.

## Dependency inspection (`list` / `tree` / `metadata`)

`pscript list` and `pscript tree` read `pscript.lock` when present. Without a lockfile, `list` shows manifest declarations as not installed; `tree` marks direct dependencies as missing a lockfile. Neither command installs packages or uses the network.

`pscript metadata` prints non-sensitive manifest fields and a summary of locked packages (truncated checksums). It never prints environment tokens.

## Library projects (`lib new`)

```text
pscript lib new my_utils ./my_utils
```

Creates `pscript.toml`, `src/my_utils.phs` (library module with a starter `version()` function), and `tests/`. Refuses to overwrite an existing `pscript.toml`.

## Program CLI (`import cli`)

Scripts use the `cli` standard library module (`cli.args()`, `cli.option()`, …) for their own argument parsing. The developer CLI reuses the same `CliState` parser for subcommands and `--help` / `--version`; it does not replace the `cli` module API.
