#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace pscript::package {

std::optional<std::string> read_text_file(const std::filesystem::path& path);

bool write_file_atomically(const std::filesystem::path& destination, const std::string& content,
                           std::string* error = nullptr);

}  // namespace pscript::package
