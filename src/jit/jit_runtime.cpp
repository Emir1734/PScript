#include "jit/jit_runtime.h"

#include "jit/jit_helpers.h"

#include "jit/jit_compiler.h"
#include "jit/jit_opt_profile.h"
#include "runtime/profiler.h"

namespace pscript::jit {
namespace {


std::uint8_t tier_rank(JitTier tier) {
    switch (tier) {
        case JitTier::Vm:
            return 0;
        case JitTier::BaselineJit:
            return 1;
        case JitTier::OptimizingJit:
            return 2;
    }
    return 0;
}

std::uint64_t loop_key(std::uint32_t function_index, std::uint32_t loop_bytecode_ip) {
    return (static_cast<std::uint64_t>(function_index) << 32) | loop_bytecode_ip;
}

}  // namespace

void JitEngine::invalidate_ic_entries_for_function(std::uint32_t function_index,
                                                   const JitCompiledCode& old_code) {
    void* old_entry = old_code.entry != nullptr ? reinterpret_cast<void*>(old_code.entry) : nullptr;
    const void* old_ic = old_code.call_ic_slots.empty() ? nullptr
                                                        : static_cast<const void*>(old_code.call_ic_slots.data());
    for (auto& [idx, compiled] : cache_) {
        (void)idx;
        for (JitCallIcSlot& slot : compiled.call_ic_slots) {
            auto clear_entry = [&](JitCallIcEntry& entry) {
                if (entry.function_index == function_index ||
                    (old_entry != nullptr && entry.entry == old_entry) ||
                    (old_ic != nullptr && entry.callee_call_ic_slots == old_ic)) {
                    entry = JitCallIcEntry{};
                }
            };
            clear_entry(slot.mono);
            for (std::size_t i = 0; i < slot.poly_count; ++i) {
                clear_entry(slot.poly[i]);
            }
            if (slot.mode == JitCallIcMode::Monomorphic && slot.mono.entry == nullptr) {
                slot.mode = JitCallIcMode::Empty;
            } else if (slot.mode == JitCallIcMode::Polymorphic) {
                std::size_t write = 0;
                for (std::size_t i = 0; i < slot.poly_count; ++i) {
                    if (slot.poly[i].entry != nullptr) {
                        if (write != i) {
                            slot.poly[write] = slot.poly[i];
                        }
                        ++write;
                    }
                }
                slot.poly_count = write;
                if (slot.poly_count == 0) {
                    slot.mode = JitCallIcMode::Empty;
                } else if (slot.poly_count == 1) {
                    slot.mono = slot.poly[0];
                    slot.poly_count = 0;
                    slot.mode = JitCallIcMode::Monomorphic;
                }
            }
        }
    }
}

void JitEngine::install_compiled(std::uint32_t function_index, JitCompiledCode&& compiled) {
    const auto existing = cache_.find(function_index);
    if (existing != cache_.end()) {
        // Clear IC entries that point at the prior executable / IC slot storage before
        // replacing the cache entry. Retire the prior blob so nested IC callee slots /
        // entry pointers cannot dangle if invalidation races an in-flight nested call.
        invalidate_ic_entries_for_function(function_index, existing->second);
        retired_codes_.push_back(std::move(existing->second));
    }
    cache_[function_index] = std::move(compiled);
}

void JitEngine::bind_bytecode(const Bytecode* bytecode, const std::string& fingerprint) {
    bytecode_ = bytecode;
    if (fingerprint != fingerprint_) {
        cache_.clear();
        retired_codes_.clear();
        optimization_generations_.clear();
        deopt_counts_.clear();
        loop_trip_counts_.clear();
        invocations_since_deopt_.clear();
        eligibility_cache_.clear();
        fingerprint_ = fingerprint;
    }
    if (bytecode_) {
        if (invocations_.size() != bytecode_->functions.size()) {
            invocations_.assign(bytecode_->functions.size(), 0);
        }
        if (eligibility_cache_.size() != bytecode_->functions.size()) {
            eligibility_cache_.assign(bytecode_->functions.size(), 0);
        }
    }
}

bool JitEngine::function_is_jit_eligible(std::uint32_t function_index) {
    if (!bytecode_ || function_index >= bytecode_->functions.size()) {
        return false;
    }
    if (eligibility_cache_.size() != bytecode_->functions.size()) {
        eligibility_cache_.assign(bytecode_->functions.size(), 0);
    }
    if (eligibility_cache_[function_index] == 1) {
        return true;
    }
    if (eligibility_cache_[function_index] == 2) {
        return false;
    }
    std::string reason;
    const bool eligible = jit_function_is_eligible(*bytecode_, function_index, reason);
    eligibility_cache_[function_index] = eligible ? 1 : 2;
    return eligible;
}

void JitEngine::set_profile(const ProfileDocument* profile) {
    profile_ = profile;
    opt_profile_ = profile_ && bytecode_ ? build_jit_opt_profile(profile_, *bytecode_) : JitOptProfile{};
}

void JitEngine::record_invocation(std::uint32_t function_index, const FunctionInfo& fn) {
    if (!bytecode_ || function_index >= invocations_.size()) {
        return;
    }
    ++invocations_[function_index];
    ++invocations_since_deopt_[function_index];
}

bool JitEngine::qualifies_for_optimizing_tier(std::uint32_t function_index, const FunctionInfo& fn) const {
    if (!bytecode_ || function_index >= invocations_.size()) {
        return false;
    }
    if (invocations_[function_index] < config_.optimizing_invocation_threshold) {
        return false;
    }
    const auto since_deopt = invocations_since_deopt_.find(function_index);
    if (since_deopt != invocations_since_deopt_.end() &&
        since_deopt->second < config_.deopt_recompile_invocation_cooldown) {
        const auto deopts = deopt_counts_.find(function_index);
        if (deopts != deopt_counts_.end() && deopts->second > 0) {
            return false;
        }
    }
    if (!config_.require_profile_for_optimizing) {
        return true;
    }
    if (fn.pgo_hotness >= static_cast<std::uint8_t>(ProfileHotness::Hot)) {
        return true;
    }
    if (const FunctionOptHints* hints = opt_profile_.function_hints(function_index)) {
        if (hints->type_integer_unstable) {
            return false;
        }
        if (hints->very_hot || hints->stable_integer_ops || hints->hot_loop_iterations > 0) {
            return true;
        }
    }
    return false;
}

JitTier JitEngine::desired_tier(std::uint32_t function_index, const FunctionInfo& fn) const {
    if (!enabled_ || !bytecode_) {
        return JitTier::Vm;
    }
    if (qualifies_for_optimizing_tier(function_index, fn)) {
        return JitTier::OptimizingJit;
    }
    if (config_.use_pgo_hotness && fn.pgo_hotness >= static_cast<std::uint8_t>(ProfileHotness::Hot)) {
        return JitTier::BaselineJit;
    }
    if (function_index < invocations_.size() &&
        invocations_[function_index] >= config_.baseline_invocation_threshold) {
        return JitTier::BaselineJit;
    }
    return JitTier::Vm;
}

const JitCompiledCode* JitEngine::lookup(std::uint32_t function_index) const {
    const auto it = cache_.find(function_index);
    if (it == cache_.end() || !it->second.valid) {
        return nullptr;
    }
    if (it->second.bytecode_fingerprint != fingerprint_) {
        return nullptr;
    }
    if (it->second.tier == JitTier::OptimizingJit && it->second.metadata && it->second.metadata->invalidated) {
        return nullptr;
    }
    return &it->second;
}

const JitCompiledCode* JitEngine::compile_if_needed(std::uint32_t function_index, const FunctionInfo& fn) {
    const JitTier tier = desired_tier(function_index, fn);
    if (tier == JitTier::Vm) {
        return nullptr;
    }
    if (const JitCompiledCode* existing = lookup(function_index)) {
        if (tier_rank(existing->tier) >= tier_rank(tier)) {
            ++cache_hits_;
            return existing;
        }
    }
    if (!bytecode_) {
        return nullptr;
    }
    ++compile_attempts_;
    std::string error;
    if (tier == JitTier::OptimizingJit) {
        ++optimizing_compile_attempts_;
        const JitOptProfile* profile = opt_profile_.usable ? &opt_profile_ : nullptr;
        if (std::optional<JitCompiledCode> compiled =
                compile_function_optimizing(*bytecode_, function_index, fingerprint_, profile, error)) {
            const std::uint32_t generation = optimization_generations_[function_index] + 1;
            optimization_generations_[function_index] = generation;
            if (compiled->metadata) {
                compiled->metadata->optimization_generation = generation;
            }
            install_compiled(function_index, std::move(*compiled));
            return lookup(function_index);
        }
    }
    if (const JitCompiledCode* baseline = lookup(function_index)) {
        if (baseline->tier == JitTier::BaselineJit) {
            return baseline;
        }
    }
    if (std::optional<JitCompiledCode> compiled =
            compile_function_baseline(*bytecode_, function_index, fingerprint_,
                                      opt_profile_.usable ? &opt_profile_ : nullptr, error)) {
        install_compiled(function_index, std::move(*compiled));
        return lookup(function_index);
    }
    return nullptr;
}

void prepare_exec_state_for_compiled_entry(JitExecState& state, const JitCompiledCode* code) {
    state.completed = 0;
    state.fallback = 0;
    state.deopt_requested = 0;
    if (!state.osr_entry) {
        state.current_insn = 0;
    }
    state.return_value = Value{};
    state.deopt_site_id = 0;
    state.deopt_resume_bytecode_ip = 0;
    state.deopt_ir_insn_index = 0;
    state.deopt_context = nullptr;
    if (code != nullptr && code->metadata) {
        state.deopt_context = &code->metadata->deopt;
    }
    if (code != nullptr && state.instruction_count == 0 && code->ir_instruction_count > 0) {
        state.instruction_count = code->ir_instruction_count;
    }
    if (code != nullptr && code->register_count > state.registers.size()) {
        state.registers.resize(code->register_count);
    }
    if (!state.preserve_registers_on_entry) {
        for (Value& reg : state.registers) {
            reg = Value{};
        }
    }
    refresh_register_data(state);
    state.preserve_registers_on_entry = false;
    const bool osr_entry = state.osr_entry;
    state.osr_entry = false;
    if (code == nullptr) {
        state.call_ic_slots = nullptr;
        state.call_ic_slot_count = 0;
        state.integer_shadow_map = nullptr;
        state.integer_shadow_count = 0;
        return;
    }
    if (!code->call_ic_slots.empty()) {
        state.call_ic_slots = const_cast<JitCallIcSlot*>(code->call_ic_slots.data());
        state.call_ic_slot_count = static_cast<std::uint32_t>(code->call_ic_slots.size());
    } else {
        state.call_ic_slots = nullptr;
        state.call_ic_slot_count = 0;
    }
    if (code->buffered_integer_locals && !code->integer_local_shadow_reg.empty()) {
        state.integer_shadow_map = code->integer_local_shadow_reg.data();
        state.integer_shadow_count = static_cast<std::uint32_t>(code->integer_local_shadow_reg.size());
        if (osr_entry) {
            hydrate_shadow_integer_locals(
                &state, code->integer_local_shadow_reg.data(),
                static_cast<std::uint32_t>(code->integer_local_shadow_reg.size()));
        } else {
            std::uint32_t arity = 0;
            if (state.bytecode != nullptr && state.function_index < state.bytecode->functions.size()) {
                arity = state.bytecode->functions[state.function_index].arity;
            }
            hydrate_shadow_integer_locals_for_entry(
                &state, code->integer_local_shadow_reg.data(),
                static_cast<std::uint32_t>(code->integer_local_shadow_reg.size()), arity);
        }
    } else {
        state.integer_shadow_map = nullptr;
        state.integer_shadow_count = 0;
    }
}

JitExecuteResult execute_compiled_entry(const JitCompiledCode* code, JitExecState& state) {
    if (!code || !code->entry) {
        return JitExecuteResult::Fallback;
    }
    prepare_exec_state_for_compiled_entry(state, code);
    code->entry(&state);
    state.integer_shadow_map = nullptr;
    state.integer_shadow_count = 0;
    // Flush shadow integers only for deopt/fallback. Completed paths already flush at Return
    // via IR StoreLocal; a second post-entry flush races with nested-call scratch reuse of the
    // same JitExecState locals/registers and was observed to AV (0xC0000005) intermittently.
    if (code->buffered_integer_locals && !code->integer_local_shadow_reg.empty() &&
        (state.deopt_requested || state.fallback)) {
        flush_shadow_integer_locals(&state, code->integer_local_shadow_reg.data(),
                                    static_cast<std::uint32_t>(code->integer_local_shadow_reg.size()));
    }
    if (state.deopt_requested) {
        return JitExecuteResult::Deoptimized;
    }
    if (state.fallback) {
        return JitExecuteResult::Fallback;
    }
    if (state.completed) {
        return JitExecuteResult::Completed;
    }
    return JitExecuteResult::Fallback;
}

JitExecuteResult JitEngine::execute(JitExecState& state, std::uint32_t function_index, const FunctionInfo& fn) {
    const JitCompiledCode* code = compile_if_needed(function_index, fn);
    const JitExecuteResult result = execute_compiled_entry(code, state);
    if (result == JitExecuteResult::Completed && state.count_native_completion &&
        native_jit_completions_ == 0) {
        ++native_jit_completions_;
    }
    return result;
}

JitExecuteResult JitEngine::execute_osr(JitExecState& state, std::uint32_t function_index, const FunctionInfo& fn,
                                        std::uint32_t loop_bytecode_ip) {
    const JitCompiledCode* code = compile_if_needed(function_index, fn);
    if (!code || !code->entry || !code->metadata) {
        return JitExecuteResult::Fallback;
    }
    if (code->tier == JitTier::OptimizingJit && code->metadata->invalidated) {
        return JitExecuteResult::Fallback;
    }
    for (const JitOsrEntry& entry : code->metadata->osr_entries) {
        if (entry.loop_bytecode_ip == loop_bytecode_ip) {
            state.osr_entry = true;
            state.current_insn = entry.ir_insn_index;
            const JitExecuteResult result = execute_compiled_entry(code, state);
            if (result == JitExecuteResult::Completed && state.count_native_completion &&
                native_jit_completions_ == 0) {
                ++native_jit_completions_;
            }
            return result;
        }
    }
    return JitExecuteResult::Fallback;
}

void JitEngine::invalidate_optimizing_code(std::uint32_t function_index, JitGuardKind guard_kind) {
    const auto it = cache_.find(function_index);
    if (it != cache_.end() && it->second.tier == JitTier::OptimizingJit) {
        if (it->second.metadata) {
            it->second.metadata->invalidated = true;
        }
        it->second.valid = false;
    }
    ++deopt_counts_[function_index];
    invocations_since_deopt_[function_index] = 0;
    ++optimization_generations_[function_index];
    if (guard_kind == JitGuardKind::IntegerType || guard_kind == JitGuardKind::IntegerBinary) {
        if (bytecode_ && function_index < bytecode_->functions.size()) {
            if (opt_profile_.functions.size() < bytecode_->functions.size()) {
                opt_profile_.functions.resize(bytecode_->functions.size());
            }
            opt_profile_.functions[function_index].type_integer_unstable = true;
            opt_profile_.functions[function_index].stable_integer_ops = false;
            opt_profile_.usable = true;
        }
    }
}

void JitEngine::record_loop_back(std::uint32_t function_index, std::uint32_t loop_bytecode_ip) {
    ++loop_trip_counts_[loop_key(function_index, loop_bytecode_ip)];
}

std::uint64_t JitEngine::loop_trip_count(std::uint32_t function_index, std::uint32_t loop_bytecode_ip) const {
    const auto it = loop_trip_counts_.find(loop_key(function_index, loop_bytecode_ip));
    if (it == loop_trip_counts_.end()) {
        return 0;
    }
    return it->second;
}

std::uint64_t JitEngine::invocation_count(std::uint32_t function_index) const {
    if (function_index >= invocations_.size()) {
        return 0;
    }
    return invocations_[function_index];
}

std::uint32_t JitEngine::optimization_generation(std::uint32_t function_index) const {
    const auto it = optimization_generations_.find(function_index);
    if (it == optimization_generations_.end()) {
        return 0;
    }
    return it->second;
}

}  // namespace pscript::jit
