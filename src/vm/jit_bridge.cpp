#include "compiler/pgo.h"

#include "compiler/bytecode.h"

#include "jit/jit_code.h"
#include "jit/jit_deopt.h"

#include "jit/jit_ic.h"

#include "jit/jit_ir.h"

#include "jit/jit_runtime.h"
#include "jit/jit_helpers.h"

#include "vm/vm.h"



#include <chrono>
#include <memory>



namespace pscript {

namespace {



constexpr std::uint32_t kMaxJitCallDepth = 240;

std::uint32_t bytecode_function_end_ip(const Bytecode& bytecode, std::uint32_t function_index) {
    if (function_index + 1 < bytecode.functions.size()) {
        return bytecode.functions[function_index + 1].entry;
    }
    return static_cast<std::uint32_t>(bytecode.instructions.size());
}

bool bytecode_function_contains_call_value(const Bytecode& bytecode, std::uint32_t function_index) {
    if (function_index >= bytecode.functions.size()) {
        return false;
    }
    const FunctionInfo& fn = bytecode.functions[function_index];
    const std::uint32_t end = bytecode_function_end_ip(bytecode, function_index);
    for (std::uint32_t ip = fn.entry; ip < end && ip < bytecode.instructions.size(); ++ip) {
        if (bytecode.instructions[ip].opcode == Opcode::CallValue) {
            return true;
        }
    }
    return false;
}

bool bytecode_function_has_back_edge_loop(const Bytecode& bytecode, std::uint32_t function_index) {
    if (function_index >= bytecode.functions.size()) {
        return false;
    }
    const FunctionInfo& fn = bytecode.functions[function_index];
    const std::uint32_t end = bytecode_function_end_ip(bytecode, function_index);
    for (std::uint32_t ip = fn.entry; ip < end && ip < bytecode.instructions.size(); ++ip) {
        const Instruction& ins = bytecode.instructions[ip];
        if (ins.opcode == Opcode::Jump && ins.a <= ip) {
            return true;
        }
        if ((ins.opcode == Opcode::JumpIfFalse || ins.opcode == Opcode::JumpIfTrue) && ins.b <= ip) {
            return true;
        }
    }
    return false;
}

bool try_repair_closure_callee_reg(jit::JitExecState& state, std::uint32_t callee_reg, std::uint32_t callee_index,
                                 const FunctionInfo& callee_fn) {
    if (callee_reg >= state.registers.size() || callee_fn.captures.empty()) {
        return false;
    }
    const Value& in_reg = state.registers[callee_reg];
    if (in_reg.kind == ValueKind::Function && in_reg.function &&
        in_reg.function->function_index == callee_index &&
        in_reg.function->captures.size() == callee_fn.captures.size()) {
        return true;
    }
    for (const std::shared_ptr<Cell>& cell : state.locals) {
        if (!cell || !cell->value || cell->value->kind != ValueKind::Function || !cell->value->function) {
            continue;
        }
        const Value& candidate = *cell->value;
        if (candidate.function->function_index == callee_index &&
            candidate.function->captures.size() == callee_fn.captures.size()) {
            state.registers[callee_reg] = candidate;
            jit::refresh_register_data(state);
            return true;
        }
    }
    return false;
}

bool ic_entry_matches(const jit::JitCallIcEntry& entry, std::uint32_t callee_index, const jit::JitCompiledCode& code,
                      const void* closure_token) {

    if (entry.function_index != callee_index || entry.entry != reinterpret_cast<void*>(code.entry)) {

        return false;

    }

    if (entry.closure_token != closure_token) {

        return false;

    }

    if (code.metadata && entry.code_generation != code.metadata->optimization_generation) {

        return false;

    }

    return true;

}

void nested_entry_shadow_prepare(jit::JitExecState& state, const jit::JitCompiledCode* code) {
    if (code == nullptr || !code->buffered_integer_locals || code->integer_local_shadow_reg.empty()) {
        state.integer_shadow_map = nullptr;
        state.integer_shadow_count = 0;
        return;
    }
    state.integer_shadow_map = code->integer_local_shadow_reg.data();
    state.integer_shadow_count = static_cast<std::uint32_t>(code->integer_local_shadow_reg.size());
    jit::hydrate_shadow_integer_locals(&state, code->integer_local_shadow_reg.data(), state.integer_shadow_count);
}

// After integer argument bind: copy parameter registers into shadow registers for mapped locals.
// Locals at or above arity are left for callee entry code (avoids full cell hydration each call).
void nested_entry_shadow_prepare_integer_args(jit::JitExecState& state, const jit::JitCompiledCode* code,
                                              std::uint32_t arity) {
    if (code == nullptr || !code->buffered_integer_locals || code->integer_local_shadow_reg.empty()) {
        state.integer_shadow_map = nullptr;
        state.integer_shadow_count = 0;
        return;
    }
    const std::vector<std::int32_t>& map = code->integer_local_shadow_reg;
    state.integer_shadow_map = map.data();
    state.integer_shadow_count = static_cast<std::uint32_t>(map.size());
    jit::hydrate_shadow_integer_locals_for_entry(&state, map.data(), state.integer_shadow_count, arity);
}

void nested_entry_shadow_prepare_unary_fast(jit::JitExecState& state, const jit::JitCompiledCode* code) {
    nested_entry_shadow_prepare_integer_args(state, code, 1);
}

void nested_entry_shadow_teardown(jit::JitExecState& state, const jit::JitCompiledCode* code) {
    if (code != nullptr && code->buffered_integer_locals && !code->integer_local_shadow_reg.empty() &&
        (state.deopt_requested || state.fallback)) {
        jit::flush_shadow_integer_locals(&state, code->integer_local_shadow_reg.data(),
                                         static_cast<std::uint32_t>(code->integer_local_shadow_reg.size()));
    }
    state.integer_shadow_map = nullptr;
    state.integer_shadow_count = 0;
}

}  // namespace

void Vm::notify_nested_mono_call_failed(jit::JitExecState* callee) {
    if (callee != nullptr && (callee->deopt_requested || callee->fallback)) {
        nested_entry_shadow_teardown(*callee, jit_engine().lookup(callee->function_index));
    }
    invalidate_nested_mono_scratch_cache();
}

bool Vm::run_jit_nested_call(jit::JitExecState& caller, const jit::JitCompiledCode& code,
                             const FunctionInfo& callee_fn,                              std::uint32_t callee_index, std::uint32_t first_arg_reg,
                             std::uint32_t instruction_count, Value& out_result,
                             const std::vector<std::shared_ptr<Cell>>* closure_upvalues) {
    const bool caller_is_scratch = &caller == &nested_jit_call_scratch();
    if (caller_is_scratch) {
        push_nested_jit_scratch_caller();
    }
    jit::JitExecState& arg_caller = caller_is_scratch && !nested_jit_scratch_stack_.empty()
                                        ? nested_jit_scratch_stack_.back()
                                        : caller;
    jit::JitExecState& callee = nested_jit_call_scratch();
    callee.host_vm = this;
    callee.runtime = arg_caller.runtime;
    callee.bytecode = arg_caller.bytecode;
    callee.function_index = callee_index;
    callee.jit_call_depth = arg_caller.jit_call_depth + 1;
    callee.ic_metrics = arg_caller.ic_metrics;
    callee.completed = false;
    callee.fallback = false;
    callee.deopt_requested = false;
    callee.current_insn = 0;
    callee.return_value = Value{};
    if (closure_upvalues != nullptr && !closure_upvalues->empty()) {
        callee.upvalues = *closure_upvalues;
    } else {
        callee.upvalues.clear();
    }
    const std::uint32_t callee_reg_count =
        std::max(callee_fn.register_count, code.register_count > 0 ? code.register_count : callee_fn.register_count);
    if (callee.registers.size() != callee_reg_count) {
        callee.registers.assign(callee_reg_count, Value{});
    } else if (callee_fn.arity < callee.registers.size()) {
        for (std::size_t i = callee_fn.arity; i < callee.registers.size(); ++i) {
            callee.registers[i] = Value{};
        }
    }
    if (nested_jit_scratch_local_count_ != callee_fn.local_count) {
        release_jit_callee_locals(callee);
        nested_jit_scratch_local_count_ = callee_fn.local_count;
    }
    prepare_jit_callee_locals(callee, callee_fn, arg_caller, first_arg_reg);
    mirror_jit_callee_args_to_registers(callee, callee_fn, arg_caller, first_arg_reg);
    callee.instruction_count = instruction_count;
    if (!code.call_ic_slots.empty()) {
        callee.call_ic_slots = const_cast<jit::JitCallIcSlot*>(code.call_ic_slots.data());
        callee.call_ic_slot_count = static_cast<std::uint32_t>(code.call_ic_slots.size());
    } else {
        callee.call_ic_slots = nullptr;
        callee.call_ic_slot_count = 0;
    }
    if (code.metadata) {
        callee.deopt_context = &code.metadata->deopt;
    } else {
        callee.deopt_context = nullptr;
    }
    jit::refresh_register_data(callee);
    if (callee_fn.arity > 0 && callee_fn.arity <= 8) {
        nested_entry_shadow_prepare_integer_args(callee, &code, callee_fn.arity);
    } else {
        nested_entry_shadow_prepare(callee, &code);
    }
    code.entry(&callee);
    nested_entry_shadow_teardown(callee, &code);
    callee.integer_shadow_map = nullptr;
    callee.integer_shadow_count = 0;
    const bool ok = !callee.deopt_requested && !callee.fallback && callee.completed;
    if (ok) {
        out_result = std::move(callee.return_value);
        if (arg_caller.jit_call_depth == 0 && out_result.kind == ValueKind::Function && out_result.function &&
            !out_result.function->captures.empty()) {
            out_result = Runtime::clone_closure_value(out_result);
        }
    }
    if (caller_is_scratch) {
        pop_nested_jit_scratch_caller();
    }
    return ok;
}

jit::JitExecState* Vm::prepare_nested_mono_callee(jit::JitExecState& caller, std::uint32_t first_arg_reg,
                                                  const jit::JitCallIcEntry& entry, std::uint32_t callee_index,
                                                  const FunctionInfo& callee_fn,
                                                  const std::vector<std::shared_ptr<Cell>>* closure_upvalues) {
    if (!entry.entry || caller.jit_call_depth >= kMaxJitCallDepth) {
        return nullptr;
    }
    ensure_jit_engine_bound();
    const jit::JitCompiledCode* compiled = jit_engine().lookup(callee_index);
    const bool caller_is_scratch = &caller == &nested_jit_call_scratch();
    if (caller_is_scratch) {
        push_nested_jit_scratch_caller();
    }
    jit::JitExecState& arg_caller =
        caller_is_scratch && !nested_jit_scratch_stack_.empty() ? nested_jit_scratch_stack_.back() : caller;
    if (entry.function_index != callee_index) {
        if (caller_is_scratch) {
            pop_nested_jit_scratch_caller();
        }
        return nullptr;
    }
    if (callee_fn.captures.size() > 1) {
        if (caller_is_scratch) {
            pop_nested_jit_scratch_caller();
        }
        return nullptr;
    }
    const std::uint32_t callee_reg_count = entry.callee_register_count > 0
                                               ? entry.callee_register_count
                                               : std::max(callee_fn.register_count, callee_fn.arity);
    jit::JitIcMetrics* metrics = arg_caller.ic_metrics;
    const std::int64_t timing_t0 =
        (metrics != nullptr && jit::ic_timing_enabled())
            ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count()
            : 0;
    const void* closure_token = entry.closure_token;
    const bool closure_has_captures = closure_upvalues != nullptr && !closure_upvalues->empty();
    auto upvalues_match_scratch = [&]() -> bool {
        if (!closure_has_captures) {
            return true;
        }
        if (closure_upvalues->size() != 1) {
            return false;
        }
        jit::JitExecState& scratch = nested_jit_call_scratch();
        if (scratch.upvalues.size() != 1) {
            return false;
        }
        return scratch.upvalues[0] == closure_upvalues->at(0);
    };
    if (nested_mono_scratch_warm_ && nested_mono_cache_entry_ == entry.entry &&
        nested_mono_cache_fn_index_ == callee_index && nested_mono_cache_closure_token_ == closure_token &&
        nested_mono_cache_reg_count_ == callee_reg_count &&
        nested_mono_cache_local_count_ == callee_fn.local_count &&
        nested_jit_scratch_local_count_ == callee_fn.local_count && upvalues_match_scratch()) {
        jit::JitExecState& callee = nested_jit_call_scratch();
        callee.jit_call_depth = arg_caller.jit_call_depth + 1;
        callee.completed = false;
        callee.fallback = false;
        callee.deopt_requested = false;
        callee.return_value = Value{};
        callee.current_insn = 0;
        const bool integer_args_bound =
            try_nested_mono_fast_integer_bind(callee, callee_fn, arg_caller, first_arg_reg);
        if (!integer_args_bound) {
            update_nested_mono_callee_args(callee, callee_fn, arg_caller, first_arg_reg);
        }
        callee.instruction_count =
            entry.callee_ir_instruction_count > 0 ? entry.callee_ir_instruction_count : callee.instruction_count;
        callee.call_ic_slots =
            entry.callee_call_ic_slot_count > 0
                ? const_cast<jit::JitCallIcSlot*>(static_cast<const jit::JitCallIcSlot*>(entry.callee_call_ic_slots))
                : nullptr;
        callee.call_ic_slot_count = entry.callee_call_ic_slot_count;
        callee.deopt_context = entry.callee_deopt_context;
        if (closure_upvalues != nullptr) {
            if (callee.upvalues.size() != closure_upvalues->size()) {
                callee.upvalues = *closure_upvalues;
            } else if (!closure_upvalues->empty()) {
                bool same = true;
                for (std::size_t i = 0; i < closure_upvalues->size(); ++i) {
                    if (callee.upvalues[i] != closure_upvalues->at(i)) {
                        same = false;
                        break;
                    }
                }
                if (!same) {
                    callee.upvalues = *closure_upvalues;
                }
            }
        } else {
            callee.upvalues.clear();
        }
        if (metrics != nullptr) {
            ++metrics->prepare_nested_mono_fast;
            if (timing_t0 != 0) {
                const std::int64_t timing_t1 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                   std::chrono::steady_clock::now().time_since_epoch())
                                                   .count();
                metrics->prepare_nested_mono_ns += static_cast<std::uint64_t>(timing_t1 - timing_t0);
            }
        }
        if (!integer_args_bound) {
            jit::refresh_register_data(callee);
        }
        if (integer_args_bound) {
            nested_entry_shadow_prepare_integer_args(callee, compiled, callee_fn.arity);
        } else if (callee_fn.arity == 1) {
            nested_entry_shadow_prepare_unary_fast(callee, compiled);
        } else {
            nested_entry_shadow_prepare(callee, compiled);
        }
        return &callee;
    }
    jit::JitExecState& callee = nested_jit_call_scratch();
    callee.host_vm = this;
    callee.runtime = arg_caller.runtime;
    callee.bytecode = arg_caller.bytecode;
    callee.function_index = callee_index;
    callee.jit_call_depth = arg_caller.jit_call_depth + 1;
    callee.ic_metrics = arg_caller.ic_metrics;
    callee.completed = false;
    callee.fallback = false;
    callee.deopt_requested = false;
    callee.return_value = Value{};
    callee.current_insn = 0;
    if (closure_upvalues != nullptr) {
        if (callee.upvalues.size() != closure_upvalues->size()) {
            callee.upvalues = *closure_upvalues;
        } else if (!closure_upvalues->empty()) {
            bool same = true;
            for (std::size_t i = 0; i < closure_upvalues->size(); ++i) {
                if (callee.upvalues[i] != closure_upvalues->at(i)) {
                    same = false;
                    break;
                }
            }
            if (!same) {
                callee.upvalues = *closure_upvalues;
            }
        }
    } else {
        callee.upvalues.clear();
    }
    if (callee.registers.size() != callee_reg_count) {
        callee.registers.assign(callee_reg_count, Value{});
    } else if (callee_fn.arity < callee.registers.size()) {
        for (std::size_t i = callee_fn.arity; i < callee.registers.size(); ++i) {
            callee.registers[i] = Value{};
        }
    }
    if (nested_jit_scratch_local_count_ != callee_fn.local_count) {
        release_jit_callee_locals(callee);
        nested_jit_scratch_local_count_ = callee_fn.local_count;
        invalidate_nested_mono_scratch_cache();
    }
    const bool used_integer_bind =
        callee_fn.arity > 0 && callee_fn.arity <= 8 &&
        try_nested_mono_fast_integer_bind(callee, callee_fn, arg_caller, first_arg_reg);
    if (!used_integer_bind) {
        update_nested_mono_callee_args(callee, callee_fn, arg_caller, first_arg_reg);
        jit::refresh_register_data(callee);
    }
    callee.instruction_count =
        entry.callee_ir_instruction_count > 0 ? entry.callee_ir_instruction_count : callee.instruction_count;
    callee.call_ic_slots =
        entry.callee_call_ic_slot_count > 0
            ? const_cast<jit::JitCallIcSlot*>(static_cast<const jit::JitCallIcSlot*>(entry.callee_call_ic_slots))
            : nullptr;
    callee.call_ic_slot_count = entry.callee_call_ic_slot_count;
    callee.deopt_context = entry.callee_deopt_context;
    nested_mono_scratch_warm_ = true;
    nested_mono_cache_fn_index_ = callee_index;
    nested_mono_cache_entry_ = entry.entry;
    nested_mono_cache_closure_token_ = closure_token;
    nested_mono_cache_reg_count_ = callee_reg_count;
    nested_mono_cache_local_count_ = callee_fn.local_count;
    if (metrics != nullptr) {
        ++metrics->prepare_nested_mono_full;
        if (timing_t0 != 0) {
            const std::int64_t timing_t1 = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count();
            metrics->prepare_nested_mono_ns += static_cast<std::uint64_t>(timing_t1 - timing_t0);
        }
    }
    if (used_integer_bind) {
        nested_entry_shadow_prepare_integer_args(callee, compiled, callee_fn.arity);
    } else if (callee_fn.arity == 1) {
        nested_entry_shadow_prepare_unary_fast(callee, compiled);
    } else {
        nested_entry_shadow_prepare(callee, compiled);
    }
    return &callee;
}

