#include "registry/db/postgres_registry_db.h"

#include "registry/db/postgres_migrations.h"
#include "runtime/crypto_ops.h"

#include <libpq-fe.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

struct PgResultDeleter {
    void operator()(PGresult* result) const { PQclear(result); }
};

using PgResult = std::unique_ptr<PGresult, PgResultDeleter>;

PgResult exec_params(PGconn* conn, const char* query, int param_count, const char* const* param_values) {
    return PgResult(PQexecParams(conn, query, param_count, nullptr, param_values, nullptr, nullptr, 0));
}

bool is_unique_violation(const PGresult* result) {
    const char* sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
    return sqlstate != nullptr && std::strcmp(sqlstate, "23505") == 0;
}

bool query_ok(const PGresult* result, std::string* error, const char* context) {
    const ExecStatusType status = PQresultStatus(result);
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) {
        return true;
    }
    if (error) {
        *error = context;
    }
    return false;
}

std::string column_text(const PGresult* result, int row, int column) {
    if (PQgetisnull(result, row, column)) {
        return {};
    }
    const char* value = PQgetvalue(result, row, column);
    return value != nullptr ? std::string(value) : std::string{};
}

}  // namespace

PostgresRegistryDatabase::PostgresRegistryDatabase(std::string connection_string)
    : connection_string_(std::move(connection_string)) {}

PostgresRegistryDatabase::~PostgresRegistryDatabase() {
    if (conn_ != nullptr) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
}

bool PostgresRegistryDatabase::ensure_connection(std::string* error) {
    if (conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK) {
        return true;
    }
    if (conn_ != nullptr) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
    return open(error);
}

bool PostgresRegistryDatabase::open(std::string* error) {
    if (conn_ != nullptr && PQstatus(conn_) == CONNECTION_OK) {
        return true;
    }
    if (conn_ != nullptr) {
        PQfinish(conn_);
        conn_ = nullptr;
    }
    conn_ = PQconnectdb(connection_string_.c_str());
    if (conn_ == nullptr) {
        if (error) {
            *error = "failed to open registry database";
        }
        return false;
    }
    if (PQstatus(conn_) != CONNECTION_OK) {
        if (error) {
            *error = "failed to open registry database";
        }
        PQfinish(conn_);
        conn_ = nullptr;
        return false;
    }
    return true;
}

bool PostgresRegistryDatabase::migrate(std::string* error) {
    if (!ensure_connection(error)) {
        return false;
    }
    return run_registry_postgres_migrations(conn_, error);
}

bool PostgresRegistryDatabase::ping(std::string* error) {
    if (!ensure_connection(error)) {
        return false;
    }
    PgResult result = exec_params(conn_, "SELECT 1", 0, nullptr);
    const bool ok = PQresultStatus(result.get()) == PGRES_TUPLES_OK && PQntuples(result.get()) > 0;
    if (!ok && error) {
        *error = "database ping failed";
    }
    return ok;
}

bool PostgresRegistryDatabase::ensure_user(const std::string& username, std::string* user_id_out, std::string* error) {
    if (!ensure_connection(error)) {
        return false;
    }
    const std::string normalized = normalize_username(username);
    const char* lookup_params[1] = {normalized.c_str()};
    PgResult lookup = exec_params(conn_, "SELECT id FROM users WHERE lower(username) = $1 LIMIT 1", 1, lookup_params);
    if (!query_ok(lookup.get(), error, "failed to look up user")) {
        return false;
    }
    if (PQntuples(lookup.get()) > 0) {
        if (user_id_out) {
            *user_id_out = column_text(lookup.get(), 0, 0);
        }
        return true;
    }

    const std::string user_id = random_id();
    const std::string created_at = now_iso8601_utc();
    const char* insert_params[4] = {user_id.c_str(), normalized.c_str(), kDisabledPasswordHash, created_at.c_str()};
    PgResult insert = exec_params(conn_,
                                  "INSERT INTO users(id, username, password_hash, created_at) VALUES($1, $2, $3, $4)", 4,
                                  insert_params);
    if (!query_ok(insert.get(), error, "failed to insert user")) {
        return false;
    }
    if (user_id_out) {
        *user_id_out = user_id;
    }
    return true;
}

