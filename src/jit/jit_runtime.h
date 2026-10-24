#pragma once

#include "compiler/bytecode.h"
#include "jit/jit_code.h"
#include "jit/jit_config.h"
#include "jit/jit_deopt.h"
#include "jit/jit_exec_state.h"
#include "jit/jit_ic.h"
#include "jit/jit_opt_profile.h"
#include "runtime/profiler.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pscript::jit {

void prepare_exec_state_for_compiled_entry(JitExecState& state, const JitCompiledCode* code);
JitExecuteResult execute_compiled_entry(const JitCompiledCode* code, JitExecState& state);

class JitEngine {
public:
    void set_enabled(bool enabled) { enabled_ = enabled; }
    bool enabled() const { return enabled_; }
    void set_config(JitTieringConfig config) { config_ = config; }
    const JitTieringConfig& config() const { return config_; }

    void bind_bytecode(const Bytecode* bytecode, const std::string& fingerprint);
    void set_profile(const ProfileDocument* profile);
    void record_invocation(std::uint32_t function_index, const FunctionInfo& fn);
    JitTier desired_tier(std::uint32_t function_index, const FunctionInfo& fn) const;
    bool qualifies_for_optimizing_tier(std::uint32_t function_index, const FunctionInfo& fn) const;

    const JitCompiledCode* lookup(std::uint32_t function_index) const;
    const JitCompiledCode* compile_if_needed(std::uint32_t function_index, const FunctionInfo& fn);

    JitExecuteResult execute(JitExecState& state, std::uint32_t function_index, const FunctionInfo& fn);
    JitExecuteResult execute_osr(JitExecState& state, std::uint32_t function_index, const FunctionInfo& fn,
                                 std::uint32_t loop_bytecode_ip);

    void invalidate_optimizing_code(std::uint32_t function_index, JitGuardKind guard_kind);
    void record_loop_back(std::uint32_t function_index, std::uint32_t loop_bytecode_ip);
    std::uint64_t loop_trip_count(std::uint32_t function_index, std::uint32_t loop_bytecode_ip) const;
    bool function_is_jit_eligible(std::uint32_t function_index);

    std::uint64_t invocation_count(std::uint32_t function_index) const;
    std::uint32_t optimization_generation(std::uint32_t function_index) const;
    std::size_t compile_attempts() const { return compile_attempts_; }
    std::size_t optimizing_compile_attempts() const { return optimizing_compile_attempts_; }
    std::size_t cache_hits() const { return cache_hits_; }
    std::size_t native_jit_completions() const { return native_jit_completions_; }
    void reset_native_jit_completions() { native_jit_completions_ = 0; }
    JitIcMetrics& ic_metrics() { return ic_metrics_; }
    const JitIcMetrics& ic_metrics() const { return ic_metrics_; }

private:
    bool enabled_ = true;
    JitTieringConfig config_;
    const Bytecode* bytecode_ = nullptr;
    const ProfileDocument* profile_ = nullptr;
    JitOptProfile opt_profile_;
    std::string fingerprint_;
    std::vector<std::uint64_t> invocations_;
    std::unordered_map<std::uint32_t, JitCompiledCode> cache_;
    // Full prior JitCompiledCode objects kept alive across tier upgrades so stale IC
    // entries (entry pointers + callee_call_ic_slots / deopt metadata) remain valid.
    std::vector<JitCompiledCode> retired_codes_;
    void invalidate_ic_entries_for_function(std::uint32_t function_index, const JitCompiledCode& old_code);
    void install_compiled(std::uint32_t function_index, JitCompiledCode&& compiled);
    std::size_t compile_attempts_ = 0;
    std::size_t optimizing_compile_attempts_ = 0;
    std::size_t cache_hits_ = 0;
    std::size_t native_jit_completions_ = 0;
    std::unordered_map<std::uint32_t, std::uint32_t> optimization_generations_;
    std::unordered_map<std::uint32_t, std::uint64_t> deopt_counts_;
    std::unordered_map<std::uint64_t, std::uint64_t> loop_trip_counts_;
    std::unordered_map<std::uint32_t, std::uint64_t> invocations_since_deopt_;
    std::vector<std::uint8_t> eligibility_cache_;
    JitIcMetrics ic_metrics_;
};

}  // namespace pscript::jit