bool Vm::jit_invoke_nested_from_entry(jit::JitExecState& caller, std::uint32_t dest, std::uint32_t first_arg_reg,
                                      const jit::JitCallIcEntry& entry, const FunctionInfo& callee_fn,
                                      std::uint32_t callee_index,
                                      const std::vector<std::shared_ptr<Cell>>* closure_upvalues) {
    const bool caller_is_scratch = &caller == &nested_jit_call_scratch();
    jit::JitExecState* callee = prepare_nested_mono_callee(caller, first_arg_reg, entry, callee_index, callee_fn,
                                                           closure_upvalues);
    if (callee == nullptr) {
        return false;
    }
    jit::JitNativeEntry callee_entry = reinterpret_cast<jit::JitNativeEntry>(entry.entry);
    if (caller.ic_metrics) {
        ++caller.ic_metrics->nested_jit_direct_entry;
    }
    callee_entry(callee);
    nested_entry_shadow_teardown(*callee, jit_engine().lookup(callee_index));
    const bool deopt = callee->deopt_requested;
    const bool fallback = callee->fallback;
    const bool completed = callee->completed;
    Value result = std::move(callee->return_value);
    if (caller_is_scratch) {
        pop_nested_jit_scratch_caller();
    }
    const bool ok = !deopt && !fallback && completed;
    if (!ok) {
        notify_nested_mono_call_failed(callee);
        if (deopt) {
            caller.deopt_requested = true;
        }
        if (fallback) {
            caller.fallback = true;
        }
        return false;
    }
    publish_nested_mono_call_result(caller, dest, result);
    return true;
}

