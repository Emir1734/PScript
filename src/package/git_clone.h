#pragma once

#include <filesystem>
#include <string>

namespace pscript::package {

bool validate_git_remote_url(const std::string& url, std::string* error = nullptr);
bool validate_git_ref(const std::string& ref, std::string* error = nullptr);

// Safe for logs: strips userinfo from https URLs.
std::string redact_git_url_for_display(const std::string& url);

bool run_git_clone(const std::string& url, const std::string& ref, const std::filesystem::path& destination,
                   std::string* error = nullptr);

}  // namespace pscript::package
