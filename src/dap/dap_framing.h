#pragma once

#include <iosfwd>
#include <string>

namespace pscript::dap {

bool read_message(std::istream& in, std::string& body);
void write_message(std::ostream& out, const std::string& body);

}  // namespace pscript::dap
