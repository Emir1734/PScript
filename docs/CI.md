# Continuous Integration

GitHub Actions workflows live under [`.github/workflows/`](../.github/workflows/).

## Jobs

| Job | Platform | Configuration | Tests |
|-----|----------|---------------|--------|
| `windows-release` | Windows x64 | MSVC, Release | CTest, label `-LE postgres` |
| `windows-debug` | Windows x64 | MSVC, Debug | CTest, label `-LE postgres` |
| `linux-release` | Ubuntu | GCC, Release | CTest, label `-LE postgres` |
| `linux-postgres` | Ubuntu + PostgreSQL 16 service | Release, libpq | CTest, label `-L postgres` |

PostgreSQL integration uses a disposable database whose name contains `test` (`pscript_registry_test`). The connection URL is supplied only through `PSCRIPT_REGISTRY_POSTGRES_TEST_URL` in the postgres job (never commit credentials).

The `lsp_server_stdio` CTest (subprocess LSP stdio harness) is registered on Windows only; Linux CI still runs the in-process `lsp` unit tests.

## Local commands

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build -LE postgres --output-on-failure
```

Windows (Visual Studio generator):

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release -LE postgres --output-on-failure
```

## Requirements

- CMake 3.20+
- C++20 compiler (MSVC 2022+, GCC 11+, Clang 11+)
- Linux: OpenSSL development headers (`libssl-dev`) for the runtime TLS stack
- Optional: PostgreSQL client (`libpq-dev`) for registry postgres tests