bool PostgresRegistryDatabase::ensure_package(const std::string& user_id, const std::string& package_name,
                                              std::string* package_id_out, std::string* error) {
    if (!ensure_connection(error)) {
        return false;
    }
    const char* lookup_params[2] = {user_id.c_str(), package_name.c_str()};
    PgResult lookup =
        exec_params(conn_, "SELECT id FROM packages WHERE owner_user_id = $1 AND name = $2 LIMIT 1", 2, lookup_params);
    if (!query_ok(lookup.get(), error, "failed to look up package")) {
        return false;
    }
    if (PQntuples(lookup.get()) > 0) {
        if (package_id_out) {
            *package_id_out = column_text(lookup.get(), 0, 0);
        }
        return true;
    }

    const std::string package_id = random_id();
    const std::string created_at = now_iso8601_utc();
    const char* insert_params[4] = {package_id.c_str(), user_id.c_str(), package_name.c_str(), created_at.c_str()};
    PgResult insert = exec_params(
        conn_,
        "INSERT INTO packages(id, owner_user_id, name, visibility, created_at) VALUES($1, $2, $3, 'public', $4)", 4,
        insert_params);
    if (!query_ok(insert.get(), error, "failed to insert package")) {
        return false;
    }
    if (package_id_out) {
        *package_id_out = package_id;
    }
    return true;
}

bool PostgresRegistryDatabase::insert_package_version(const std::string& package_id, const package::SemVer& version,
                                                      const std::string& checksum_sha256, std::uint64_t archive_size,
                                                      const std::string& storage_key, std::string* error) {
    if (!ensure_connection(error)) {
        return false;
    }
    const std::string version_text = version.to_string();
    const std::string version_id = random_id();
    const std::string published_at = now_iso8601_utc();
    const std::string archive_size_text = std::to_string(archive_size);
    const char* insert_params[7] = {version_id.c_str(),       package_id.c_str(),   version_text.c_str(),
                                    checksum_sha256.c_str(),  archive_size_text.c_str(), storage_key.c_str(),
                                    published_at.c_str()};
    PgResult insert = exec_params(
        conn_,
        "INSERT INTO package_versions(id, package_id, version, checksum_sha256, archive_size, storage_key, "
        "published_at) VALUES($1, $2, $3, $4, $5::bigint, $6, $7)",
        7, insert_params);
    if (PQresultStatus(insert.get()) != PGRES_COMMAND_OK) {
        if (is_unique_violation(insert.get())) {
            if (error) {
                *error = "package version already exists";
            }
            return false;
        }
        if (error) {
            *error = "failed to insert package version";
        }
        return false;
    }
    return true;
}

std::vector<std::string> PostgresRegistryDatabase::list_versions(const std::string& owner_username,
                                                                 const std::string& package_name,
                                                                 std::string* error) {
    std::vector<std::string> versions;
    if (!ensure_connection(error)) {
        return versions;
    }
    const std::string owner = normalize_username(owner_username);
    const char* params[2] = {owner.c_str(), package_name.c_str()};
    PgResult result = exec_params(
        conn_,
        "SELECT pv.version FROM package_versions pv "
        "INNER JOIN packages p ON p.id = pv.package_id "
        "INNER JOIN users u ON u.id = p.owner_user_id "
        "WHERE lower(u.username) = $1 AND p.name = $2 "
        "ORDER BY pv.published_at ASC",
        2, params);
    if (!query_ok(result.get(), error, "failed to list versions")) {
        return versions;
    }
    const int rows = PQntuples(result.get());
    versions.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        versions.emplace_back(column_text(result.get(), row, 0));
    }
    return versions;
}

