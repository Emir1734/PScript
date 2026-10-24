#pragma once

#include "parser/ast.h"

#include <string>

namespace pscript {

std::string format_source_layout(const std::string& source, const Program& program);

}  // namespace pscript