void Vm::populate_jit_exec_state(jit::JitExecState& state, const FunctionInfo& fn,

                                 std::uint32_t function_index) const {

    state.host_vm = const_cast<Vm*>(this);

    state.runtime = runtime_;

    state.bytecode = &bytecode_;

    state.function_index = function_index;

    state.jit_call_depth = 0;

    state.ic_metrics = &const_cast<jit::JitEngine&>(jit_engine()).ic_metrics();

    std::uint32_t reg_count = fn.register_count;

    if (const jit::JitCompiledCode* compiled = jit_engine().lookup(function_index)) {

        if (compiled->register_count > reg_count) {

            reg_count = compiled->register_count;

        }

        if (compiled->ir_instruction_count > 0) {

            state.instruction_count = compiled->ir_instruction_count;

        }

    } else if (function_index < jit_ir_instruction_counts_.size() &&

               jit_ir_instruction_counts_[function_index] != 0) {

        state.instruction_count = jit_ir_instruction_counts_[function_index];

    }

    state.registers.resize(reg_count);
    jit::refresh_register_data(state);

    state.locals = locals_;

    state.upvalues = upvalues_;

    for (std::uint32_t i = 0; i < fn.register_count; ++i) {
        const std::size_t physical = register_base_ + i;
        if (physical < registers_.size() && registers_[physical].has_value()) {
            state.registers[i] = *registers_[physical];
        }
    }

    state.preserve_registers_on_entry = true;
    state.count_native_completion = false;

}



