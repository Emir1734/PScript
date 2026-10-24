#pragma once

#include "stdlib/archive/archive_types.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pscript::archive {

[[nodiscard]] bool is_absolute_archive_path(std::string_view path);
[[nodiscard]] std::optional<std::string> normalize_archive_entry_path(std::string_view raw, bool is_directory);
[[nodiscard]] std::filesystem::path secure_extract_path(const std::filesystem::path& destination_root,
                                                          const std::string& entry_path);
void check_extract_limits(const ArchiveLimits& limits, std::uint64_t entry_count, std::uint64_t total_uncompressed,
                          std::uint64_t single_file_size);
bool is_safe_symlink_target(std::string_view target, const std::filesystem::path& destination_root);

}  // namespace pscript::archive
