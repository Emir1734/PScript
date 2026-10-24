#include "jit/jit_compiler.h"

#include "compiler/bytecode.h"
#include "jit/jit_backend.h"
#include "jit/jit_deopt.h"
#include "jit/jit_ir.h"
#include "jit/jit_opt.h"
#include "jit/jit_opt_profile.h"

#include <algorithm>

namespace pscript::jit {

namespace {

std::uint32_t bytecode_function_end_ip(const Bytecode& bytecode, std::uint32_t function_index) {
    if (function_index + 1 < bytecode.functions.size()) {
        return bytecode.functions[function_index + 1].entry;
    }
    return static_cast<std::uint32_t>(bytecode.instructions.size());
}

bool bytecode_function_stores_upvalue(const Bytecode& bytecode, std::uint32_t function_index) {
    if (function_index >= bytecode.functions.size()) {
        return false;
    }
    const FunctionInfo& fn = bytecode.functions[function_index];
    const std::uint32_t end = bytecode_function_end_ip(bytecode, function_index);
    for (std::uint32_t ip = fn.entry; ip < end && ip < bytecode.instructions.size(); ++ip) {
        if (bytecode.instructions[ip].opcode == Opcode::StoreUpvalue) {
            return true;
        }
    }
    return false;
}

bool ir_callvalue_targets_mutating_closure(const Bytecode& bytecode, const JitFunctionIr& ir) {
    std::vector<std::optional<std::uint32_t>> reg_closure_fn(ir.register_count);
    std::vector<std::optional<std::uint32_t>> local_closure_fn(ir.local_count);
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::MakeClosure && insn.a < reg_closure_fn.size()) {
            reg_closure_fn[insn.a] = insn.b;
        } else if (insn.opcode == JitIrOpcode::MoveReg && insn.a < reg_closure_fn.size() &&
                   insn.b < reg_closure_fn.size() && reg_closure_fn[insn.b]) {
            reg_closure_fn[insn.a] = reg_closure_fn[insn.b];
        } else if (insn.opcode == JitIrOpcode::StoreLocal && insn.a < local_closure_fn.size() &&
                   insn.b < reg_closure_fn.size() && reg_closure_fn[insn.b]) {
            local_closure_fn[insn.a] = reg_closure_fn[insn.b];
        } else if (insn.opcode == JitIrOpcode::LoadLocal && insn.b < local_closure_fn.size() &&
                   insn.a < reg_closure_fn.size() && local_closure_fn[insn.b]) {
            reg_closure_fn[insn.a] = local_closure_fn[insn.b];
        }
    }
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::CallValue || insn.b >= reg_closure_fn.size() || !reg_closure_fn[insn.b]) {
            continue;
        }
        const std::uint32_t callee = *reg_closure_fn[insn.b];
        if (bytecode_function_stores_upvalue(bytecode, callee)) {
            return true;
        }
    }
    return false;
}

bool needs_conservative_callvalue_loop(const Bytecode& bytecode, const JitFunctionIr& ir);

bool ir_callvalue_targets_callee_with_callvalue(const Bytecode& bytecode, const JitFunctionIr& ir) {
    std::vector<std::optional<std::uint32_t>> reg_closure_fn(ir.register_count);
    std::vector<std::optional<std::uint32_t>> local_closure_fn(ir.local_count);
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::MakeClosure && insn.a < reg_closure_fn.size()) {
            reg_closure_fn[insn.a] = insn.b;
        } else if (insn.opcode == JitIrOpcode::LoadFunction && insn.a < reg_closure_fn.size()) {
            reg_closure_fn[insn.a] = insn.b;
        } else if (insn.opcode == JitIrOpcode::MoveReg && insn.a < reg_closure_fn.size() &&
                   insn.b < reg_closure_fn.size() && reg_closure_fn[insn.b]) {
            reg_closure_fn[insn.a] = reg_closure_fn[insn.b];
        } else if (insn.opcode == JitIrOpcode::StoreLocal && insn.a < local_closure_fn.size() &&
                   insn.b < reg_closure_fn.size() && reg_closure_fn[insn.b]) {
            local_closure_fn[insn.a] = reg_closure_fn[insn.b];
        } else if (insn.opcode == JitIrOpcode::LoadLocal && insn.b < local_closure_fn.size() &&
                   insn.a < reg_closure_fn.size() && local_closure_fn[insn.b]) {
            reg_closure_fn[insn.a] = local_closure_fn[insn.b];
        }
    }
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::CallValue || insn.b >= reg_closure_fn.size() || !reg_closure_fn[insn.b]) {
            continue;
        }
        const std::uint32_t callee = *reg_closure_fn[insn.b];
        if (callee == ir.function_index) {
            return true;
        }
        JitFunctionIr callee_ir;
        std::string error;
        if (build_jit_ir_from_bytecode(bytecode, callee, callee_ir, error) && ir_has_call_value_sites(callee_ir)) {
            return true;
        }
    }
    return false;
}

