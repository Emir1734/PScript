#pragma once

#include "package/registry_credentials.h"

#include <string>
#include <vector>

namespace pscript::package {

struct RegistryAuthToken {
    std::string username;
    std::string token;
    std::string expires_at;
};

bool registry_remote_register(const std::string& registry_url, const std::string& username, std::string_view password,
                              RegistryAuthToken* token_out, std::string* error = nullptr);
bool registry_remote_login(const std::string& registry_url, const std::string& username, std::string_view password,
                           RegistryAuthToken* token_out, std::string* error = nullptr);
bool registry_remote_logout(const std::string& registry_url, const std::string& bearer_token,
                            std::string* error = nullptr);
bool registry_remote_whoami(const std::string& registry_url, const std::string& bearer_token, std::string* username_out,
                            std::string* error = nullptr);

std::vector<std::string> registry_remote_search(const std::string& registry_url, const std::string& query, int limit,
                                                std::string* error = nullptr);

}  // namespace pscript::package
