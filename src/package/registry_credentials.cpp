#include "package/registry_credentials.h"

#include "package/atomic_file.h"
#include "package/json_util.h"
#include "package/project.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace pscript::package {
namespace {

std::string trim(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return std::string(value);
}

}  // namespace

std::filesystem::path registry_credentials_path() {
    if (const char* override_path = std::getenv("PSCRIPT_REGISTRY_CREDENTIALS_FILE")) {
        if (override_path[0] != '\0') {
            return std::filesystem::path(override_path);
        }
    }
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    std::filesystem::path base = home != nullptr ? std::filesystem::path(home) : std::filesystem::temp_directory_path();
    return base / ".pscript" / "registry-credentials.json";
}

bool save_registry_credentials(const RegistryCredentials& credentials, std::string* error) {
    if (credentials.registry_url.empty() || credentials.token.empty() || credentials.username.empty()) {
        if (error) {
            *error = "incomplete registry credentials";
        }
        return false;
    }
    const std::filesystem::path path = registry_credentials_path();
    std::ostringstream out;
    out << "{\n"
        << "  \"registry_url\": " << json_string(credentials.registry_url) << ",\n"
        << "  \"username\": " << json_string(credentials.username) << ",\n"
        << "  \"token\": " << json_string(credentials.token) << "\n"
        << "}\n";
    if (!write_file_atomically(path, out.str(), error)) {
        return false;
    }
#ifndef _WIN32
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, ec);
#endif
    return true;
}

std::optional<RegistryCredentials> load_registry_credentials(std::string* error) {
    const std::filesystem::path path = registry_credentials_path();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    const std::string body = buffer.str();
    RegistryCredentials credentials;
    if (const std::optional<std::string> url = json_parse_string_field(body, "registry_url")) {
        credentials.registry_url = *url;
    }
    if (const std::optional<std::string> username = json_parse_string_field(body, "username")) {
        credentials.username = *username;
    }
    if (const std::optional<std::string> token = json_parse_string_field(body, "token")) {
        credentials.token = *token;
    }
    if (credentials.token.empty() || credentials.username.empty()) {
        if (error) {
            *error = "registry credentials file is invalid";
        }
        return std::nullopt;
    }
    if (credentials.registry_url.empty()) {
        credentials.registry_url = default_registry_url();
    }
    return credentials;
}

bool clear_registry_credentials(std::string* error) {
    const std::filesystem::path path = registry_credentials_path();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return true;
    }
    std::filesystem::remove(path, ec);
    if (ec) {
        if (error) {
            *error = "failed to remove registry credentials";
        }
        return false;
    }
    return true;
}

bool registry_url_matches_configured(const std::string& registry_url, std::string* error) {
    const std::string configured = default_registry_url();
    if (configured != registry_url) {
        if (error) {
            *error = "registry URL does not match configured PSCRIPT_REGISTRY";
        }
        return false;
    }
    return true;
}

}  // namespace pscript::package
