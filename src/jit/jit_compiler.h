#pragma once

#include "compiler/bytecode.h"
#include "jit/jit_code.h"
#include "jit/jit_opt.h"
#include "jit/jit_opt_profile.h"

#include <optional>
#include <string>

namespace pscript::jit {

std::optional<JitCompiledCode> compile_function_baseline(const Bytecode& bytecode, std::uint32_t function_index,
                                                         const std::string& fingerprint, const JitOptProfile* profile,
                                                         std::string& error);

std::optional<JitCompiledCode> compile_function_optimizing(const Bytecode& bytecode, std::uint32_t function_index,
                                                           const std::string& fingerprint,
                                                           const JitOptProfile* profile, std::string& error);

bool prepare_function_ir_for_jit_compile(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                                         bool stable_direct_callvalue, OptimizingPassStats& stats, std::string& error,
                                         bool& conservative_dispatch_emit);

}  // namespace pscript::jit