bool Vm::dispatch_jit_execution_result(jit::JitExecState& state, const FunctionInfo& fn,

                                       std::uint32_t function_index, jit::JitExecuteResult result) {

    switch (result) {

        case jit::JitExecuteResult::Completed:

            finish_jit_return(std::move(state.return_value), fn, function_index);

            return true;

        case jit::JitExecuteResult::Deoptimized: {

            jit::JitEngine& engine = jit_engine();

            if (state.deopt_context && state.deopt_site_id < state.deopt_context->sites.size()) {

                engine.invalidate_optimizing_code(function_index,

                                                  state.deopt_context->sites[state.deopt_site_id].guard_kind);

            } else {

                engine.invalidate_optimizing_code(function_index, jit::JitGuardKind::IntegerType);

            }

            resume_after_jit_deopt(state, fn, function_index);

            return true;

        }

        case jit::JitExecuteResult::Fallback:

        default:

            return false;

    }

}



bool Vm::function_may_use_jit(std::uint32_t function_index) {

    if (!jit_enabled_) {

        return false;

    }

    if (function_index >= bytecode_.functions.size()) {

        return false;

    }

    if (jit_eligibility_hints_.size() != bytecode_.functions.size()) {

        jit_eligibility_hints_.assign(bytecode_.functions.size(), 0);

    }

    if (jit_eligibility_hints_[function_index] == 1) {

        if (jit_ir_instruction_counts_.size() != bytecode_.functions.size()) {

            jit_ir_instruction_counts_.assign(bytecode_.functions.size(), 0);

        }

        if (jit_ir_instruction_counts_[function_index] == 0) {

            jit::JitFunctionIr ir;

            std::string error;

            if (jit::build_jit_ir_from_bytecode(bytecode_, function_index, ir, error)) {

                jit_ir_instruction_counts_[function_index] = static_cast<std::uint32_t>(ir.instructions.size());

            }

        }

        return true;

    }

    if (jit_eligibility_hints_[function_index] == 2) {

        return false;

    }

    std::string reason;

    if (!jit::jit_function_is_eligible(bytecode_, function_index, reason)) {

        jit_eligibility_hints_[function_index] = 2;

        return false;

    }

    jit_eligibility_hints_[function_index] = 1;

    if (jit_ir_instruction_counts_.size() != bytecode_.functions.size()) {

        jit_ir_instruction_counts_.assign(bytecode_.functions.size(), 0);

    }

    if (jit_ir_instruction_counts_[function_index] == 0) {

        jit::JitFunctionIr ir;

        std::string error;

        if (jit::build_jit_ir_from_bytecode(bytecode_, function_index, ir, error)) {

            jit_ir_instruction_counts_[function_index] = static_cast<std::uint32_t>(ir.instructions.size());

        }

    }

    return true;

}



