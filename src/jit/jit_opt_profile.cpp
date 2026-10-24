#include "jit/jit_opt_profile.h"

#include <algorithm>

namespace pscript::jit {

const FunctionOptHints* JitOptProfile::function_hints(std::uint32_t function_index) const {
    if (!usable || function_index >= functions.size()) {
        return nullptr;
    }
    return &functions[function_index];
}

double JitOptProfile::branch_bias_for_ip(std::uint32_t function_index, std::uint32_t bytecode_ip,
                                         bool& found) const {
    found = false;
    return 0.5;
}

JitOptProfile build_jit_opt_profile(const ProfileDocument* profile, const Bytecode& bytecode) {
    JitOptProfile out;
    out.functions.assign(bytecode.functions.size(), {});
    if (!profile) {
        return out;
    }
    out.usable = true;
    for (const ProfileFunctionRecord& fn : profile->functions) {
        if (fn.function_index >= out.functions.size()) {
            continue;
        }
        FunctionOptHints& hints = out.functions[fn.function_index];
        hints.call_count = fn.calls;
        hints.very_hot = fn.hotness == ProfileHotness::Hot;
        hints.stable_integer_ops = hints.very_hot && fn.calls >= 100;
    }
    std::uint64_t max_loop = 0;
    for (const ProfileLoopRecord& loop : profile->loops) {
        max_loop = std::max(max_loop, loop.iterations);
    }
    const std::uint64_t hot_loop_threshold = max_loop > 0 ? max_loop / 10 : 0;
    for (const ProfileLoopRecord& loop : profile->loops) {
        if (loop.function_index < out.functions.size() && loop.iterations >= hot_loop_threshold &&
            loop.iterations > 0) {
            out.functions[loop.function_index].hot_loop_iterations =
                std::max(out.functions[loop.function_index].hot_loop_iterations, loop.iterations);
            if (loop.iterations >= hot_loop_threshold * 2) {
                out.functions[loop.function_index].stable_integer_ops = true;
            }
        }
    }
    return out;
}

}  // namespace pscript::jit
