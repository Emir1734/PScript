#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace pscript::package {

struct RegistryCredentials {
    std::string registry_url;
    std::string username;
    std::string token;
};

std::filesystem::path registry_credentials_path();
bool save_registry_credentials(const RegistryCredentials& credentials, std::string* error = nullptr);
std::optional<RegistryCredentials> load_registry_credentials(std::string* error = nullptr);
bool clear_registry_credentials(std::string* error = nullptr);

bool registry_url_matches_configured(const std::string& registry_url, std::string* error = nullptr);

}  // namespace pscript::package
