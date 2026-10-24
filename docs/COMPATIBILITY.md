# PScript 1.0 Compatibility & API Freeze

This document defines the **stable public contract** for PScript at **`1.0.0`**, how **`1.x`** releases may evolve, and what is **not** part of the contract. It is grounded in the current repository implementation. Stable **`1.0.0`** uses an empty `PSCRIPT_VERSION_PRERELEASE` in `CMakeLists.txt`; pre-releases append `-rc.*` to the version string.

**API freeze does not stop development.** New backward-compatible language features, stdlib additions, and tooling improvements are allowed in **`1.x`** minor releases. Freeze means **documented public behavior** must not break without deprecation and a future major version.

There is **no Rust-style edition system**; source compatibility is maintained across **`1.x`** unless a symbol is explicitly deprecated and later removed in **`2.0.0`**.

---

## 1. Public contract surfaces (1.0)

### Language & source files

| Surface | Contract |
| -------- | -------- |
| Source extension | `.phs` (UTF-8 text) |
| Specification | `docs/LANGUAGE_SPEC.md` plus topic guides under `docs/language/` |
| Block syntax | **`:` indented blocks** and **`{ }` blocks** (same semantics where both are supported) |
| Comments | `#` line comments |
| Identifiers | ASCII letters, digits, `_` (see spec) |
| Keywords / operators | As listed in `LANGUAGE_SPEC` §3–§4 and `docs/language/operators.md` |
| Types & generics | `int`, `float`, `bool`, `string`, `any`, classes, structs, enums, interfaces, `list<T>`, `map<K,V>`, etc. — per spec and `docs/language/types.md` |
| Functions / classes / interfaces / struct / enum / match | Per spec and `docs/language/*.md` |
| Exceptions | Prelude types (`RuntimeException`, `TypeException`, …) and `try` / `catch` / `finally` / `throw` |
| Async / await / defer | Per spec; sync programs without scheduler behave synchronously |
| Modules | `import`, `from … import`, `as`; visibility `pub` / `public`, `prot` / `protected`, `priv` / `private` — `docs/language/modules.md` |

**Not public contract:** register bytecode layout, opcode numeric values, internal VM/JIT data structures, PGO profile binary layout beyond documented tooling flags, and compiler pass ordering.

### Runtime semantics

| Surface | Contract |
| -------- | -------- |
| Observable behavior | Program output, exceptions, and I/O side effects as defined by language + stdlib docs |
| VM execution | Reference semantics for all programs |
| JIT (when enabled) | Must match VM for the same bytecode and inputs; parity is guarded by `stabilization` tests |
| `process.exit(n)` | Host process exit code `n` |
| CLI unhandled runtime errors | Exit code **`3`** (see Developer CLI) |

### Standard library

| Surface | Contract |
| -------- | -------- |
| Module list & APIs | `docs/language/stdlib.md` (authoritative for public exports) |
| Resolution | User `.phs` file overrides stdlib module name; then package deps; then stdlib |
| Global built-ins | `print`, `input`, `type`, `abs`, `min`, `max`, `round`, `clamp`, etc. — per stdlib doc |

**Not public contract:** native C++ helper names, `Native*` sidecar layouts, internal module registration order.

### Packages & projects

| Surface | Contract |
| -------- | -------- |
| Manifest | Root `pscript.toml` — fields supported by `src/package/manifest.*` |
| Lockfile | `pscript.lock` — resolved packages + checksums (`src/package/lockfile.*`) |
| SemVer & constraints | `MAJOR.MINOR.PATCH`; `^`, `~`, exact — `docs/package-ecosystem.md` |
| Install layout | `.pscript/deps/` |
| Registry override | `PSCRIPT_REGISTRY`, publish token env vars — ecosystem doc |

### Developer CLI (`pscript` executable)

Authoritative command list and exit codes: **`docs/language/developer-cli.md`** (kept in sync with `src/cli/dev_cli.cpp` help text).

Public subcommands include: `run`, `check`, `init`, `add`, `remove`, `install`, `update`, `sync`, `package`, `publish`, `fmt`, `lint`, `repl`, and `profile` subcommands. Legacy **`pscript file.phs`** is equivalent to **`pscript run file.phs`**.

### Tooling (LSP / formatter / linter)

| Surface | Contract |
| -------- | -------- |
| Analysis API | `analyze_source`, `format_source`, `AnalysisSession` — `docs/language/code-intelligence.md` |
| LSP server | `pscript_lsp` — capabilities described in `docs/language/editor-integration.md` |
| Formatter | Layout-preserving indent/whitespace; preserves `:` vs `{` style |
| Linter v1 | Unused locals, unreachable code, empty expression statements |

