#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace pscript::package {

std::string json_escape(std::string_view text);
std::string json_string(std::string_view text);

std::string json_auth_credentials_body(const std::string& username, std::string_view password);

std::optional<std::string> json_parse_string_field(std::string_view body, std::string_view field_name,
                                                  std::size_t max_body_bytes = 1 << 20);

}  // namespace pscript::package
