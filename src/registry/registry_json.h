#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pscript::registry {

std::string json_escape(std::string_view text);
std::string json_string(std::string_view text);
std::string json_string_array(const std::vector<std::string>& values);
std::string json_search_results(const std::vector<std::string>& package_ids);

}  // namespace pscript::registry
