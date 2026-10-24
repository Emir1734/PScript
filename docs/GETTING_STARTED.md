# Getting started

## Hello world

Create `main.phs`:

```phs
print("Hello, PScript!")
```

Run:

```text
pscript run main.phs
```

Legacy form (same behavior):

```text
pscript main.phs
```

## Project with packages

```text
pscript init myapp
cd myapp
pscript run src/main.phs
```

This creates `pscript.toml` and `src/main.phs`. Add dependencies with `pscript add` and `pscript install` — see [package-ecosystem.md](package-ecosystem.md).

## Learn the language

| Topic | Document |
|--------|-----------|
| Overview & grammar | [LANGUAGE_SPEC.md](LANGUAGE_SPEC.md) |
| Syntax (`:` and `{ }` blocks) | [language/syntax.md](language/syntax.md) |
| Types | [language/types.md](language/types.md) |
| Functions & classes | [language/functions.md](language/functions.md), [language/classes.md](language/classes.md) |
| Modules | [language/modules.md](language/modules.md) |
| Standard library | [language/stdlib.md](language/stdlib.md) |

## Tooling

| Task | Command / doc |
|------|----------------|
| Compile check | `pscript check file.phs` |
| Format | `pscript fmt [--write] file.phs` |
| Lint | `pscript lint file.phs` |
| REPL | `pscript repl` |
| CLI reference | [language/developer-cli.md](language/developer-cli.md) |

## Examples

See the [examples/](../examples/) directory in the repository and release archives (`examples/hello/main.phs`).

## Next steps

- [INSTALLATION.md](INSTALLATION.md) — platforms and binaries  
- [COMPATIBILITY.md](COMPATIBILITY.md) — 1.0 public contract and versioning  
- [TROUBLESHOOTING.md](TROUBLESHOOTING.md) — common issues  
