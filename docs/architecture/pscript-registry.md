# PScript Registry — Architecture

This document describes the PScript package Registry: design, security boundaries, and implementation status through **Phase 5** (verification and hardening).

---

## Implementation status

| Phase | Scope | Status |
|-------|--------|--------|
| **1–3** | Architecture, SQLite service, auth & authorization | Done |
| **4** | Remote publish, CLI auth, bundled Argon2 | Done |
| **5** | Hardening, credential tests, E2E workflow, PostgreSQL (libpq), deployment docs | Done (see limitations) |

The Registry is suitable for **local development and staged production** behind TLS and a reverse proxy. It is not a turnkey public SaaS without operational hardening (see [deployment guide](../deployment/registry-production.md)).

---

## Technology

| Layer | Choice | Notes |
|--------|--------|--------|
| Service | `pscript_registry` + `pscript_registry_server` | C++20 |
| HTTP | Runtime sync TCP + HTTP parser | Plain HTTP on loopback in dev |
| Database (dev) | **SQLite** (`sqlite:<path>`) | Default; data preserved across migrations |
| Database (prod) | **PostgreSQL** (`postgres://` / `postgresql://`) | When built with **libpq** (`PSCRIPT_HAS_LIBPQ`) |
| Passwords | **Argon2id** via bundled [libargon2](../../deps/argon2_upstream/ATTRIBUTION.md) | Not dependent on OpenSSL Argon2 KDF |
| Sessions | Bearer `pscr_*` tokens; SHA-256 hash in DB | Revocation + expiry |
| Blobs | Sharded files under storage root | Path confinement; server-generated keys |

---

## Authentication & JSON

- Auth bodies parsed with escape-aware JSON string extraction (`package/json_util`).
- Client register/login bodies use proper JSON escaping (quotes, backslashes, Unicode).
- Password policy: 12–128 chars, no ASCII controls (tab/newline rejected).
- HTTP header lookup is case-insensitive (`Authorization` / `authorization`).

---

## Credential storage (CLI)

- Path: `%USERPROFILE%\.pscript\registry-credentials.json` or `~/.pscript/registry-credentials.json`.
- Override for tests: `PSCRIPT_REGISTRY_CREDENTIALS_FILE`.
- Stores **token + username + registry URL** only (not passwords). **Not** OS keychain.
- Atomic write via `write_file_atomically`; Unix attempts mode `600`.
- Load requires non-empty `token` and `username`.

---

## Publish, integrity, recovery

- Immutable versions: unique `(package_id, version)`; HTTP **409** on duplicate.
- Publish order: auth → ownership → body validation → archive extract + directory checksum → blob write → DB insert.
- On DB insert failure after blob write, blob file is removed; valid blobs are not deleted on DB failure elsewhere.
- Startup: `list_storage_keys` + `remove_unreferenced_blobs` reconciles orphan files (deterministic; does not delete referenced blobs).
- Download/read: DB row without blob returns error (no silent success).

---

## Rate limiting

- In-memory per client key; bounded buckets (8192) and entries per bucket (256).
- Not distributed — use proxy rate limits in production.

---

## PostgreSQL

- Enabled when CMake finds libpq.
- Same schema semantics as SQLite (migrations 001–002, Postgres dialect).
- Integration tests: `pscript_registry_postgres_tests` requires `PSCRIPT_REGISTRY_POSTGRES_TEST_URL` pointing at a database whose name contains `test` or `integration` (never run against production DBs).

---

## HTTPS / deployment

- Registry listens on loopback by default; TLS at Caddy/Nginx (see [deploy/registry/Caddyfile](../../deploy/registry/Caddyfile) and [registry-production.md](../deployment/registry-production.md)).
- `PSCRIPT_REGISTRY_TRUST_FORWARDED_CLIENT=1` only behind a trusted proxy.

---

## Tests

| Target | Purpose |
|--------|---------|
| `pscript_registry_tests` | HTTP router, auth, publish, integration workflow |
| `pscript_package_tests` | Includes `registry_credentials_test` |
| `pscript_registry_postgres_tests` | Real PostgreSQL (env-gated) |

**Skipped / platform gaps**

- `remove metadata rollback` in `pscript_package_tests`: **SKIP on Windows** — read-only `pscript.lock` does not reliably force `write_file_atomically` to fail; rollback path is exercised on Unix in the same test.
- `registry_postgres`: skips (exit 0) when `PSCRIPT_REGISTRY_POSTGRES_TEST_URL` is unset; re-run CMake configure after setting the URL so CTest passes it via `ENVIRONMENT`.

```powershell
ctest --test-dir out\Debug -C DEBUG -R "registry|package"
```

---

## Known limitations (pre–public production)

- SQLite single-node; Postgres HA/replication not documented here
- No distributed rate limits or account lockout
- File-based credentials (no HSM/keychain)
- Residual crash windows between blob write and DB commit mitigated but not zero
- Formal security audit not performed

---

## Related docs

- [Package & Ecosystem](../package-ecosystem.md)
- [Production deployment](../deployment/registry-production.md)
- [Developer CLI](../language/developer-cli.md)
