#pragma once

#include "registry/auth/auth_service.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace pscript::registry {

struct RegistryConfig {
    std::string bind_host = "127.0.0.1";
    int bind_port = 8741;
    // sqlite: absolute path, or postgres:// when libpq backend is enabled.
    std::string database_url;
    std::filesystem::path storage_dir;
    std::size_t max_upload_bytes = 64ULL * 1024 * 1024;
    std::string api_prefix = "/v1";
    int max_search_results = 50;
    bool allow_public_bind = false;
    bool trust_forwarded_client = false;
    AuthSettings auth;
};

std::optional<RegistryConfig> load_registry_config_from_environment(std::string* error = nullptr);
bool validate_registry_config(const RegistryConfig& config, std::string* error = nullptr);

}  // namespace pscript::registry