bool Vm::try_execute_jit_for_entered_function(const FunctionInfo& fn, std::uint32_t function_index) {

    if (frames_.size() > 64) {

        return false;

    }

    if (!function_may_use_jit(function_index)) {

        return false;

    }

    ensure_jit_engine_bound();

    jit::JitEngine& engine = jit_engine();

    (void)engine.compile_if_needed(function_index, fn);

    if (const jit::JitCompiledCode* compiled = engine.lookup(function_index)) {

        if (jit_ir_instruction_counts_.size() <= function_index) {

            jit_ir_instruction_counts_.resize(function_index + 1, 0);

        }

        jit_ir_instruction_counts_[function_index] = compiled->ir_instruction_count;

    }

    jit::JitExecState state;

    populate_jit_exec_state(state, fn, function_index);
    const jit::JitCompiledCode* compiled_for_count = engine.lookup(function_index);
    state.count_native_completion =
        compiled_for_count != nullptr && compiled_for_count->tier == jit::JitTier::OptimizingJit &&
        bytecode_function_has_back_edge_loop(bytecode_, function_index) &&
        bytecode_function_contains_call_value(bytecode_, function_index) && frames_.size() <= 2;

    nested_jit_scratch_stack_.clear();
    nested_jit_call_scratch_ = jit::JitExecState{};
    nested_jit_scratch_local_count_ = 0xFFFFFFFFu;
    invalidate_nested_mono_scratch_cache();

    const jit::JitExecuteResult result = engine.execute(state, function_index, fn);

    return dispatch_jit_execution_result(state, fn, function_index, result);

}



