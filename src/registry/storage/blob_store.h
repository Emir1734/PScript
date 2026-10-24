#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>

namespace pscript::registry {

class BlobStore {
public:
    explicit BlobStore(std::filesystem::path root);

    bool ensure_ready(std::string* error) const;
    std::string generate_storage_key() const;

    bool store_new_blob(const std::filesystem::path& source_archive, const std::string& storage_key,
                        std::string* error) const;
    std::optional<std::filesystem::path> resolve_blob_path(const std::string& storage_key, std::string* error) const;

    int remove_unreferenced_blobs(const std::unordered_set<std::string>& referenced_keys, std::string* error) const;

    std::size_t max_upload_bytes = 64ULL * 1024 * 1024;

private:
    std::filesystem::path root_;
};

}  // namespace pscript::registry
