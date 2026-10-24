#pragma once

#include "package/semver.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pscript::registry {

struct UserRecord {
    std::string id;
    std::string username;
    std::string password_hash;
};

struct AuthSessionRecord {
    std::string session_id;
    std::string user_id;
    std::string username;
    std::string expires_at;
};

enum class RegisterUserResult {
    Ok,
    UsernameTaken,
    Failed,
};

struct PackageVersionRecord {
    std::string version;
    std::string checksum_sha256;
    std::uint64_t archive_size = 0;
    std::string storage_key;
};

class RegistryDatabase {
public:
    virtual ~RegistryDatabase() = default;

    virtual bool open(std::string* error) = 0;
    virtual bool migrate(std::string* error) = 0;
    virtual bool ping(std::string* error) = 0;

    virtual bool ensure_user(const std::string& username, std::string* user_id_out, std::string* error) = 0;
    virtual bool ensure_package(const std::string& user_id, const std::string& package_name, std::string* package_id_out,
                                std::string* error) = 0;

    virtual bool insert_package_version(const std::string& package_id, const package::SemVer& version,
                                        const std::string& checksum_sha256, std::uint64_t archive_size,
                                        const std::string& storage_key, std::string* error) = 0;

    virtual std::vector<std::string> list_versions(const std::string& owner_username, const std::string& package_name,
                                                   std::string* error) = 0;

    virtual std::optional<PackageVersionRecord> get_version(const std::string& owner_username,
                                                            const std::string& package_name,
                                                            const std::string& version_text, std::string* error) = 0;

    virtual std::vector<std::string> search_packages(const std::string& query, int limit, std::string* error) = 0;

    virtual RegisterUserResult register_user(const std::string& username, const std::string& password_hash,
                                             std::string* user_id_out, std::string* error) = 0;

    virtual std::optional<UserRecord> find_user_by_username(const std::string& username, std::string* error) = 0;

    virtual bool create_auth_session(const std::string& user_id, const std::string& token_hash,
                                     const std::string& expires_at, std::string* session_id_out,
                                     std::string* error) = 0;

    virtual std::optional<AuthSessionRecord> find_active_session_by_token_hash(const std::string& token_hash,
                                                                               std::string* error) = 0;

    virtual bool revoke_session_by_token_hash(const std::string& token_hash, std::string* error) = 0;

    virtual int purge_expired_sessions(std::string* error) = 0;

    virtual bool user_owns_package(const std::string& user_id, const std::string& owner_username,
                                  const std::string& package_name, std::string* error) = 0;

    virtual bool package_version_exists(const std::string& owner_username, const std::string& package_name,
                                        const std::string& version_text, std::string* error) = 0;

    virtual std::vector<std::string> list_storage_keys(std::string* error) = 0;
};

std::unique_ptr<RegistryDatabase> make_registry_database(const std::string& database_url);

}  // namespace pscript::registry
