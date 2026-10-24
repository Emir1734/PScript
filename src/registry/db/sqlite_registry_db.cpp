#include "registry/db/sqlite_registry_db.h"

#include "registry/db/migrations.h"
#include "runtime/crypto_ops.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace pscript::registry {
namespace {

constexpr const char* kDisabledPasswordHash = "$pscript$disabled$";

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

std::string random_id() {
    const std::vector<std::uint8_t> bytes = crypto_random_bytes(16);
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::uint8_t byte : bytes) {
        out.width(2);
        out << static_cast<int>(byte);
    }
    return out.str();
}

std::string normalize_username(std::string_view username) {
    std::string out(username);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

bool bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
    return sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

bool step_done(sqlite3_stmt* stmt, std::string* error, const char* context) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
        return true;
    }
    if (error) {
        *error = context;
    }
    return false;
}

}  // namespace

SqliteRegistryDatabase::SqliteRegistryDatabase(std::filesystem::path sqlite_path)
    : sqlite_path_(std::move(sqlite_path)) {}

SqliteRegistryDatabase::~SqliteRegistryDatabase() {
    if (db_ != nullptr) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool SqliteRegistryDatabase::open(std::string* error) {
    if (db_ != nullptr) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(sqlite_path_.parent_path(), ec);
    const std::string path = sqlite_path_.string();
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        if (error) {
            *error = "failed to open registry database";
        }
        return false;
    }
    sqlite3_exec(db_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    return true;
}

bool SqliteRegistryDatabase::migrate(std::string* error) {
    if (db_ == nullptr && !open(error)) {
        return false;
    }
    return run_registry_migrations(db_, error);
}

bool SqliteRegistryDatabase::ping(std::string* error) {
    if (db_ == nullptr && !open(error)) {
        return false;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT 1", -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "database ping failed";
        }
        return false;
    }
    const bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (!ok && error) {
        *error = "database ping failed";
    }
    return ok;
}

bool SqliteRegistryDatabase::ensure_user(const std::string& username, std::string* user_id_out, std::string* error) {
    const std::string normalized = normalize_username(username);
    sqlite3_stmt* lookup = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM users WHERE username = ? COLLATE NOCASE LIMIT 1", -1, &lookup,
                           nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to look up user";
        }
        return false;
    }
    bind_text(lookup, 1, normalized);
    if (sqlite3_step(lookup) == SQLITE_ROW) {
        if (user_id_out) {
            *user_id_out = reinterpret_cast<const char*>(sqlite3_column_text(lookup, 0));
        }
        sqlite3_finalize(lookup);
        return true;
    }
    sqlite3_finalize(lookup);

    const std::string user_id = random_id();
    const std::string created_at = now_iso8601_utc();
    sqlite3_stmt* insert = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO users(id, username, password_hash, created_at) VALUES(?, ?, ?, ?)", -1, &insert,
                           nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to create user";
        }
        return false;
    }
    bind_text(insert, 1, user_id);
    bind_text(insert, 2, normalized);
    bind_text(insert, 3, kDisabledPasswordHash);
    bind_text(insert, 4, created_at);
    if (sqlite3_step(insert) != SQLITE_DONE) {
        sqlite3_finalize(insert);
        if (error) {
            *error = "failed to insert user";
        }
        return false;
    }
    sqlite3_finalize(insert);
    if (user_id_out) {
        *user_id_out = user_id;
    }
    return true;
}

bool SqliteRegistryDatabase::ensure_package(const std::string& user_id, const std::string& package_name,
                                            std::string* package_id_out, std::string* error) {
    sqlite3_stmt* lookup = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM packages WHERE owner_user_id = ? AND name = ? LIMIT 1", -1, &lookup,
                           nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to look up package";
        }
        return false;
    }
    bind_text(lookup, 1, user_id);
    bind_text(lookup, 2, package_name);
    if (sqlite3_step(lookup) == SQLITE_ROW) {
        if (package_id_out) {
            *package_id_out = reinterpret_cast<const char*>(sqlite3_column_text(lookup, 0));
        }
        sqlite3_finalize(lookup);
        return true;
    }
    sqlite3_finalize(lookup);

    const std::string package_id = random_id();
    const std::string created_at = now_iso8601_utc();
    sqlite3_stmt* insert = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO packages(id, owner_user_id, name, visibility, created_at) VALUES(?, ?, ?, "
                           "'public', ?)",
                           -1, &insert, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to create package";
        }
        return false;
    }
    bind_text(insert, 1, package_id);
    bind_text(insert, 2, user_id);
    bind_text(insert, 3, package_name);
    bind_text(insert, 4, created_at);
    if (sqlite3_step(insert) != SQLITE_DONE) {
        sqlite3_finalize(insert);
        if (error) {
            *error = "failed to insert package";
        }
        return false;
    }
    sqlite3_finalize(insert);
    if (package_id_out) {
        *package_id_out = package_id;
    }
    return true;
}

