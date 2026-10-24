#include "registry/service/registry_service.h"

#include "package/checksum.h"
#include "package/package_archive.h"
#include "runtime/crypto_ops.h"

#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace pscript::registry {
namespace {

std::string bytes_to_hex_lower(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::uint8_t byte : bytes) {
        out.width(2);
        out << static_cast<int>(byte);
    }
    return out.str();
}

bool validate_archive_directory_checksum(const std::filesystem::path& archive_path, std::string* directory_checksum,
                                         std::string* error) {
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path() / "pscript_registry_validate";
    std::error_code ec;
    std::filesystem::remove_all(temp_root, ec);
    std::filesystem::create_directories(temp_root, ec);
    if (!package::extract_tar_gz_archive(archive_path, temp_root, error)) {
        std::filesystem::remove_all(temp_root, ec);
        return false;
    }
    const std::string checksum = package::sha256_directory(temp_root.string(), error);
    std::filesystem::remove_all(temp_root, ec);
    if (checksum.empty()) {
        return false;
    }
    if (directory_checksum) {
        *directory_checksum = checksum;
    }
    return true;
}

}  // namespace

RegistryService::RegistryService(std::unique_ptr<RegistryDatabase> database, BlobStore blob_store,
                                 AuthSettings auth_settings)
    : database_(std::move(database)),
      blob_store_(std::move(blob_store)),
      auth_service_(*database_, auth_settings, rate_limiter_) {}

AuthService& RegistryService::auth() {
    return auth_service_;
}

const AuthService& RegistryService::auth() const {
    return auth_service_;
}

RegistryDatabase& RegistryService::database() {
    return *database_;
}

bool RegistryService::initialize(std::string* error) {
    if (!blob_store_.ensure_ready(error)) {
        return false;
    }
    if (!database_->open(error)) {
        return false;
    }
    if (!database_->migrate(error)) {
        return false;
    }
    auth_service_.maintenance(error);
    std::string reconcile_error;
    const std::vector<std::string> keys = database_->list_storage_keys(&reconcile_error);
    std::unordered_set<std::string> referenced(keys.begin(), keys.end());
    blob_store_.remove_unreferenced_blobs(referenced, &reconcile_error);
    return true;
}

RegistryHealth RegistryService::health(std::string* error) const {
    RegistryHealth status;
    status.database_ok = database_->ping(error);
    status.ok = status.database_ok;
    return status;
}

std::vector<std::string> RegistryService::search(const std::string& query, int limit, std::string* error) const {
    return database_->search_packages(query, limit, error);
}

std::vector<std::string> RegistryService::list_versions(const package::ScopedPackageId& package_id,
                                                        std::string* error) const {
    return database_->list_versions(package_id.username, package_id.package, error);
}

bool RegistryService::read_version_archive(const package::ScopedPackageId& package_id, const package::SemVer& version,
                                           std::vector<std::uint8_t>* bytes_out, std::string* error) const {
    const std::optional<PackageVersionRecord> record =
        database_->get_version(package_id.username, package_id.package, version.to_string(), error);
    if (!record) {
        if (error && error->empty()) {
            *error = "package version not found";
        }
        return false;
    }
    const std::optional<std::filesystem::path> blob_path = blob_store_.resolve_blob_path(record->storage_key, error);
    if (!blob_path) {
        if (error && error->empty()) {
            *error = "package blob missing";
        }
        return false;
    }
    std::ifstream in(*blob_path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "failed to read package archive";
        }
        return false;
    }
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::filesystem::path temp_archive =
        std::filesystem::temp_directory_path() / "pscript_registry_download_verify.tar.gz";
    std::ofstream temp_out(temp_archive, std::ios::binary | std::ios::trunc);
    temp_out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    temp_out.close();
    std::string directory_checksum;
    if (!validate_archive_directory_checksum(temp_archive, &directory_checksum, error)) {
        std::error_code remove_ec;
        std::filesystem::remove(temp_archive, remove_ec);
        return false;
    }
    std::error_code remove_ec;
    std::filesystem::remove(temp_archive, remove_ec);
    if (directory_checksum != record->checksum_sha256) {
        if (error) {
            *error = "package archive checksum mismatch";
        }
        return false;
    }
    if (bytes_out) {
        *bytes_out = data;
    }
    return true;
}