bool needs_conservative_callvalue(const Bytecode& bytecode, const JitFunctionIr& ir) {
    if (!ir_has_call_value_sites(ir)) {
        return false;
    }
    if (ir_callvalue_sites_are_stable_direct(bytecode, ir)) {
        return false;
    }
    if (ir_callvalue_is_only_self_recursive(bytecode, ir)) {
        return false;
    }
    if (ir_callvalue_targets_callee_with_callvalue(bytecode, ir)) {
        return true;
    }
    return needs_conservative_callvalue_loop(bytecode, ir);
}

bool needs_conservative_callvalue_loop(const Bytecode& bytecode, const JitFunctionIr& ir) {
    if (!ir_has_call_value_sites(ir)) {
        return false;
    }
    const bool has_loop_headers =
        std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& block) { return block.loop_header; });
    if (!has_loop_headers) {
        return false;
    }
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        if (ir.instructions[i].opcode == JitIrOpcode::CallValue && instruction_in_backedge_loop(ir, i)) {
            return true;
        }
    }
    if (ir_callvalue_targets_mutating_closure(bytecode, ir)) {
        return true;
    }
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        if (ir.instructions[i].opcode == JitIrOpcode::MakeClosure && instruction_in_backedge_loop(ir, i)) {
            return true;
        }
    }
    return false;
}

bool run_conservative_callvalue_pipeline(const Bytecode& bytecode, JitFunctionIr& ir, std::string& error) {
    apply_stable_call_value_lowering(bytecode, ir);
    build_cfg_for_function(ir);
    if (ir.instructions.empty()) {
        error = "optimizer removed all instructions";
        return false;
    }
    return true;
}

}  // namespace

