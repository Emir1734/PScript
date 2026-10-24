#pragma once

#include <cstdint>

namespace pscript::jit {

enum class JitTier : std::uint8_t {
    Vm = 0,
    BaselineJit = 1,
    OptimizingJit = 2,  // future Section 2
};

struct JitTieringConfig {
    std::uint32_t baseline_invocation_threshold = 32;
    std::uint32_t optimizing_invocation_threshold = 10'000;
    bool use_pgo_hotness = true;
    bool require_profile_for_optimizing = true;
    std::uint32_t max_inline_insn_count = 24;
    std::uint32_t osr_loop_trip_threshold = 1'000;
    std::uint32_t deopt_recompile_invocation_cooldown = 64;
};

}  // namespace pscript::jit