bool Vm::resume_after_jit_deopt(jit::JitExecState& state, const FunctionInfo& fn, std::uint32_t function_index) {

    locals_ = std::move(state.locals);

    upvalues_ = std::move(state.upvalues);

    const std::size_t needed = register_base_ + fn.register_count;
    if (registers_.size() < needed) {
        registers_.resize(needed);
    }
    for (std::uint32_t i = 0; i < fn.register_count && i < state.registers.size(); ++i) {
        registers_[register_base_ + i] = state.registers[i];
    }

    jump_to(state.deopt_resume_bytecode_ip);

    return true;

}



bool Vm::try_jit_osr_at_loop(std::uint32_t loop_bytecode_ip) {

    if (!jit_enabled_ || frames_.empty()) {

        return false;

    }

    const std::uint32_t function_index = frames_.back().function_index;

    if (!function_may_use_jit(function_index)) {

        return false;

    }

    const FunctionInfo& fn = bytecode_.functions[function_index];

    if (bytecode_function_contains_call_value(bytecode_, function_index)) {
        return false;
    }

    ensure_jit_engine_bound();

    jit::JitEngine& engine = jit_engine();

    engine.record_loop_back(function_index, loop_bytecode_ip);

    if (engine.loop_trip_count(function_index, loop_bytecode_ip) < engine.config().osr_loop_trip_threshold) {

        return false;

    }

    jit::JitExecState state;

    populate_jit_exec_state(state, fn, function_index);

    const jit::JitExecuteResult result = engine.execute_osr(state, function_index, fn, loop_bytecode_ip);

    if (result == jit::JitExecuteResult::Completed) {

        return dispatch_jit_execution_result(state, fn, function_index, result);

    }

    if (result == jit::JitExecuteResult::Deoptimized) {

        dispatch_jit_execution_result(state, fn, function_index, result);

        return false;

    }

    return false;

}



bool Vm::jit_enabled() const {

    return jit_enabled_;

}



jit::JitEngine& Vm::jit_engine() {

    if (!jit_engine_) {

        jit_engine_ = std::make_unique<jit::JitEngine>();

        jit_engine_->set_enabled(jit_enabled_);

        if (jit_config_.has_value()) {

            jit_engine_->set_config(*jit_config_);

        }

    }

    return *jit_engine_;

}



const jit::JitEngine& Vm::jit_engine() const {

    return const_cast<Vm*>(this)->jit_engine();

}



void Vm::set_jit_enabled(bool enabled) {

    jit_enabled_ = enabled;

    if (jit_engine_) {

        jit_engine_->set_enabled(enabled);

    }

}



void Vm::set_jit_config(jit::JitTieringConfig config) {

    jit_config_ = std::move(config);

    if (jit_engine_) {

        jit_engine_->set_config(*jit_config_);

    }

}



