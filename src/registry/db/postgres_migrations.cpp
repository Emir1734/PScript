#include "registry/db/postgres_migrations.h"

#include <libpq-fe.h>

#include <ctime>
#include <sstream>
#include <string>
#include <vector>

namespace pscript::registry {
namespace {

struct Migration {
    int version;
    const char* sql;
};

constexpr const char* kMigration001 = R"sql(
CREATE TABLE IF NOT EXISTS schema_migrations (
  version INTEGER PRIMARY KEY,
  applied_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS users (
  id TEXT PRIMARY KEY,
  username TEXT NOT NULL,
  password_hash TEXT NOT NULL,
  created_at TEXT NOT NULL,
  disabled_at TEXT
);

CREATE UNIQUE INDEX IF NOT EXISTS idx_users_username_lower ON users (lower(username));

CREATE TABLE IF NOT EXISTS packages (
  id TEXT PRIMARY KEY,
  owner_user_id TEXT NOT NULL REFERENCES users(id),
  name TEXT NOT NULL,
  visibility TEXT NOT NULL DEFAULT 'public' CHECK (visibility IN ('public', 'private')),
  created_at TEXT NOT NULL,
  UNIQUE(owner_user_id, name)
);

CREATE INDEX IF NOT EXISTS idx_packages_owner ON packages(owner_user_id);

CREATE TABLE IF NOT EXISTS package_versions (
  id TEXT PRIMARY KEY,
  package_id TEXT NOT NULL REFERENCES packages(id),
  version TEXT NOT NULL,
  checksum_sha256 TEXT NOT NULL,
  archive_size BIGINT NOT NULL,
  storage_key TEXT NOT NULL UNIQUE,
  published_at TEXT NOT NULL,
  UNIQUE(package_id, version)
);

CREATE INDEX IF NOT EXISTS idx_package_versions_package ON package_versions(package_id);
)sql";

constexpr const char* kMigration002 = R"sql(
CREATE TABLE IF NOT EXISTS auth_sessions (
  id TEXT PRIMARY KEY,
  user_id TEXT NOT NULL REFERENCES users(id),
  token_hash TEXT NOT NULL UNIQUE,
  expires_at TEXT NOT NULL,
  revoked_at TEXT,
  created_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_auth_sessions_user ON auth_sessions(user_id);
CREATE INDEX IF NOT EXISTS idx_auth_sessions_expires ON auth_sessions(expires_at);
CREATE INDEX IF NOT EXISTS idx_auth_sessions_token ON auth_sessions(token_hash);
)sql";

const std::vector<Migration> kMigrations = {
    {1, kMigration001},
    {2, kMigration002},
};

std::string now_iso8601_utc() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

bool migration_applied(PGconn* conn, int version) {
    const std::string version_text = std::to_string(version);
    const char* params[1] = {version_text.c_str()};
    PGresult* result = PQexecParams(conn, "SELECT 1 FROM schema_migrations WHERE version = $1::int LIMIT 1", 1, nullptr,
                                    params, nullptr, nullptr, 0);
    const bool applied = PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0;
    PQclear(result);
    return applied;
}

bool exec_sql(PGconn* conn, const char* sql, std::string* error) {
    PGresult* result = PQexec(conn, sql);
    const ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        if (error) {
            std::ostringstream out;
            out << "migration failed";
            const char* message = PQerrorMessage(conn);
            if (message != nullptr && message[0] != '\0') {
                out << ": " << message;
            }
            *error = out.str();
        }
        PQclear(result);
        return false;
    }
    PQclear(result);
    return true;
}

}  // namespace

bool run_registry_postgres_migrations(PGconn* conn, std::string* error) {
    if (conn == nullptr) {
        if (error) {
            *error = "postgres connection is not open";
        }
        return false;
    }
    for (const Migration& migration : kMigrations) {
        if (migration_applied(conn, migration.version)) {
            continue;
        }
        if (!exec_sql(conn, migration.sql, error)) {
            return false;
        }
        const std::string version_text = std::to_string(migration.version);
        const std::string applied_at = now_iso8601_utc();
        const char* params[2] = {version_text.c_str(), applied_at.c_str()};
        PGresult* result =
            PQexecParams(conn, "INSERT INTO schema_migrations(version, applied_at) VALUES($1::int, $2)", 2, nullptr,
                         params, nullptr, nullptr, 0);
        if (PQresultStatus(result) != PGRES_COMMAND_OK) {
            PQclear(result);
            if (error) {
                *error = "failed to record migration version";
            }
            return false;
        }
        PQclear(result);
    }
    return true;
}

}  // namespace pscript::registry
