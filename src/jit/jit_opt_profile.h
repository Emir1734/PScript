#pragma once

#include "compiler/bytecode.h"
#include "runtime/profiler.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace pscript::jit {

struct FunctionOptHints {
    bool stable_integer_ops = false;
    bool very_hot = false;
    bool type_integer_unstable = false;
    std::uint64_t call_count = 0;
    std::uint64_t hot_loop_iterations = 0;
};

struct JitOptProfile {
    bool usable = false;
    std::vector<FunctionOptHints> functions;

    const FunctionOptHints* function_hints(std::uint32_t function_index) const;
    double branch_bias_for_ip(std::uint32_t function_index, std::uint32_t bytecode_ip, bool& found) const;
};

JitOptProfile build_jit_opt_profile(const ProfileDocument* profile, const Bytecode& bytecode);

}  // namespace pscript::jit
