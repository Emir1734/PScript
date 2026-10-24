#pragma once

#include <iostream>
#include <string_view>
#include <vector>

namespace pscript {

int run_registry_auth_command(std::string_view command, const std::vector<std::string>& args, std::ostream& out,
                              std::ostream& err);

bool is_registry_auth_command(std::string_view name);

}  // namespace pscript
