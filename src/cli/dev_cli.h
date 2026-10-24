#pragma once

#include "cli/cli.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace pscript {

void print_dev_usage(std::ostream& out);

CliRunResult dispatch_dev_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                              std::istream& in);

}  // namespace pscript
