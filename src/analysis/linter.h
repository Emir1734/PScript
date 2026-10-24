#pragma once

#include "analysis/diagnostics.h"
#include "parser/ast.h"

#include <string>
#include <vector>

namespace pscript {

std::vector<Diagnostic> lint_program(const Program& program, const std::string& file);

}  // namespace pscript
