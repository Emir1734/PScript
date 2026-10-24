#include "registry/registry_config.h"

#include <cstdlib>
#include <optional>
#include <sstream>

namespace pscript::registry {
namespace {

std::string env_or_empty(const char* name) {
    if (const char* value = std::getenv(name)) {
        return value;
    }
    return {};
}

bool parse_bool(const std::string& value) {
    return value == "1" || value == "true" || value == "TRUE" || value == "yes" || value == "YES";
}

bool parse_positive_int(const std::string& value, int* out) {
    if (value.empty()) {
        return false;
    }
    try {
        const long parsed = std::stol(value);
        if (parsed <= 0 || parsed > 65535) {
            return false;
        }
        *out = static_cast<int>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_size_t(const std::string& value, std::size_t* out) {
    if (value.empty()) {
        return false;
    }
    try {
        const unsigned long long parsed = std::stoull(value);
        *out = static_cast<std::size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

bool is_loopback_host(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

}  // namespace

std::optional<RegistryConfig> load_registry_config_from_environment(std::string* error) {
    RegistryConfig config;

    const std::string host = env_or_empty("PSCRIPT_REGISTRY_HOST");
    if (!host.empty()) {
        config.bind_host = host;
    }

    const std::string port = env_or_empty("PSCRIPT_REGISTRY_PORT");
    if (!port.empty()) {
        if (!parse_positive_int(port, &config.bind_port)) {
            if (error) {
                *error = "invalid PSCRIPT_REGISTRY_PORT";
            }
            return std::nullopt;
        }
    }

    const std::string allow_public = env_or_empty("PSCRIPT_REGISTRY_ALLOW_PUBLIC_BIND");
    config.allow_public_bind = parse_bool(allow_public);

    const std::string storage = env_or_empty("PSCRIPT_REGISTRY_STORAGE_DIR");
    if (!storage.empty()) {
        config.storage_dir = std::filesystem::path(storage);
    } else {
        config.storage_dir = std::filesystem::path("data") / "registry" / "blobs";
    }

    const std::string db = env_or_empty("PSCRIPT_REGISTRY_DATABASE_URL");
    if (!db.empty()) {
        config.database_url = db;
    } else {
        const std::string sqlite_path = env_or_empty("PSCRIPT_REGISTRY_SQLITE_PATH");
        if (!sqlite_path.empty()) {
            config.database_url = "sqlite:" + sqlite_path;
        } else {
            config.database_url = "sqlite:data/registry/registry.db";
        }
    }

    const std::string max_upload = env_or_empty("PSCRIPT_REGISTRY_MAX_UPLOAD_BYTES");
    if (!max_upload.empty()) {
        if (!parse_size_t(max_upload, &config.max_upload_bytes)) {
            if (error) {
                *error = "invalid PSCRIPT_REGISTRY_MAX_UPLOAD_BYTES";
            }
            return std::nullopt;
        }
    }

    const std::string prefix = env_or_empty("PSCRIPT_REGISTRY_API_PREFIX");
    if (!prefix.empty()) {
        config.api_prefix = prefix;
        if (config.api_prefix.empty() || config.api_prefix.front() != '/') {
            if (error) {
                *error = "PSCRIPT_REGISTRY_API_PREFIX must start with '/'";
            }
            return std::nullopt;
        }
    }

    const std::string trust_proxy = env_or_empty("PSCRIPT_REGISTRY_TRUST_FORWARDED_CLIENT");
    config.trust_forwarded_client = parse_bool(trust_proxy);

    const std::string session_ttl = env_or_empty("PSCRIPT_REGISTRY_SESSION_TTL_SECONDS");
    if (!session_ttl.empty()) {
        std::size_t parsed = 0;
        if (!parse_size_t(session_ttl, &parsed) || parsed == 0) {
            if (error) {
                *error = "invalid PSCRIPT_REGISTRY_SESSION_TTL_SECONDS";
            }
            return std::nullopt;
        }
        config.auth.session_ttl_seconds = static_cast<std::uint32_t>(parsed);
    }

    const std::string max_search = env_or_empty("PSCRIPT_REGISTRY_MAX_SEARCH_RESULTS");
    if (!max_search.empty()) {
        int parsed = 0;
        if (!parse_positive_int(max_search, &parsed) || parsed > 200) {
            if (error) {
                *error = "invalid PSCRIPT_REGISTRY_MAX_SEARCH_RESULTS";
            }
            return std::nullopt;
        }
        config.max_search_results = parsed;
    }

    if (!validate_registry_config(config, error)) {
        return std::nullopt;
    }
    return config;
}

bool validate_registry_config(const RegistryConfig& config, std::string* error) {
    if (config.bind_port <= 0 || config.bind_port > 65535) {
        if (error) {
            *error = "invalid listen port";
        }
        return false;
    }
    if (!config.allow_public_bind && !is_loopback_host(config.bind_host)) {
        if (error) {
            *error = "refusing to bind to a non-loopback address without PSCRIPT_REGISTRY_ALLOW_PUBLIC_BIND=1";
        }
        return false;
    }
    if (config.database_url.empty()) {
        if (error) {
            *error = "database URL is required";
        }
        return false;
    }
    if (config.storage_dir.empty()) {
        if (error) {
            *error = "storage directory is required";
        }
        return false;
    }
    if (config.max_upload_bytes == 0) {
        if (error) {
            *error = "max upload size must be greater than zero";
        }
        return false;
    }
    return true;
}

}  // namespace pscript::registry
