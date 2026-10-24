#pragma once

#include "compiler/bytecode.h"

#include <cstdint>

namespace pscript::jit {

bool callee_body_is_arg_plus_one(const Bytecode& bytecode, const FunctionInfo& callee_fn,
                                 std::uint32_t callee_index);
bool callee_body_is_arg_plus_const(const Bytecode& bytecode, const FunctionInfo& callee_fn, std::uint32_t callee_index,
                                   std::int64_t& constant_out);
bool callee_body_is_identity(const Bytecode& bytecode, const FunctionInfo& callee_fn, std::uint32_t callee_index);

}  // namespace pscript::jit
