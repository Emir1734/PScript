#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace pscript::registry {

struct AuthCredentialsJson {
    std::string username;
    std::string password;
};

std::optional<AuthCredentialsJson> parse_auth_credentials_json(std::string_view body, std::size_t max_body_bytes);

}  // namespace pscript::registry