**Not public contract:** LSP JSON-RPC message ordering beyond LSP spec, internal diagnostic code strings (may add new codes in minors).

---

## 2. Versioning & release policy

Semantic versioning applies to the **language distribution** (`pscript` / libraries), not to individual third-party packages.

| Version | Meaning |
| -------- | -------- |
| **`1.0.0`** | First stable public contract (this document + linked specs) |
| **`1.x.y`** | **Backward-compatible** evolution |
| **`1.x.0` (minor)** | New features, new stdlib symbols, new CLI flags/subcommands if existing behavior unchanged |
| **`1.x.y` (patch)** | Bug fixes, security fixes, documentation fixes; no intentional breaking changes |
| **`2.0.0` (major)** | Breaking changes allowed only after deprecation period (see below) |

Pre-**`1.0.0`** versions (e.g. **`0.1.0`**) are development releases; minor doc/code drift may occur, but this audit aligns docs and tests before tagging **`1.0.0`**.

---

## 3. Deprecation & breaking changes

### Deprecation rules (from **`1.0.0`** onward)

1. **No removal or intentional behavior change** of a documented public API in **`1.x`** without prior deprecation.
2. Deprecation must appear in **release notes** and, where applicable, **compiler or linter warnings** (future) or documentation strikethrough with replacement.
3. Deprecated APIs remain available for **at least one full minor series** (e.g. deprecated in **`1.3.0`**, removed no earlier than **`2.0.0`**).
4. Every deprecation must document **replacement** and **migration** (short example preferred).
5. **Security** or **correctness** fixes that change observable behavior may ship in patches; if user-visible, document as a breaking-fix exception in release notes (still avoid silent semantic changes when possible).

### What counts as a breaking change

- Removing or renaming a stdlib module export, keyword, or operator
- Changing documented exception type or message for a **specified** error case (prefer new exception subtype)
- Changing `pscript.toml` / `pscript.lock` required fields or meaning
- Changing CLI exit codes documented in `developer-cli.md`
- Changing `.phs` parse/compile behavior for programs that compiled on prior **`1.x`**
- JIT/VM observable divergence on valid programs

### What is **not** a breaking change

- New keywords (if unambiguous), new stdlib functions/modules, new CLI subcommands/flags
- New linter warnings
- Internal performance changes (JIT tiers, PGO) that preserve semantics
- Stricter compile errors for programs that were **already ill-typed** per spec but incorrectly accepted (document in release notes)

---

## 4. Compatibility test guard

Automated guards (do not duplicate unnecessarily):

| Area | Tests |
| ------ | ----- |
| Language syntax anchors | `compatibility` CTest (`tests/compatibility_test.cpp`) |
| CLI exit codes & subcommands | `cli` CTest (`tests/cli_test.cpp`) |
| Package manifest/lock/SemVer | `package` CTest (`tests/package_test.cpp`) |
| VM ↔ JIT parity | `stabilization` CTest (`tests/stabilization_test.cpp`) |
| Lexer/parser/compiler/VM | `lexer`, `parser`, `compiler`, `vm`, `runtime`, … |
| LSP | `lsp`, `lsp_server_stdio` |

When adding a **new public API** in **`1.x`**, add or extend tests in the suite above; update **`docs/language/stdlib.md`** or **`LANGUAGE_SPEC.md`** in the same change.

---

## 5. Audit notes (implementation vs docs)

Resolved in this freeze pass:

- **`docs/language/developer-cli.md`** — aligned with implemented `fmt`, `lint`, package commands, `init` layout, exit codes.
- **`docs/language/stdlib.md`** — package manager exists; stdlib doc corrected.
- **`docs/development/design-decisions.md`** — execution pipeline mentions optional JIT with VM parity expectation.
- **`docs/package-ecosystem.md`** — `init` usage aligned with CLI.
- **`LANGUAGE_SPEC.md`** — module/package wording updated to reflect V1 package manager.

Known non-contract / internal (do not freeze):

- Bytecode opcodes, register allocation details, JIT IR passes, profile JSON schema version fields beyond CLI `--pgo-profile` tooling.

---

## 6. References

- `docs/LANGUAGE_SPEC.md`
- `docs/language/` — language guides
- `docs/package-ecosystem.md`
- `docs/language/developer-cli.md`
- `docs/language/code-intelligence.md`
- `docs/language/editor-integration.md`