bool prepare_function_ir_for_jit_compile(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                                         bool /*stable_direct_callvalue_hint*/, OptimizingPassStats& stats,
                                         std::string& error, bool& conservative_dispatch_emit) {
    build_cfg_for_function(ir);
    const bool stable_direct_callvalue = ir_callvalue_sites_are_stable_direct(bytecode, ir);
    ir.stable_direct_callvalue = stable_direct_callvalue;
    const bool has_calls = ir_has_call_sites(ir) || ir_has_call_value_sites(ir);
    conservative_dispatch_emit = false;
    ir.conservative_dispatch_emit = false;
    if (!has_calls) {
        const bool has_loop_headers =
            std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& b) { return b.loop_header; });
        bool needs_full_pipeline = has_loop_headers;
        if (!needs_full_pipeline) {
            for (const JitIrInstruction& insn : ir.instructions) {
                if (insn.opcode == JitIrOpcode::Equal || insn.opcode == JitIrOpcode::NotEqual ||
                    insn.opcode == JitIrOpcode::StringEqual || insn.opcode == JitIrOpcode::StringNotEqual) {
                    needs_full_pipeline = true;
                    break;
                }
                if (insn.opcode == JitIrOpcode::LoadConst && insn.b < bytecode.constants.size() &&
                    bytecode.constants[insn.b].kind == ConstantKind::String) {
                    needs_full_pipeline = true;
                    break;
                }
            }
        }
        if (!needs_full_pipeline && profile != nullptr) {
            if (const FunctionOptHints* hints = profile->function_hints(ir.function_index)) {
                if (hints->stable_integer_ops && !has_loop_headers) {
                    bool leaf_numeric_only = true;
                    for (const JitIrInstruction& insn : ir.instructions) {
                        switch (insn.opcode) {
                            case JitIrOpcode::Nop:
                            case JitIrOpcode::LoadConst:
                            case JitIrOpcode::LoadConstInt:
                            case JitIrOpcode::LoadLocal:
                            case JitIrOpcode::StoreLocal:
                            case JitIrOpcode::MoveReg:
                            case JitIrOpcode::Add:
                            case JitIrOpcode::Sub:
                            case JitIrOpcode::Mul:
                            case JitIrOpcode::Div:
                            case JitIrOpcode::Mod:
                            case JitIrOpcode::IntAdd:
                            case JitIrOpcode::IntSub:
                            case JitIrOpcode::IntMul:
                            case JitIrOpcode::IntDiv:
                            case JitIrOpcode::IntMod:
                            case JitIrOpcode::IntLess:
                            case JitIrOpcode::IntEqual:
                            case JitIrOpcode::GuardInteger:
                            case JitIrOpcode::Jump:
                            case JitIrOpcode::JumpIfFalse:
                            case JitIrOpcode::Return:
                                continue;
                            default:
                                leaf_numeric_only = false;
                                break;
                        }
                        if (!leaf_numeric_only) {
                            break;
                        }
                    }
                    if (leaf_numeric_only) {
                        needs_full_pipeline = true;
                    }
                }
            }
        }
        if (needs_full_pipeline) {
            return run_optimizing_pipeline(bytecode, ir, profile, stats, error);
        }
        apply_stable_call_value_lowering(bytecode, ir);
        apply_baseline_loop_integer_ops(bytecode, ir);
        build_cfg_for_function(ir);
        buffer_integer_locals(bytecode, ir, stats);
        if (ir_may_use_baseline_numeric_loop_fast_path(bytecode, ir)) {
            ir.baseline_numeric_loop_fast_path = true;
        }
        return true;
    }
    if (stable_direct_callvalue) {
        if (!run_optimizing_pipeline(bytecode, ir, profile, stats, error)) {
            return false;
        }
        if (!finish_stable_direct_callvalue_loop_compile(bytecode, ir, profile, stats, error)) {
            return false;
        }
        conservative_dispatch_emit = ir_has_call_sites(ir) || ir_has_call_value_sites(ir);
        ir.conservative_dispatch_emit = conservative_dispatch_emit;
        return true;
    }
    bool conservative_callvalue = needs_conservative_callvalue(bytecode, ir);
    if (conservative_callvalue) {
        if (!run_conservative_callvalue_pipeline(bytecode, ir, error)) {
            return false;
        }
        if (!conservative_callvalue && !ir_has_call_value_sites(ir) && ir_has_call_sites(ir) &&
            !run_optimizing_pipeline(bytecode, ir, nullptr, stats, error)) {
            return false;
        }
        conservative_dispatch_emit = ir_has_call_sites(ir) || ir_has_call_value_sites(ir);
        ir.conservative_dispatch_emit = conservative_dispatch_emit;
    } else if (!run_optimizing_pipeline(bytecode, ir, profile, stats, error)) {
        return false;
    }
    if (stable_direct_callvalue &&
        !finish_stable_direct_callvalue_loop_compile(bytecode, ir, profile, stats, error)) {
        return false;
    }
    if (!ir_has_call_sites(ir) && !ir_has_call_value_sites(ir)) {
        conservative_dispatch_emit = false;
        ir.conservative_dispatch_emit = false;
    }
    return true;
}

namespace {

bool ir_requires_jit_deopt_metadata(const JitFunctionIr& ir) {
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::GuardInteger || insn.opcode == JitIrOpcode::GuardValueKind ||
            insn.opcode == JitIrOpcode::GuardFieldObject) {
            return true;
        }
    }
    return false;
}

bool emit_compiled_function_ir(const Bytecode& bytecode, JitFunctionIr& ir, JitCodeBuffer& buffer, JitCompiledCode& compiled,
                               JitTier tier, bool conservative_dispatch_emit, std::string& error) {
    assign_call_ic_slots(ir, compiled.call_ic_slots);
    const bool calls_remain = ir_has_call_sites(ir) || ir_has_call_value_sites(ir);
    const bool linear_without_deopt = ir.baseline_numeric_loop_fast_path && !ir_requires_jit_deopt_metadata(ir) &&
                                      !calls_remain;
    const bool use_linear_native_emit =
        linear_without_deopt ||
        (tier == JitTier::BaselineJit && !calls_remain && !ir_requires_jit_deopt_metadata(ir));
    if (use_linear_native_emit) {
        if (!compile_baseline_jit(ir, &bytecode, buffer, compiled, error)) {
            return false;
        }
        compiled.tier = tier;
        return true;
    }
    if (conservative_dispatch_emit) {
        if (!compile_baseline_jit(ir, &bytecode, buffer, compiled, error)) {
            return false;
        }
        compiled.tier = tier;
        return true;
    }
    auto metadata = std::make_shared<JitCompiledMetadata>();
    build_deopt_metadata(ir, *metadata);
    assign_deopt_site_ids(ir, *metadata);
    compiled.metadata = std::move(metadata);
    if (!compile_optimizing_jit(ir, &bytecode, buffer, compiled, error)) {
        return false;
    }
    compiled.tier = tier;
    return true;
}

}  // namespace