bool Vm::jit_invoke_direct_call(jit::JitExecState& state, std::uint32_t dest, std::uint32_t first_arg_reg,

                                std::uint32_t callee_index, jit::JitCallIcSlot* ic_slot,

                                const ClosureData* closure) {

    if (!state.bytecode || callee_index >= state.bytecode->functions.size()) {

        return false;

    }

    if (state.jit_call_depth >= kMaxJitCallDepth) {

        return false;

    }

    if (callee_index == state.function_index && closure == nullptr) {
        if (state.jit_call_depth >= 1) {
            return jit_invoke_call_from_interpreter(state, dest, first_arg_reg, callee_index);
        }
    }

    const FunctionInfo& callee_fn = state.bytecode->functions[callee_index];

    if (callee_fn.is_native || callee_fn.is_async || callee_fn.is_constructor || callee_fn.has_implicit_this) {

        return false;

    }

    if (!callee_fn.captures.empty()) {

        if (closure == nullptr || closure->captures.size() != callee_fn.captures.size()) {

            return false;

        }

    } else if (closure != nullptr && !closure->captures.empty()) {

        return false;

    }

    const void* closure_token = closure != nullptr ? static_cast<const void*>(closure) : nullptr;

    ensure_jit_engine_bound();

    jit::JitEngine& engine = jit_engine();



    const jit::JitCompiledCode* code = nullptr;

    bool ic_fast_hit = false;



    if (ic_slot && ic_slot->mode == jit::JitCallIcMode::Monomorphic &&

        ic_slot->mono.function_index == callee_index && ic_slot->mono.entry != nullptr) {

        code = engine.lookup(callee_index);

        if (code && code->entry && ic_entry_matches(ic_slot->mono, callee_index, *code, closure_token)) {

            jit::record_call_ic_hit(ic_slot, state.ic_metrics, true, false);

            if (ic_slot->mono.callee_register_count == 0) {
                jit::hydrate_call_ic_entry(ic_slot->mono, callee_index, code, closure_token, state.bytecode);
            }

            ic_fast_hit = true;

        } else {

            code = nullptr;

        }

    } else if (ic_slot && ic_slot->mode == jit::JitCallIcMode::Polymorphic) {

        code = engine.lookup(callee_index);

        if (code && code->entry) {

            for (std::size_t pi = 0; pi < ic_slot->poly_count; ++pi) {

                const jit::JitCallIcEntry& entry = ic_slot->poly[pi];

                if (entry.entry != nullptr && ic_entry_matches(entry, callee_index, *code, closure_token)) {

                    jit::record_call_ic_hit(ic_slot, state.ic_metrics, false, true);

                    if (ic_slot->poly[pi].callee_register_count == 0) {
                        jit::hydrate_call_ic_entry(ic_slot->poly[pi], callee_index, code, closure_token,
                                                   state.bytecode);
                    }

                    ic_fast_hit = true;

                    break;

                }

            }

        }

        if (!ic_fast_hit) {

            code = nullptr;

        }

    }



    if (!ic_fast_hit) {

        if (!engine.function_is_jit_eligible(callee_index)) {

            return false;

        }

        code = engine.compile_if_needed(callee_index, callee_fn);

        if (!code || !code->entry) {

            return false;

        }

        if (ic_slot) {

            if (ic_slot->mode == jit::JitCallIcMode::Megamorphic) {

                if (state.ic_metrics) {

                    ++state.ic_metrics->call_megamorphic;

                }

            } else if (ic_slot->mode == jit::JitCallIcMode::Polymorphic) {

                bool found = false;

                for (std::size_t pi = 0; pi < ic_slot->poly_count; ++pi) {

                    const jit::JitCallIcEntry& entry = ic_slot->poly[pi];

                    if (entry.function_index == callee_index &&

                        entry.entry == reinterpret_cast<void*>(code->entry) &&

                        entry.closure_token == closure_token) {

                        found = true;

                        break;

                    }

                }

                if (found) {

                    jit::record_call_ic_hit(ic_slot, state.ic_metrics, false, true);

                } else {

                    jit::record_call_ic_miss(ic_slot, state.ic_metrics);

                    jit::update_call_ic(ic_slot, callee_index, code, closure_token, state.bytecode);

                }

            } else if (ic_slot->mode == jit::JitCallIcMode::Monomorphic &&

                       ic_slot->mono.function_index == callee_index &&

                       ic_slot->mono.entry == reinterpret_cast<void*>(code->entry) &&

                       ic_slot->mono.closure_token == closure_token) {

                jit::record_call_ic_hit(ic_slot, state.ic_metrics, true, false);

            } else {

                jit::record_call_ic_miss(ic_slot, state.ic_metrics);

                jit::update_call_ic(ic_slot, callee_index, code, closure_token, state.bytecode);

            }

        }

    }



    const std::vector<std::shared_ptr<Cell>>* upvalues =
        closure != nullptr && !closure->captures.empty() ? &closure->captures : nullptr;

    const jit::JitCallIcEntry* fast_entry = nullptr;
    if (ic_slot && ic_fast_hit) {
        if (ic_slot->mode == jit::JitCallIcMode::Monomorphic && ic_slot->mono.entry != nullptr) {
            fast_entry = &ic_slot->mono;
        } else if (ic_slot->mode == jit::JitCallIcMode::Polymorphic && code != nullptr) {
            for (std::size_t pi = 0; pi < ic_slot->poly_count; ++pi) {
                const jit::JitCallIcEntry& entry = ic_slot->poly[pi];
                if (entry.entry != nullptr && ic_entry_matches(entry, callee_index, *code, closure_token)) {
                    fast_entry = &ic_slot->poly[pi];
                    break;
                }
            }
        }
    }

    if (fast_entry != nullptr && fast_entry->callee_register_count > 0 &&
        jit_invoke_nested_from_entry(state, dest, first_arg_reg, *fast_entry, callee_fn, callee_index, upvalues)) {
        return true;
    }

    if (state.ic_metrics) {
        ++state.ic_metrics->nested_jit_helper_invoke;
    }

    std::uint32_t callee_insn_count = code->ir_instruction_count;

    if (callee_insn_count == 0) {

        if (callee_index < jit_ir_instruction_counts_.size()) {

            callee_insn_count = jit_ir_instruction_counts_[callee_index];

        }

    }

    Value result;

    if (!run_jit_nested_call(state, *code, callee_fn, callee_index, first_arg_reg, callee_insn_count, result,
                             upvalues)) {
        if (&state == &nested_jit_call_scratch()) {
            return jit_invoke_call_from_interpreter(state, dest, first_arg_reg, callee_index);
        }
        return false;
    }

    if (dest >= state.registers.size()) {

        state.registers.resize(dest + 1);
        jit::refresh_register_data(state);

    }

    state.registers[dest] = std::move(result);
    jit::refresh_register_data(state);
    if (state.integer_shadow_map != nullptr && state.integer_shadow_count > 0) {
        jit::hydrate_shadow_integer_locals(&state, state.integer_shadow_map, state.integer_shadow_count);
    }

    return true;

}

