#pragma once

#include "package/package_identity.h"
#include "package/semver.h"
#include "registry/auth/auth_service.h"
#include "registry/auth/rate_limiter.h"
#include "registry/auth/authz.h"
#include "registry/db/registry_db.h"
#include "registry/storage/blob_store.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pscript::registry {

struct RegistryHealth {
    bool ok = false;
    bool database_ok = false;
};

class RegistryService {
public:
    RegistryService(std::unique_ptr<RegistryDatabase> database, BlobStore blob_store, AuthSettings auth_settings = {});

    bool initialize(std::string* error);
    AuthService& auth();
    const AuthService& auth() const;
    RegistryDatabase& database();
    RegistryHealth health(std::string* error) const;

    std::vector<std::string> search(const std::string& query, int limit, std::string* error) const;
    std::vector<std::string> list_versions(const package::ScopedPackageId& package_id, std::string* error) const;
    bool read_version_archive(const package::ScopedPackageId& package_id, const package::SemVer& version,
                              std::vector<std::uint8_t>* bytes_out, std::string* error) const;

    // Used by tests and internal tooling only — not exposed as a public HTTP publish route in Phase 2.
    bool ingest_version(const package::ScopedPackageId& package_id, const package::SemVer& version,
                        const std::filesystem::path& archive_path, std::string* error);

    struct PublishVersionResult {
        std::string canonical_id;
        std::string version;
        std::string checksum_sha256;
    };

    bool publish_version(const AuthenticatedPrincipal& principal, const package::ScopedPackageId& package_id,
                         const package::SemVer& version, const std::filesystem::path& archive_path,
                         PublishVersionResult* result, std::string* error);

private:
    bool commit_published_archive(const package::ScopedPackageId& package_id, const std::string& owner_user_id,
                                  const package::SemVer& version, const std::filesystem::path& archive_path,
                                  PublishVersionResult* result, std::string* error);
    std::unique_ptr<RegistryDatabase> database_;
    BlobStore blob_store_;
    RateLimiter rate_limiter_;
    AuthService auth_service_;
};

}  // namespace pscript::registry