std::optional<JitCompiledCode> compile_function_baseline(const Bytecode& bytecode, std::uint32_t function_index,
                                                         const std::string& fingerprint, const JitOptProfile* profile,
                                                         std::string& error) {
    if (!jit_backend_available()) {
        error = "JIT backend unavailable";
        return std::nullopt;
    }
    JitFunctionIr ir;
    if (!build_jit_ir_from_bytecode(bytecode, function_index, ir, error)) {
        return std::nullopt;
    }
    const bool stable_direct_callvalue = ir_callvalue_sites_are_stable_direct(bytecode, ir);
    OptimizingPassStats stats;
    bool conservative_dispatch_emit = false;
    if (!prepare_function_ir_for_jit_compile(bytecode, ir, profile, stable_direct_callvalue, stats, error,
                                             conservative_dispatch_emit)) {
        return std::nullopt;
    }
    // Baseline tier does not emit optimizing type guards; integer-local shadow buffering would be unsound
    // when parameters or locals are not integers at runtime.
    if (ir.buffered_integer_locals) {
        ir.buffered_integer_locals = false;
        ir.integer_local_shadow_reg.clear();
    }
    JitCodeBuffer buffer;
    JitCompiledCode compiled;
    if (!emit_compiled_function_ir(bytecode, ir, buffer, compiled, JitTier::BaselineJit, conservative_dispatch_emit,
                                   error)) {
        return std::nullopt;
    }
    compiled.ir_instruction_count = static_cast<std::uint32_t>(ir.instructions.size());
    compiled.register_count = std::max(bytecode.functions[function_index].register_count, ir.register_count);
    compiled.buffered_integer_locals = ir.buffered_integer_locals;
    compiled.integer_local_shadow_reg = ir.integer_local_shadow_reg;
    compiled.cold_calls_outside_loop = ir.cold_calls_outside_loop;
    compiled.bytecode_fingerprint = fingerprint;
    compiled.function_index = function_index;
    return compiled;
}

std::optional<JitCompiledCode> compile_function_optimizing(const Bytecode& bytecode, std::uint32_t function_index,
                                                           const std::string& fingerprint,
                                                           const JitOptProfile* profile, std::string& error) {
    if (!jit_backend_available()) {
        error = "JIT backend unavailable";
        return std::nullopt;
    }
    if (bytecode_function_stores_upvalue(bytecode, function_index)) {
        return compile_function_baseline(bytecode, function_index, fingerprint, profile, error);
    }
    JitFunctionIr ir;
    if (!build_jit_ir_from_bytecode(bytecode, function_index, ir, error)) {
        return std::nullopt;
    }
    const bool stable_direct_callvalue = ir_callvalue_sites_are_stable_direct(bytecode, ir);
    OptimizingPassStats stats;
    bool conservative_dispatch_emit = false;
    if (!prepare_function_ir_for_jit_compile(bytecode, ir, profile, stable_direct_callvalue, stats, error,
                                             conservative_dispatch_emit)) {
        return std::nullopt;
    }
    JitCodeBuffer buffer;
    JitCompiledCode compiled;
    if (!emit_compiled_function_ir(bytecode, ir, buffer, compiled, JitTier::OptimizingJit, conservative_dispatch_emit,
                                   error)) {
        return std::nullopt;
    }
    if (compiled.metadata == nullptr && !conservative_dispatch_emit) {
        compiled.metadata = std::make_shared<JitCompiledMetadata>();
    }
    compiled.ir_instruction_count = static_cast<std::uint32_t>(ir.instructions.size());
    compiled.register_count = std::max(bytecode.functions[function_index].register_count, ir.register_count);
    compiled.buffered_integer_locals = ir.buffered_integer_locals;
    compiled.integer_local_shadow_reg = ir.integer_local_shadow_reg;
    compiled.cold_calls_outside_loop = ir.cold_calls_outside_loop;
    compiled.bytecode_fingerprint = fingerprint;
    compiled.function_index = function_index;
    return compiled;
}

}  // namespace pscript::jit