bool SqliteRegistryDatabase::insert_package_version(const std::string& package_id, const package::SemVer& version,
                                                    const std::string& checksum_sha256, std::uint64_t archive_size,
                                                    const std::string& storage_key, std::string* error) {
    const std::string version_text = version.to_string();
    const std::string version_id = random_id();
    const std::string published_at = now_iso8601_utc();
    sqlite3_stmt* insert = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO package_versions(id, package_id, version, checksum_sha256, archive_size, "
                           "storage_key, published_at) VALUES(?, ?, ?, ?, ?, ?, ?)",
                           -1, &insert, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to prepare version insert";
        }
        return false;
    }
    bind_text(insert, 1, version_id);
    bind_text(insert, 2, package_id);
    bind_text(insert, 3, version_text);
    bind_text(insert, 4, checksum_sha256);
    sqlite3_bind_int64(insert, 5, static_cast<sqlite3_int64>(archive_size));
    bind_text(insert, 6, storage_key);
    bind_text(insert, 7, published_at);
    const int rc = sqlite3_step(insert);
    sqlite3_finalize(insert);
    if (rc == SQLITE_CONSTRAINT) {
        if (error) {
            *error = "package version already exists";
        }
        return false;
    }
    if (rc != SQLITE_DONE) {
        if (error) {
            *error = "failed to insert package version";
        }
        return false;
    }
    return true;
}

std::vector<std::string> SqliteRegistryDatabase::list_versions(const std::string& owner_username,
                                                               const std::string& package_name, std::string* error) {
    std::vector<std::string> versions;
    const std::string owner = normalize_username(owner_username);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT pv.version FROM package_versions pv "
            "INNER JOIN packages p ON p.id = pv.package_id "
            "INNER JOIN users u ON u.id = p.owner_user_id "
            "WHERE u.username = ? COLLATE NOCASE AND p.name = ? "
            "ORDER BY pv.published_at ASC",
            -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to list versions";
        }
        return versions;
    }
    bind_text(stmt, 1, owner);
    bind_text(stmt, 2, package_name);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        versions.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return versions;
}

std::optional<PackageVersionRecord> SqliteRegistryDatabase::get_version(const std::string& owner_username,
                                                                          const std::string& package_name,
                                                                          const std::string& version_text,
                                                                          std::string* error) {
    const std::string owner = normalize_username(owner_username);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT pv.version, pv.checksum_sha256, pv.archive_size, pv.storage_key FROM package_versions pv "
            "INNER JOIN packages p ON p.id = pv.package_id "
            "INNER JOIN users u ON u.id = p.owner_user_id "
            "WHERE u.username = ? COLLATE NOCASE AND p.name = ? AND pv.version = ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to look up version";
        }
        return std::nullopt;
    }
    bind_text(stmt, 1, owner);
    bind_text(stmt, 2, package_name);
    bind_text(stmt, 3, version_text);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    PackageVersionRecord record;
    record.version = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    record.checksum_sha256 = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    record.archive_size = static_cast<std::uint64_t>(sqlite3_column_int64(stmt, 2));
    record.storage_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    sqlite3_finalize(stmt);
    return record;
}

std::vector<std::string> SqliteRegistryDatabase::search_packages(const std::string& query, int limit,
                                                                 std::string* error) {
    std::vector<std::string> results;
    if (limit <= 0) {
        return results;
    }
    std::string pattern = query;
    for (char& ch : pattern) {
        if (ch == '%' || ch == '_') {
            ch = ' ';
        }
    }
    pattern = '%' + pattern + '%';

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT '@' || u.username || '/' || p.name "
            "FROM packages p "
            "INNER JOIN users u ON u.id = p.owner_user_id "
            "WHERE ('@' || u.username || '/' || p.name) LIKE ? ESCAPE '\\' "
            "OR p.name LIKE ? ESCAPE '\\' "
            "ORDER BY p.name ASC "
            "LIMIT ?",
            -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "search failed";
        }
        return results;
    }
    bind_text(stmt, 1, pattern);
    bind_text(stmt, 2, pattern);
    sqlite3_bind_int(stmt, 3, limit);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        results.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    }
    sqlite3_finalize(stmt);
    return results;
}

RegisterUserResult SqliteRegistryDatabase::register_user(const std::string& username, const std::string& password_hash,
                                                         std::string* user_id_out, std::string* error) {
    const std::string normalized = normalize_username(username);
    const std::string user_id = random_id();
    const std::string created_at = now_iso8601_utc();
    sqlite3_stmt* insert = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO users(id, username, password_hash, created_at) VALUES(?, ?, ?, ?)", -1, &insert,
                           nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to create user";
        }
        return RegisterUserResult::Failed;
    }
    bind_text(insert, 1, user_id);
    bind_text(insert, 2, normalized);
    bind_text(insert, 3, password_hash);
    bind_text(insert, 4, created_at);
    const int rc = sqlite3_step(insert);
    sqlite3_finalize(insert);
    if (rc == SQLITE_CONSTRAINT) {
        if (error) {
            *error = "username already taken";
        }
        return RegisterUserResult::UsernameTaken;
    }
    if (rc != SQLITE_DONE) {
        if (error) {
            *error = "failed to register user";
        }
        return RegisterUserResult::Failed;
    }
    if (user_id_out) {
        *user_id_out = user_id;
    }
    return RegisterUserResult::Ok;
}

