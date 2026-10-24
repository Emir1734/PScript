#pragma once

#include <string>

namespace pscript::package {

std::string sha256_directory(const std::string& path, std::string* error = nullptr);
bool verify_directory_checksum(const std::string& path, const std::string& expected_hex, std::string* error = nullptr);

}  // namespace pscript::package
