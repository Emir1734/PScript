#pragma once

#include "jit/jit_code.h"
#include "jit/jit_ir.h"

#include <string>

namespace pscript {
struct Bytecode;
}

namespace pscript::jit {

bool jit_backend_available();

bool compile_baseline_jit(const JitFunctionIr& ir, const Bytecode* bytecode, JitCodeBuffer& buffer,
                          JitCompiledCode& out, std::string& error);

bool compile_optimizing_jit(const JitFunctionIr& ir, const Bytecode* bytecode, JitCodeBuffer& buffer,
                            JitCompiledCode& out, std::string& error);

bool ir_eligible_for_linear_numeric(const JitFunctionIr& ir, const Bytecode* bytecode);

bool ir_requires_dispatch_loop_emit(const JitFunctionIr& ir);

}  // namespace pscript::jit