std::optional<UserRecord> SqliteRegistryDatabase::find_user_by_username(const std::string& username,
                                                                        std::string* error) {
    const std::string normalized = normalize_username(username);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id, username, password_hash FROM users WHERE username = ? COLLATE NOCASE LIMIT 1",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to look up user";
        }
        return std::nullopt;
    }
    bind_text(stmt, 1, normalized);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    UserRecord record;
    record.id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    record.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    record.password_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    sqlite3_finalize(stmt);
    return record;
}

bool SqliteRegistryDatabase::create_auth_session(const std::string& user_id, const std::string& token_hash,
                                                 const std::string& expires_at, std::string* session_id_out,
                                                 std::string* error) {
    const std::string session_id = random_id();
    const std::string created_at = now_iso8601_utc();
    sqlite3_stmt* insert = nullptr;
    if (sqlite3_prepare_v2(db_,
                           "INSERT INTO auth_sessions(id, user_id, token_hash, expires_at, created_at) VALUES(?, ?, ?, "
                           "?, ?)",
                           -1, &insert, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to create session";
        }
        return false;
    }
    bind_text(insert, 1, session_id);
    bind_text(insert, 2, user_id);
    bind_text(insert, 3, token_hash);
    bind_text(insert, 4, expires_at);
    bind_text(insert, 5, created_at);
    const int rc = sqlite3_step(insert);
    sqlite3_finalize(insert);
    if (rc != SQLITE_DONE) {
        if (error) {
            *error = "failed to insert session";
        }
        return false;
    }
    if (session_id_out) {
        *session_id_out = session_id;
    }
    return true;
}

std::optional<AuthSessionRecord> SqliteRegistryDatabase::find_active_session_by_token_hash(const std::string& token_hash,
                                                                                         std::string* error) {
    const std::string now = now_iso8601_utc();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT s.id, s.user_id, u.username, s.expires_at FROM auth_sessions s "
            "INNER JOIN users u ON u.id = s.user_id "
            "WHERE s.token_hash = ? AND s.revoked_at IS NULL AND s.expires_at > ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to look up session";
        }
        return std::nullopt;
    }
    bind_text(stmt, 1, token_hash);
    bind_text(stmt, 2, now);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return std::nullopt;
    }
    AuthSessionRecord record;
    record.session_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    record.user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    record.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    record.expires_at = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    sqlite3_finalize(stmt);
    return record;
}

bool SqliteRegistryDatabase::revoke_session_by_token_hash(const std::string& token_hash, std::string* error) {
    const std::string revoked_at = now_iso8601_utc();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE auth_sessions SET revoked_at = ? WHERE token_hash = ? AND revoked_at IS NULL",
                           -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to revoke session";
        }
        return false;
    }
    bind_text(stmt, 1, revoked_at);
    bind_text(stmt, 2, token_hash);
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        if (error) {
            *error = "failed to revoke session";
        }
        return false;
    }
    return true;
}

int SqliteRegistryDatabase::purge_expired_sessions(std::string* error) {
    const std::string now = now_iso8601_utc();
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM auth_sessions WHERE expires_at <= ? OR revoked_at IS NOT NULL", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to purge sessions";
        }
        return 0;
    }
    bind_text(stmt, 1, now);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        if (error) {
            *error = "failed to purge sessions";
        }
        return 0;
    }
    const int removed = sqlite3_changes(db_);
    sqlite3_finalize(stmt);
    return removed;
}

bool SqliteRegistryDatabase::package_version_exists(const std::string& owner_username, const std::string& package_name,
                                                    const std::string& version_text, std::string* error) {
    return get_version(owner_username, package_name, version_text, error).has_value();
}

std::vector<std::string> SqliteRegistryDatabase::list_storage_keys(std::string* error) {
    std::vector<std::string> keys;
    if (db_ == nullptr && !open(error)) {
        return keys;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT storage_key FROM package_versions", -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to list storage keys";
        }
        return keys;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char* text = sqlite3_column_text(stmt, 0);
        if (text != nullptr) {
            keys.emplace_back(reinterpret_cast<const char*>(text));
        }
    }
    sqlite3_finalize(stmt);
    return keys;
}

bool SqliteRegistryDatabase::user_owns_package(const std::string& user_id, const std::string& owner_username,
                                               const std::string& package_name, std::string* error) {
    const std::string owner = normalize_username(owner_username);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT 1 FROM packages p INNER JOIN users u ON u.id = p.owner_user_id "
            "WHERE p.owner_user_id = ? AND u.username = ? COLLATE NOCASE AND p.name = ? LIMIT 1",
            -1, &stmt, nullptr) != SQLITE_OK) {
        if (error) {
            *error = "failed to check package ownership";
        }
        return false;
    }
    bind_text(stmt, 1, user_id);
    bind_text(stmt, 2, owner);
    bind_text(stmt, 3, package_name);
    const bool owned = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return owned;
}

}  // namespace pscript::registry
