#include "registry/http/json_body.h"

#include "package/json_util.h"

namespace pscript::registry {

std::optional<AuthCredentialsJson> parse_auth_credentials_json(std::string_view body, std::size_t max_body_bytes) {
    const std::optional<std::string> username = package::json_parse_string_field(body, "username", max_body_bytes);
    const std::optional<std::string> password = package::json_parse_string_field(body, "password", max_body_bytes);
    if (!username || !password) {
        return std::nullopt;
    }
    AuthCredentialsJson credentials;
    credentials.username = *username;
    credentials.password = *password;
    return credentials;
}

}  // namespace pscript::registry