std::optional<PackageVersionRecord> PostgresRegistryDatabase::get_version(const std::string& owner_username,
                                                                          const std::string& package_name,
                                                                          const std::string& version_text,
                                                                          std::string* error) {
    if (!ensure_connection(error)) {
        return std::nullopt;
    }
    const std::string owner = normalize_username(owner_username);
    const char* params[3] = {owner.c_str(), package_name.c_str(), version_text.c_str()};
    PgResult result = exec_params(
        conn_,
        "SELECT pv.version, pv.checksum_sha256, pv.archive_size, pv.storage_key FROM package_versions pv "
        "INNER JOIN packages p ON p.id = pv.package_id "
        "INNER JOIN users u ON u.id = p.owner_user_id "
        "WHERE lower(u.username) = $1 AND p.name = $2 AND pv.version = $3 LIMIT 1",
        3, params);
    if (!query_ok(result.get(), error, "failed to look up version")) {
        return std::nullopt;
    }
    if (PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    PackageVersionRecord record;
    record.version = column_text(result.get(), 0, 0);
    record.checksum_sha256 = column_text(result.get(), 0, 1);
    const char* size_text = PQgetvalue(result.get(), 0, 2);
    record.archive_size = size_text != nullptr ? static_cast<std::uint64_t>(std::strtoull(size_text, nullptr, 10)) : 0;
    record.storage_key = column_text(result.get(), 0, 3);
    return record;
}

std::vector<std::string> PostgresRegistryDatabase::search_packages(const std::string& query, int limit,
                                                                   std::string* error) {
    std::vector<std::string> results;
    if (limit <= 0) {
        return results;
    }
    if (!ensure_connection(error)) {
        return results;
    }
    std::string pattern = query;
    for (char& ch : pattern) {
        if (ch == '%' || ch == '_') {
            ch = ' ';
        }
    }
    pattern = '%' + pattern + '%';
    const std::string limit_text = std::to_string(limit);
    const char* params[3] = {pattern.c_str(), pattern.c_str(), limit_text.c_str()};
    PgResult result = exec_params(
        conn_,
        "SELECT '@' || u.username || '/' || p.name "
        "FROM packages p "
        "INNER JOIN users u ON u.id = p.owner_user_id "
        "WHERE ('@' || u.username || '/' || p.name) LIKE $1 ESCAPE E'\\\\' "
        "OR p.name LIKE $2 ESCAPE E'\\\\' "
        "ORDER BY p.name ASC "
        "LIMIT $3::int",
        3, params);
    if (!query_ok(result.get(), error, "search failed")) {
        return results;
    }
    const int rows = PQntuples(result.get());
    results.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        results.emplace_back(column_text(result.get(), row, 0));
    }
    return results;
}

