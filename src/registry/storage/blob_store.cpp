#include "registry/storage/blob_store.h"

#include "runtime/crypto_ops.h"

#include <fstream>
#include <unordered_set>

namespace pscript::registry {
namespace {

bool is_safe_storage_key(const std::string& key) {
    if (key.empty() || key.size() > 256) {
        return false;
    }
    for (char ch : key) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '/' || ch == '.' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    if (key.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

}  // namespace

BlobStore::BlobStore(std::filesystem::path root) : root_(std::move(root)) {}

bool BlobStore::ensure_ready(std::string* error) const {
    std::error_code ec;
    std::filesystem::create_directories(root_, ec);
    if (ec) {
        if (error) {
            *error = "failed to create blob storage directory";
        }
        return false;
    }
    return true;
}

std::string BlobStore::generate_storage_key() const {
    const std::vector<std::uint8_t> bytes = crypto_random_bytes(16);
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        constexpr char digits[] = "0123456789abcdef";
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 0x0f]);
    }
    const std::string prefix = hex.substr(0, 2);
    const std::string rest = hex.substr(2);
    return prefix + "/" + rest + ".tar.gz";
}

bool BlobStore::store_new_blob(const std::filesystem::path& source_archive, const std::string& storage_key,
                               std::string* error) const {
    if (!is_safe_storage_key(storage_key)) {
        if (error) {
            *error = "invalid storage key";
        }
        return false;
    }
    const std::filesystem::path destination = root_ / storage_key;
    const std::filesystem::path parent = destination.parent_path();
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        if (error) {
            *error = "failed to create blob parent directory";
        }
        return false;
    }
    if (std::filesystem::exists(destination, ec)) {
        if (error) {
            *error = "blob already exists";
        }
        return false;
    }

    std::error_code size_ec;
    const auto size = std::filesystem::file_size(source_archive, size_ec);
    if (size_ec || size > max_upload_bytes) {
        if (error) {
            *error = "archive exceeds upload size limit";
        }
        return false;
    }

    std::filesystem::copy_file(source_archive, destination, std::filesystem::copy_options::none, ec);
    if (ec) {
        if (error) {
            *error = "failed to store package blob";
        }
        return false;
    }
    return true;
}

std::optional<std::filesystem::path> BlobStore::resolve_blob_path(const std::string& storage_key,
                                                                  std::string* error) const {
    if (!is_safe_storage_key(storage_key)) {
        if (error) {
            *error = "invalid storage key";
        }
        return std::nullopt;
    }
    const std::filesystem::path candidate = root_ / storage_key;
    std::error_code ec;
    const std::filesystem::path root_abs = std::filesystem::absolute(root_, ec);
    const std::filesystem::path candidate_abs = std::filesystem::absolute(candidate, ec);
    if (ec) {
        if (error) {
            *error = "failed to resolve blob path";
        }
        return std::nullopt;
    }
    const auto root_prefix = root_abs.lexically_normal().generic_string();
    const auto candidate_string = candidate_abs.lexically_normal().generic_string();
    if (candidate_string.size() < root_prefix.size() ||
        candidate_string.compare(0, root_prefix.size(), root_prefix) != 0) {
        if (error) {
            *error = "blob path escapes storage root";
        }
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(candidate, ec)) {
        return std::nullopt;
    }
    return candidate;
}

int BlobStore::remove_unreferenced_blobs(const std::unordered_set<std::string>& referenced_keys,
                                          std::string* error) const {
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) {
        return 0;
    }
    int removed = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root_, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), root_, ec);
        if (ec) {
            continue;
        }
        const std::string key = relative.generic_string();
        if (!is_safe_storage_key(key)) {
            continue;
        }
        if (referenced_keys.find(key) != referenced_keys.end()) {
            continue;
        }
        std::filesystem::remove(entry.path(), ec);
        if (!ec) {
            ++removed;
        }
    }
    return removed;
}

}  // namespace pscript::registry
