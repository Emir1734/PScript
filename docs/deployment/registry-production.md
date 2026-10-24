# PScript Registry — production deployment

This document describes patterns for running the Registry behind TLS. It does **not** by itself constitute a production deployment.

## Verification status (closure)

| Item | Status |
|------|--------|
| `deploy/registry/Caddyfile` syntax | Validate locally with `caddy validate --config deploy/registry/Caddyfile` when Caddy is installed |
| Local TLS (`registry.localhost`, `tls internal`) | **Not verified** in CI unless an operator runs Caddy + registry locally |
| Public production HTTPS | **Not verified** — requires your hostname, certificates, and firewall |
| Default CLI registry URL | `https://packages.pscript.dev/v1` (no silent HTTP downgrade for that host) |

## Components

| Component | Role |
|-----------|------|
| `pscript_registry_server` | C++ HTTP API (plain HTTP only) |
| Reverse proxy (Caddy or Nginx) | TLS termination, optional rate limits |
| PostgreSQL | Multi-node / production database (`postgres://` or `postgresql://` when built with libpq) |
| SQLite | Single-node development only (`sqlite:<path>`) |

## TLS and binding

- Keep the Registry bound to **loopback** (`PSCRIPT_REGISTRY_HOST=127.0.0.1`) unless explicitly testing LAN setups.
- Do **not** set `PSCRIPT_REGISTRY_ALLOW_PUBLIC_BIND=1` on the public Internet without a proxy and firewall plan.
- Terminate TLS at the proxy. CLI clients must use `PSCRIPT_REGISTRY=https://…`; the client does not silently downgrade to HTTP for non-localhost hosts.
- Example local TLS proxy: [deploy/registry/Caddyfile](../../deploy/registry/Caddyfile) (`caddy run --config deploy/registry/Caddyfile`).

## Trusted proxy headers

- `PSCRIPT_REGISTRY_TRUST_FORWARDED_CLIENT=1` enables rate-limit keys from `X-Forwarded-For`.
- Enable **only** when every untrusted client connects through a proxy that strips spoofed `X-Forwarded-For` and the proxy→registry hop is on a private network.
- Without this flag, rate limits use the registry bind host as the client key (safe default for direct localhost access).

## Secrets and credentials

- Store PostgreSQL credentials in the connection URL or environment available only to the registry service user.
- CLI bearer tokens live in `~/.pscript/registry-credentials.json` (file-based, not OS keychain).
- Never log `Authorization` headers or publish tokens.

## PostgreSQL

- Build with libpq (`PostgreSQL_FOUND` in CMake).
- Use a dedicated database user with least privilege.
- Run integration tests against a **dedicated** database whose name contains `test` or `integration`:

```powershell
$env:PSCRIPT_REGISTRY_POSTGRES_TEST_URL = "postgresql://postgres@localhost/pscript_registry_test"
cmake -S . -B out\Debug   # re-run so CTest embeds ENVIRONMENT for registry_postgres
ctest --test-dir out\Debug -C DEBUG -R registry_postgres
```

Without the URL at **configure** time, `registry_postgres` still runs but the executable **skips** (exit 0) when the variable is unset in the test process.

## Firewall

- Expose **443** (proxy) only.
- Block direct access to the registry port (8741) from untrusted networks.

## Remaining production gaps

- Distributed rate limiting and account lockout
- Full HA / replication runbooks
- Centralized audit logging and alerting
- Formal security review and penetration testing