RegisterUserResult PostgresRegistryDatabase::register_user(const std::string& username,
                                                             const std::string& password_hash, std::string* user_id_out,
                                                             std::string* error) {
    if (!ensure_connection(error)) {
        return RegisterUserResult::Failed;
    }
    const std::string normalized = normalize_username(username);
    const std::string user_id = random_id();
    const std::string created_at = now_iso8601_utc();
    const char* insert_params[4] = {user_id.c_str(), normalized.c_str(), password_hash.c_str(), created_at.c_str()};
    PgResult insert = exec_params(conn_,
                                  "INSERT INTO users(id, username, password_hash, created_at) VALUES($1, $2, $3, $4)", 4,
                                  insert_params);
    if (PQresultStatus(insert.get()) != PGRES_COMMAND_OK) {
        if (is_unique_violation(insert.get())) {
            if (error) {
                *error = "username already taken";
            }
            return RegisterUserResult::UsernameTaken;
        }
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

std::optional<UserRecord> PostgresRegistryDatabase::find_user_by_username(const std::string& username,
                                                                           std::string* error) {
    if (!ensure_connection(error)) {
        return std::nullopt;
    }
    const std::string normalized = normalize_username(username);
    const char* params[1] = {normalized.c_str()};
    PgResult result =
        exec_params(conn_, "SELECT id, username, password_hash FROM users WHERE lower(username) = $1 LIMIT 1", 1, params);
    if (!query_ok(result.get(), error, "failed to look up user")) {
        return std::nullopt;
    }
    if (PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    UserRecord record;
    record.id = column_text(result.get(), 0, 0);
    record.username = column_text(result.get(), 0, 1);
    record.password_hash = column_text(result.get(), 0, 2);
    return record;
}

bool PostgresRegistryDatabase::create_auth_session(const std::string& user_id, const std::string& token_hash,
                                                   const std::string& expires_at, std::string* session_id_out,
                                                   std::string* error) {
    if (!ensure_connection(error)) {
        return false;
    }
    const std::string session_id = random_id();
    const std::string created_at = now_iso8601_utc();
    const char* insert_params[5] = {session_id.c_str(), user_id.c_str(), token_hash.c_str(), expires_at.c_str(),
                                    created_at.c_str()};
    PgResult insert = exec_params(
        conn_,
        "INSERT INTO auth_sessions(id, user_id, token_hash, expires_at, created_at) VALUES($1, $2, $3, $4, $5)", 5,
        insert_params);
    if (!query_ok(insert.get(), error, "failed to insert session")) {
        return false;
    }
    if (session_id_out) {
        *session_id_out = session_id;
    }
    return true;
}

std::optional<AuthSessionRecord> PostgresRegistryDatabase::find_active_session_by_token_hash(
    const std::string& token_hash, std::string* error) {
    if (!ensure_connection(error)) {
        return std::nullopt;
    }
    const std::string now = now_iso8601_utc();
    const char* params[2] = {token_hash.c_str(), now.c_str()};
    PgResult result = exec_params(
        conn_,
        "SELECT s.id, s.user_id, u.username, s.expires_at FROM auth_sessions s "
        "INNER JOIN users u ON u.id = s.user_id "
        "WHERE s.token_hash = $1 AND s.revoked_at IS NULL AND s.expires_at > $2 LIMIT 1",
        2, params);
    if (!query_ok(result.get(), error, "failed to look up session")) {
        return std::nullopt;
    }
    if (PQntuples(result.get()) == 0) {
        return std::nullopt;
    }
    AuthSessionRecord record;
    record.session_id = column_text(result.get(), 0, 0);
    record.user_id = column_text(result.get(), 0, 1);
    record.username = column_text(result.get(), 0, 2);
    record.expires_at = column_text(result.get(), 0, 3);
    return record;
}

bool PostgresRegistryDatabase::revoke_session_by_token_hash(const std::string& token_hash, std::string* error) {
    if (!ensure_connection(error)) {
        return false;
    }
    const std::string revoked_at = now_iso8601_utc();
    const char* params[2] = {revoked_at.c_str(), token_hash.c_str()};
    PgResult result = exec_params(conn_,
                                  "UPDATE auth_sessions SET revoked_at = $1 WHERE token_hash = $2 AND revoked_at IS NULL",
                                  2, params);
    if (!query_ok(result.get(), error, "failed to revoke session")) {
        return false;
    }
    return true;
}

int PostgresRegistryDatabase::purge_expired_sessions(std::string* error) {
    if (!ensure_connection(error)) {
        return 0;
    }
    const std::string now = now_iso8601_utc();
    const char* params[1] = {now.c_str()};
    PgResult result = exec_params(conn_,
                                  "DELETE FROM auth_sessions WHERE expires_at <= $1 OR revoked_at IS NOT NULL", 1,
                                  params);
    if (!query_ok(result.get(), error, "failed to purge sessions")) {
        return 0;
    }
    return static_cast<int>(std::strtol(PQcmdTuples(result.get()), nullptr, 10));
}

bool PostgresRegistryDatabase::user_owns_package(const std::string& user_id, const std::string& owner_username,
                                                 const std::string& package_name, std::string* error) {
    if (!ensure_connection(error)) {
        return false;
    }
    const std::string owner = normalize_username(owner_username);
    const char* params[3] = {user_id.c_str(), owner.c_str(), package_name.c_str()};
    PgResult result = exec_params(
        conn_,
        "SELECT 1 FROM packages p INNER JOIN users u ON u.id = p.owner_user_id "
        "WHERE p.owner_user_id = $1 AND lower(u.username) = $2 AND p.name = $3 LIMIT 1",
        3, params);
    if (!query_ok(result.get(), error, "failed to check package ownership")) {
        return false;
    }
    return PQntuples(result.get()) > 0;
}

bool PostgresRegistryDatabase::package_version_exists(const std::string& owner_username,
                                                      const std::string& package_name, const std::string& version_text,
                                                      std::string* error) {
    return get_version(owner_username, package_name, version_text, error).has_value();
}

std::vector<std::string> PostgresRegistryDatabase::list_storage_keys(std::string* error) {
    std::vector<std::string> keys;
    if (!ensure_connection(error)) {
        return keys;
    }
    PgResult result = exec_params(conn_, "SELECT storage_key FROM package_versions", 0, nullptr);
    if (!query_ok(result.get(), error, "failed to list storage keys")) {
        return keys;
    }
    const int rows = PQntuples(result.get());
    keys.reserve(static_cast<std::size_t>(rows));
    for (int row = 0; row < rows; ++row) {
        if (!PQgetisnull(result.get(), row, 0)) {
            keys.emplace_back(column_text(result.get(), row, 0));
        }
    }
    return keys;
}

}  // namespace pscript::registry
