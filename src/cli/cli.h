#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace pscript {

int run_cli(int argc, char** argv);

struct CliRunResult {
    int exit_code = 1;
};

CliRunResult run_cli_with_args(const std::vector<std::string>& args, std::ostream& out, std::ostream& err);
CliRunResult run_cli_with_args(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                               std::istream& in);

}  // namespace pscript