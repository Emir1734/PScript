#pragma once

#include <filesystem>
#include <string>

namespace pscript::package {

// Registry packages are distributed as package.tar.gz (see docs/package-ecosystem.md).
bool create_directory_tar_gz(const std::filesystem::path& source_dir, const std::filesystem::path& archive_path,
                             std::string* error = nullptr);
bool extract_tar_gz_archive(const std::filesystem::path& archive_path, const std::filesystem::path& destination_dir,
                            std::string* error = nullptr);

}  // namespace pscript::package
