#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace pscript {

int run_profile_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err);

}  // namespace pscript
