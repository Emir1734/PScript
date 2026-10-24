#pragma once

#include "registry/db/registry_db.h"

#include <string>

struct pg_conn;

namespace pscript::registry {

class PostgresRegistryDatabase final : public RegistryDatabase {
public:
    explicit PostgresRegistryDatabase(std::string connection_string);
    ~PostgresRegistryDatabase() override;

    bool open(std::string* error) override;
    bool migrate(std::string* error) override;
    bool ping(std::string* error) override;

    bool ensure_user(const std::string& username, std::string* user_id_out, std::string* error) override;
    bool ensure_package(const std::string& user_id, const std::string& package_name, std::string* package_id_out,
                        std::string* error) override;

    bool insert_package_version(const std::string& package_id, const package::SemVer& version,
                                const std::string& checksum_sha256, std::uint64_t archive_size,
                                const std::string& storage_key, std::string* error) override;

    std::vector<std::string> list_versions(const std::string& owner_username, const std::string& package_name,
                                           std::string* error) override;

    std::optional<PackageVersionRecord> get_version(const std::string& owner_username, const std::string& package_name,
                                                    const std::string& version_text, std::string* error) override;

    std::vector<std::string> search_packages(const std::string& query, int limit, std::string* error) override;

    RegisterUserResult register_user(const std::string& username, const std::string& password_hash,
                                     std::string* user_id_out, std::string* error) override;

    std::optional<UserRecord> find_user_by_username(const std::string& username, std::string* error) override;

    bool create_auth_session(const std::string& user_id, const std::string& token_hash, const std::string& expires_at,
                             std::string* session_id_out, std::string* error) override;

    std::optional<AuthSessionRecord> find_active_session_by_token_hash(const std::string& token_hash,
                                                                       std::string* error) override;

    bool revoke_session_by_token_hash(const std::string& token_hash, std::string* error) override;

    int purge_expired_sessions(std::string* error) override;

    bool user_owns_package(const std::string& user_id, const std::string& owner_username,
                           const std::string& package_name, std::string* error) override;

    bool package_version_exists(const std::string& owner_username, const std::string& package_name,
                                const std::string& version_text, std::string* error) override;

    std::vector<std::string> list_storage_keys(std::string* error) override;

private:
    bool ensure_connection(std::string* error);

    std::string connection_string_;
    pg_conn* conn_ = nullptr;
};

}  // namespace pscript::registry
