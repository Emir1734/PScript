#include "registry/db/migrations.h"

#include <sqlite3.h>

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

CREATE UNIQUE INDEX IF NOT EXISTS idx_users_username_lower ON users(username COLLATE NOCASE);

CREATE TABLE IF NOT EXISTS packages (
  id TEXT PRIMARY KEY,
  owner_user_id TEXT NOT NULL REFERENCES users(id),
  name TEXT NOT NULL,
  visibility TEXT NOT NULL DEFAULT 'public' CHECK(visibility IN ('public', 'private')),
  created_at TEXT NOT NULL,
  UNIQUE(owner_user_id, name)
);

CREATE INDEX IF NOT EXISTS idx_packages_owner ON packages(owner_user_id);

CREATE TABLE IF NOT EXISTS package_versions (
  id TEXT PRIMARY KEY,
  package_id TEXT NOT NULL REFERENCES packages(id),
  version TEXT NOT NULL,
  checksum_sha256 TEXT NOT NULL,
  archive_size INTEGER NOT NULL,
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

bool migration_applied(sqlite3* db, int version) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM schema_migrations WHERE version = ? LIMIT 1", -1, &stmt, nullptr) !=
        SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int(stmt, 1, version);
    const int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return step == SQLITE_ROW;
}

bool exec_sql(sqlite3* db, const char* sql, std::string* error) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        if (error) {
            std::ostringstream out;
            out << "migration failed";
            if (message != nullptr) {
                out << ": " << message;
            }
            *error = out.str();
        }
        sqlite3_free(message);
        return false;
    }
    return true;
}

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

}  // namespace

bool run_registry_migrations(sqlite3* db, std::string* error) {
    for (const Migration& migration : kMigrations) {
        if (migration_applied(db, migration.version)) {
            continue;
        }
        if (!exec_sql(db, migration.sql, error)) {
            return false;
        }
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "INSERT INTO schema_migrations(version, applied_at) VALUES(?, ?)", -1, &stmt,
                               nullptr) != SQLITE_OK) {
            if (error) {
                *error = "failed to record migration";
            }
            return false;
        }
        const std::string applied_at = now_iso8601_utc();
        sqlite3_bind_int(stmt, 1, migration.version);
        sqlite3_bind_text(stmt, 2, applied_at.c_str(), static_cast<int>(applied_at.size()), SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            if (error) {
                *error = "failed to record migration version";
            }
            return false;
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

}  // namespace pscript::registry
