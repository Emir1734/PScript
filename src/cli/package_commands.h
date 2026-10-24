#pragma once

#include <iostream>
#include <string_view>
#include <vector>

namespace pscript {

class CliState;

int run_package_command(const std::string& command, const std::vector<std::string>& args, CliState& cli,
                        std::ostream& out, std::ostream& err);

bool is_package_command(std::string_view name);

}  // namespace pscript