bool Vm::jit_invoke_call_from_interpreter(jit::JitExecState& state, std::uint32_t dest, std::uint32_t first_arg_reg,
                                          std::uint32_t callee_index) {
    if (!state.bytecode || callee_index >= state.bytecode->functions.size()) {
        return false;
    }
    ++jit_vm_fallback_depth_;
    const std::size_t needed = register_base_ + state.registers.size();
    if (registers_.size() < needed) {
        registers_.resize(needed);
    }
    for (std::size_t i = 0; i < state.registers.size(); ++i) {
        registers_[register_base_ + i] = state.registers[i];
    }
    locals_ = state.locals;

    Instruction insn{};
    insn.opcode = Opcode::Call;
    insn.a = dest;
    insn.b = first_arg_reg;
    insn.c = callee_index;

    const std::size_t depth_before = frames_.size();
    try {
        call_function(insn);
    } catch (...) {
        --jit_vm_fallback_depth_;
        return false;
    }

    while (frames_.size() > depth_before) {
        if (halted_ || debug_execution_paused_) {
            --jit_vm_fallback_depth_;
            return false;
        }
        if (ip_ >= code().instructions.size()) {
            --jit_vm_fallback_depth_;
            fail("invalid bytecode");
        }
        if (debug_hook_ != nullptr) {
            if (!step_one_instruction()) {
                --jit_vm_fallback_depth_;
                return false;
            }
            continue;
        }
        execute(fetch());
    }

    --jit_vm_fallback_depth_;

    const std::size_t window = state.registers.size();
    for (std::size_t i = 0; i < window; ++i) {
        const std::size_t physical = register_base_ + i;
        if (physical < registers_.size() && registers_[physical].has_value()) {
            state.registers[i] = *registers_[physical];
        } else {
            state.registers[i] = Runtime::make_null();
        }
    }
    jit::refresh_register_data(state);
    state.locals = locals_;
    return true;
}

bool Vm::jit_invoke_call_value_from_interpreter(jit::JitExecState& state, std::uint32_t dest, std::uint32_t callee_reg,
                                                std::uint32_t argc) {
    if (callee_reg >= state.registers.size()) {
        return false;
    }
    ++jit_vm_fallback_depth_;
    const std::size_t needed = register_base_ + state.registers.size();
    if (registers_.size() < needed) {
        registers_.resize(needed);
    }
    for (std::size_t i = 0; i < state.registers.size(); ++i) {
        registers_[register_base_ + i] = state.registers[i];
    }
    locals_ = state.locals;

    Instruction insn{};
    insn.opcode = Opcode::CallValue;
    insn.a = dest;
    insn.b = callee_reg;
    insn.c = argc;

    const std::size_t depth_before = frames_.size();
    try {
        call_value(insn);
    } catch (...) {
        --jit_vm_fallback_depth_;
        return false;
    }

    while (frames_.size() > depth_before) {
        if (halted_ || debug_execution_paused_) {
            --jit_vm_fallback_depth_;
            return false;
        }
        if (ip_ >= code().instructions.size()) {
            --jit_vm_fallback_depth_;
            fail("invalid bytecode");
        }
        if (debug_hook_ != nullptr) {
            if (!step_one_instruction()) {
                --jit_vm_fallback_depth_;
                return false;
            }
            continue;
        }
        execute(fetch());
    }

    --jit_vm_fallback_depth_;

    const std::size_t window = state.registers.size();
    for (std::size_t i = 0; i < window; ++i) {
        const std::size_t physical = register_base_ + i;
        if (physical < registers_.size() && registers_[physical].has_value()) {
            state.registers[i] = *registers_[physical];
        } else {
            state.registers[i] = Runtime::make_null();
        }
    }
    jit::refresh_register_data(state);
    state.locals = locals_;
    state.upvalues = upvalues_;
    return true;
}

bool Vm::jit_execute_call_value(jit::JitExecState& state, std::uint32_t dest, std::uint32_t callee_reg,

                                std::uint32_t argc, jit::JitCallIcSlot* ic_slot) {

    if (callee_reg >= state.registers.size()) {

        return false;

    }

    const Value& callee = state.registers[callee_reg];

    if (callee.kind != ValueKind::Function || !callee.function) {

        return false;

    }

    if (callee.function->module_index != 0xFFFFFFFFu) {

        return false;

    }

    if (callee.function->function_index >= bytecode_.functions.size()) {

        return false;

    }

    const std::uint32_t callee_index = callee.function->function_index;

    const FunctionInfo& callee_fn = bytecode_.functions[callee_index];

    if (callee_fn.is_native || callee_fn.is_async || callee_fn.is_constructor) {

        return false;

    }

    if (argc != callee_fn.arity) {

        return false;

    }

    if (!callee_fn.captures.empty()) {
        if (callee.function->captures.size() != callee_fn.captures.size()) {
            try_repair_closure_callee_reg(state, callee_reg, callee_index, callee_fn);
        }
        if (callee_fn.captures.size() > 1) {
            return jit_invoke_call_value_from_interpreter(state, dest, callee_reg, argc);
        }
    }

    if (jit_invoke_direct_call(state, dest, callee_reg + 1, callee_index, ic_slot, callee.function.get())) {

        return true;

    }

    return jit_invoke_call_value_from_interpreter(state, dest, callee_reg, argc);

}



void Vm::finish_jit_return(Value result, const FunctionInfo& fn, std::uint32_t function_index) {

    std::uint32_t return_reg = 0;

    const std::uint32_t start = fn.entry;

    std::uint32_t end = code().instructions.size();

    if (function_index + 1 < code().functions.size()) {

        end = code().functions[function_index + 1].entry;

    }

    for (std::uint32_t ip = start; ip < end && ip < code().instructions.size(); ++ip) {

        if (code().instructions[ip].opcode == Opcode::Return) {

            return_reg = code().instructions[ip].a;

            break;

        }

    }

    write_register(return_reg) = std::move(result);

    Instruction ret_insn;

    ret_insn.opcode = Opcode::Return;

    ret_insn.a = return_reg;

    return_from_function(ret_insn);

}



}  // namespace pscript

