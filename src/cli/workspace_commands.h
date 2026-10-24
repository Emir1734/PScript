#pragma once

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {

int run_test_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err);
int run_doctor_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err);
int run_explain_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err);

bool is_workspace_command(std::string_view name);

}  // namespace pscript