bool RegistryService::ingest_version(const package::ScopedPackageId& package_id, const package::SemVer& version,
                                     const std::filesystem::path& archive_path, std::string* error) {
    if (!package::is_valid_username(package_id.username) || !package::is_valid_unscoped_package_name(package_id.package)) {
        if (error) {
            *error = "invalid package identity";
        }
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(archive_path, ec)) {
        if (error) {
            *error = "archive file not found";
        }
        return false;
    }
    const auto size = std::filesystem::file_size(archive_path, ec);
    if (ec || size > blob_store_.max_upload_bytes) {
        if (error) {
            *error = "archive exceeds upload size limit";
        }
        return false;
    }
    std::string directory_checksum;
    if (!validate_archive_directory_checksum(archive_path, &directory_checksum, error)) {
        return false;
    }

    std::string user_id;
    if (!database_->ensure_user(package_id.username, &user_id, error)) {
        return false;
    }
    PublishVersionResult published;
    return commit_published_archive(package_id, user_id, version, archive_path, &published, error);
}

bool RegistryService::publish_version(const AuthenticatedPrincipal& principal,
                                      const package::ScopedPackageId& package_id, const package::SemVer& version,
                                      const std::filesystem::path& archive_path, PublishVersionResult* result,
                                      std::string* error) {
    if (principal.username != package_id.username) {
        if (error) {
            *error = "forbidden";
        }
        return false;
    }
    if (!package::is_valid_username(package_id.username) || !package::is_valid_unscoped_package_name(package_id.package)) {
        if (error) {
            *error = "invalid package identity";
        }
        return false;
    }
    std::string ownership_error;
    if (!database_->user_owns_package(principal.user_id, package_id.username, package_id.package, &ownership_error)) {
        std::string package_db_id;
        if (!database_->ensure_package(principal.user_id, package_id.package, &package_db_id, error)) {
            return false;
        }
    }
    if (database_->package_version_exists(package_id.username, package_id.package, version.to_string(), error)) {
        if (error) {
            *error = "package version already exists";
        }
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(archive_path, ec)) {
        if (error) {
            *error = "archive file not found";
        }
        return false;
    }
    const auto size = std::filesystem::file_size(archive_path, ec);
    if (ec || size > blob_store_.max_upload_bytes) {
        if (error) {
            *error = "archive exceeds upload size limit";
        }
        return false;
    }
    std::string directory_checksum;
    if (!validate_archive_directory_checksum(archive_path, &directory_checksum, error)) {
        return false;
    }
    return commit_published_archive(package_id, principal.user_id, version, archive_path, result, error);
}

bool RegistryService::commit_published_archive(const package::ScopedPackageId& package_id, const std::string& owner_user_id,
                                               const package::SemVer& version, const std::filesystem::path& archive_path,
                                               PublishVersionResult* result, std::string* error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(archive_path, ec);
    std::string directory_checksum;
    if (!validate_archive_directory_checksum(archive_path, &directory_checksum, error)) {
        return false;
    }
    if (database_->package_version_exists(package_id.username, package_id.package, version.to_string(), error)) {
        if (error) {
            *error = "package version already exists";
        }
        return false;
    }
    std::string package_db_id;
    if (!database_->ensure_package(owner_user_id, package_id.package, &package_db_id, error)) {
        return false;
    }
    const std::string storage_key = blob_store_.generate_storage_key();
    if (!blob_store_.store_new_blob(archive_path, storage_key, error)) {
        return false;
    }
    if (!database_->insert_package_version(package_db_id, version, directory_checksum, static_cast<std::uint64_t>(size),
                                           storage_key, error)) {
        std::error_code remove_ec;
        if (const std::optional<std::filesystem::path> stored = blob_store_.resolve_blob_path(storage_key, nullptr)) {
            std::filesystem::remove(*stored, remove_ec);
        }
        return false;
    }
    if (result) {
        result->canonical_id = package_id.canonical();
        result->version = version.to_string();
        result->checksum_sha256 = directory_checksum;
    }
    return true;
}

}  // namespace pscript::registry
