#pragma once

#include "compiler/types.h"
#include "parser/ast.h"

#include <string>

namespace pscript {

class Bytecode;

std::string type_descriptor_to_string(const TypeDescriptor& desc);
TypeDescriptor parse_type_spec(const std::string& spec, const Location& loc, const Bytecode& bytecode,
                               const std::vector<std::string>* active_type_params = nullptr);

}  // namespace pscript
