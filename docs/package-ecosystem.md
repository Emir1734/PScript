# Package & Ecosystem (V1)

Part of the **1.0 public contract** — see [Compatibility & API Freeze](COMPATIBILITY.md).

PScript projects use `pscript.toml` at the repository root:

```text
MyProject/
├── pscript.toml
├── pscript.lock
├── src/
│   └── main.phs
└── tests/
```

## Commands

```text
pscript init <name>
pscript add <package> [constraint]
pscript remove <package>
pscript install [--dev]
pscript update [package]
pscript sync
pscript audit
pscript package
pscript pack [--force]
pscript publish [--dry-run] [token]
pscript register <username>
pscript login <username>
pscript logout
pscript whoami
pscript search <query>
pscript list
pscript tree
pscript metadata
pscript lib new <name> [directory]
```

## Package identity

Community packages use scoped names:

```text
@username/package
```

Examples: `@emir/utils`, `@ahmet/utils`. The username is tied to the registry account (globally unique). The same package name may exist under different usernames. Legacy unscoped names remain valid for local/path workflows.

## Dependencies

- SemVer `MAJOR.MINOR.PATCH`
- Constraints: exact, `^`, `~`
- Sources: registry (default), `{ path = "..." }`, `{ git = "...", ref = "..." }`
- Installed copies live under `.pscript/deps/`
- `pscript.lock` records resolved versions and SHA-256 directory checksums
- No install scripts or post-install hooks

## Imports

`import foo` is unchanged. The module resolver searches the dependency cache after local files and before stdlib modules.

For a dependency package, a bare `import <name>` loads the package **library module**, not the application scaffold:

1. Read `[project].name` from the installed package’s `pscript.toml` (fall back to the lockfile dependency name).
2. Resolve `src/<project_name>.phs` under the install directory.
3. If that file is missing, fall back to `src/main.phs` (legacy layout).

Explicit submodules are unchanged: `import foo.main` resolves `src/main.phs`, and `import foo.util` resolves `src/util.phs`.

A published package may keep the default `src/main.phs` scaffold alongside `src/<project_name>.phs`; the library import always prefers the named module file when both exist. Use `main.phs` only as the **application entry** when you run the package as a program (`pscript run`, or the root project’s own `src/main.phs`).

## Registry

Set `PSCRIPT_REGISTRY` to the registry base URL (HTTPS in production) or to a **local directory** for offline dev (`packages/<name>/versions.json` for legacy names, `packages/<username>/<package>/versions.json` for scoped names).

Remote registry workflow (HTTP):

- `pscript register <username>` / `pscript login <username>` — prompts for password; stores a bearer token in `~/.pscript/registry-credentials.json` (or `%USERPROFILE%\.pscript\` on Windows).
- `pscript whoami` / `pscript logout` — session introspection and server-side revoke + local credential delete.
- `pscript add @username/package` or `@username/package@^1.0.0` — resolve scoped names against the remote search API.
- `pscript publish` — uploads a validated package archive when credentials or `PSCRIPT_PUBLISH_TOKEN` are set; `pscript publish --dry-run` still performs only local validation (no network).

Publish authentication precedence: explicit `pscript publish <token>` argument, then `PSCRIPT_PUBLISH_TOKEN`, then saved registry credentials.

For the Registry service architecture, authentication API, publish API, dev setup, security boundaries, and rollout phases, see [PScript Registry Architecture](architecture/pscript-registry.md).
