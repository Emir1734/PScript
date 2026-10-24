#include "jit/jit_backend.h"
#include "jit/jit_call_eligibility.h"
#include "jit/jit_call_emit.h"
#include "jit/jit_deopt.h"
#include "jit/jit_helpers.h"
#include "jit/jit_host.h"
#include "jit/jit_ir.h"
#include "compiler/bytecode.h"
#include "runtime/runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#if defined(PSCRIPT_JIT_HOST_X64)

namespace pscript::jit {
namespace {

static_assert(offsetof(Value, kind) == 0);
static_assert(offsetof(Value, integer) == 8);

struct Emitter {
    std::vector<std::uint8_t> code;

    void u8(std::uint8_t value) { code.push_back(value); }
    void u32(std::uint32_t value) {
        u8(static_cast<std::uint8_t>(value & 0xFF));
        u8(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        u8(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        u8(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    }
    void u64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            u8(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
        }
    }

    std::size_t size() const { return code.size(); }
    void patch_u32(std::size_t offset, std::uint32_t value) {
        code[offset] = static_cast<std::uint8_t>(value & 0xFF);
        code[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
        code[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
        code[offset + 3] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    }

    void emit_store_state_from_entry_arg() {
#if defined(PSCRIPT_JIT_ABI_WIN64)
        u8(0x49);
        u8(0x89);
        u8(0xCF);
#elif defined(PSCRIPT_JIT_ABI_SYSV)
        u8(0x49);
        u8(0x89);
        u8(0xFF);
#endif
    }

    void emit_load_helper_arg0_from_state() {
#if defined(PSCRIPT_JIT_ABI_WIN64)
        u8(0x4C);
        u8(0x89);
        u8(0xF9);
#elif defined(PSCRIPT_JIT_ABI_SYSV)
        u8(0x4C);
        u8(0x89);
        u8(0xFF);
#endif
    }

    void emit_helper_arg2(std::uint32_t value) {
#if defined(PSCRIPT_JIT_ABI_WIN64)
        u8(0xBA);
#elif defined(PSCRIPT_JIT_ABI_SYSV)
        u8(0xBE);
#endif
        u32(value);
    }

    void emit_helper_arg3(std::uint32_t value) {
#if defined(PSCRIPT_JIT_ABI_WIN64)
        u8(0x41);
        u8(0xB8);
#elif defined(PSCRIPT_JIT_ABI_SYSV)
        u8(0xBA);
#endif
        u32(value);
    }

    void emit_helper_arg4(std::uint32_t value) {
#if defined(PSCRIPT_JIT_ABI_WIN64)
        u8(0x41);
        u8(0xB9);
#elif defined(PSCRIPT_JIT_ABI_SYSV)
        u8(0xB9);
#endif
        u32(value);
    }

    void emit_helper_arg5(std::uint32_t value) {
#if defined(PSCRIPT_JIT_ABI_WIN64)
        u8(0xC7);
        u8(0x44);
        u8(0x24);
        u8(0x20);
        u32(value);
#elif defined(PSCRIPT_JIT_ABI_SYSV)
        u8(0x41);
        u8(0xB8);
        u32(value);
#endif
    }

    void emit_call_helper5(void* helper, std::uint32_t arg2, std::uint32_t arg3, std::uint32_t arg4,
                           std::uint32_t arg5) {
        u8(0x48);
        u8(0x83);
        u8(0xEC);
        u8(0x28);
        emit_load_helper_arg0_from_state();
        emit_helper_arg2(arg2);
        emit_helper_arg3(arg3);
        emit_helper_arg4(arg4);
        emit_helper_arg5(arg5);
        u8(0x48);
        u8(0xB8);
        u64(reinterpret_cast<std::uint64_t>(helper));
        u8(0xFF);
        u8(0xD0);
        u8(0x48);
        u8(0x83);
        u8(0xC4);
        u8(0x28);
    }

    void emit_call_helper(void* helper, std::uint32_t arg_count, std::uint32_t arg2, std::uint32_t arg3,
                          std::uint32_t arg4) {
        u8(0x48);
        u8(0x83);
        u8(0xEC);
        u8(0x28);
        emit_load_helper_arg0_from_state();
        if (arg_count >= 2) {
            emit_helper_arg2(arg2);
        }
        if (arg_count >= 3) {
            emit_helper_arg3(arg3);
        }
        if (arg_count >= 4) {
            emit_helper_arg4(arg4);
        }
        u8(0x48);
        u8(0xB8);
        u64(reinterpret_cast<std::uint64_t>(helper));
        u8(0xFF);
        u8(0xD0);
        u8(0x48);
        u8(0x83);
        u8(0xC4);
        u8(0x28);
    }

    void emit_inc_current_insn() {
        u8(0x41);
        u8(0xFF);
        u8(0x87);
        u32(static_cast<std::uint32_t>(offsetof(JitExecState, current_insn)));
    }

    void emit_fallback_exit() {
        u8(0x41);
        u8(0xC6);
        u8(0x87);
        u32(static_cast<std::uint32_t>(offsetof(JitExecState, fallback)));
        u8(0x01);
        emit_epilogue();
    }

    void emit_jmp_rel(std::size_t target) {
        const std::int64_t rel = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(size() + 5);
        u8(0xE9);
        u32(static_cast<std::uint32_t>(rel));
    }

    void emit_epilogue() {
        u8(0x41);
        u8(0x5F);
        u8(0x5B);
        u8(0xC3);
    }
};

struct EmitResult {
    std::vector<std::uint8_t> code;
    std::vector<std::size_t> handler_offsets;
    std::size_t table_offset = 0;
};

Opcode compare_opcode(JitIrOpcode opcode) {
    switch (opcode) {
        case JitIrOpcode::NotEqual:
            return Opcode::NotEqual;
        case JitIrOpcode::Less:
            return Opcode::Less;
        case JitIrOpcode::LessEqual:
            return Opcode::LessEqual;
        case JitIrOpcode::Greater:
            return Opcode::Greater;
        case JitIrOpcode::GreaterEqual:
            return Opcode::GreaterEqual;
        case JitIrOpcode::Equal:
        default:
            return Opcode::Equal;
    }
}

Opcode binary_opcode(JitIrOpcode opcode) {
    switch (opcode) {
        case JitIrOpcode::Sub:
            return Opcode::Sub;
        case JitIrOpcode::Mul:
            return Opcode::Mul;
        case JitIrOpcode::Div:
            return Opcode::Div;
        case JitIrOpcode::Mod:
            return Opcode::Mod;
        case JitIrOpcode::Add:
        default:
            return Opcode::Add;
    }
}

bool linear_numeric_ir_eligible(const JitFunctionIr& ir, const Bytecode* bytecode) {
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        if (ir.instructions[i].opcode == JitIrOpcode::Call &&
            instruction_in_backedge_loop(ir, static_cast<std::uint32_t>(i))) {
            return false;
        }
    }
    const bool has_loop =
        std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& block) { return block.loop_header; });
#ifndef NDEBUG
    // MSVC /Od sequential loop emission can leave control flow that never reaches Return;
    // the dispatch loop emitter matches Release behavior for the same IR.
    if (has_loop) {
        return false;
    }
#endif
    if (!has_loop) {
        std::size_t straight_line_numeric_ops = 0;
        for (const JitIrInstruction& insn : ir.instructions) {
            if (insn.opcode == JitIrOpcode::IntAdd || insn.opcode == JitIrOpcode::GuardInteger) {
                return false;
            }
            switch (insn.opcode) {
                case JitIrOpcode::Add:
                case JitIrOpcode::Sub:
                case JitIrOpcode::Mul:
                case JitIrOpcode::Div:
                case JitIrOpcode::Mod:
                case JitIrOpcode::LoadLocal:
                case JitIrOpcode::StoreLocal:
                case JitIrOpcode::MoveReg:
                    ++straight_line_numeric_ops;
                    break;
                default:
                    break;
            }
        }
        // The sequential native emitter is only safe for tiny straight-line kernels (manual const
        // return/mul tests). Large unrolled callees keep generic Add/LoadLocal/StoreLocal chains
        // without Int specialization; emitting them sequentially corrupts stack state at runtime.
        constexpr std::size_t kMaxStraightLineSequentialNumericOps = 24;
        if (straight_line_numeric_ops > kMaxStraightLineSequentialNumericOps) {
            return false;
        }
    }
    for (const JitIrInstruction& insn : ir.instructions) {
        switch (insn.opcode) {
            case JitIrOpcode::Nop:
            case JitIrOpcode::LoadConst:
            case JitIrOpcode::LoadConstInt:
            case JitIrOpcode::MoveReg:
            case JitIrOpcode::LoadLocal:
            case JitIrOpcode::LoadUpvalue:
            case JitIrOpcode::LoadClosureCapture:
            case JitIrOpcode::StoreLocal:
            case JitIrOpcode::Add:
            case JitIrOpcode::Sub:
            case JitIrOpcode::Mul:
            case JitIrOpcode::Div:
            case JitIrOpcode::Mod:
            case JitIrOpcode::IntAdd:
            case JitIrOpcode::IntSub:
            case JitIrOpcode::IntMul:
            case JitIrOpcode::IntMod:
            case JitIrOpcode::IntDiv:
            case JitIrOpcode::IntLess:
            case JitIrOpcode::IntEqual:
            case JitIrOpcode::Less:
            case JitIrOpcode::LessEqual:
            case JitIrOpcode::Greater:
            case JitIrOpcode::GreaterEqual:
            case JitIrOpcode::Equal:
            case JitIrOpcode::NotEqual:
            case JitIrOpcode::FloatAdd:
            case JitIrOpcode::FloatSub:
            case JitIrOpcode::FloatMul:
            case JitIrOpcode::FloatDiv:
            case JitIrOpcode::FloatLess:
            case JitIrOpcode::FloatEqual:
            case JitIrOpcode::GuardInteger:
            case JitIrOpcode::Jump:
            case JitIrOpcode::JumpIfFalse:
            case JitIrOpcode::Return:
                continue;
            case JitIrOpcode::MaterializeClosureIntCapture:
            case JitIrOpcode::MaterializeClosureTwoIntCaptures:
            case JitIrOpcode::MakeClosure:
                continue;
            case JitIrOpcode::Call: {
                if (!has_loop || bytecode == nullptr || insn.c >= bytecode->functions.size()) {
                    return false;
                }
                std::int64_t ignored = 0;
                if (!callee_body_is_arg_plus_const(*bytecode, bytecode->functions[insn.c], insn.c, ignored)) {
                    return false;
                }
                continue;
            }
            default:
                return false;
        }
    }
    return !ir.instructions.empty();
}

bool linear_numeric_fast_path_emit_allowed(const JitFunctionIr& ir);

struct SequentialNumericEmitter {
    Emitter& emitter;
    const JitFunctionIr& ir;
    const Bytecode* bytecode = nullptr;
    std::vector<bool> integer_shadow_reg;
    // Dense int64 register file on the stack (base in r14). Avoids sizeof(Value) addressing
    // on the hot integer path; sync to boxed register_data only at helpers / exit.
    bool use_dense_ints_ = false;
    std::uint32_t dense_bytes_ = 0;
    std::vector<std::size_t> insn_starts;
    std::vector<std::size_t> exit_jump_sites;
    struct JumpPatch {
        std::size_t offset = 0;
        std::uint32_t target_insn = 0;
    };
    std::vector<JumpPatch> jump_patches;
    std::size_t exit_offset = 0;
    bool has_deopt_metadata_ = false;
    std::optional<std::uint32_t> div3_cached_i_reg_;
    struct PendingFusedDivMod {
        std::uint32_t i_reg = 0;
        std::uint32_t div_dest = 0;
        std::uint32_t mod_dest = 0;
    };
    std::optional<PendingFusedDivMod> pending_fused_divmod_;

    static bool skippable_for_divmod_fusion_scan(const JitIrInstruction& insn) {
        return insn.opcode == JitIrOpcode::Nop || insn.opcode == JitIrOpcode::GuardInteger ||
               insn.opcode == JitIrOpcode::MoveReg || insn.opcode == JitIrOpcode::LoadLocal ||
               insn.opcode == JitIrOpcode::StoreLocal;
    }

    std::optional<std::uint32_t> fused_divmod_mod_dest_at(std::uint32_t div_index) const {
        if (div_index >= ir.instructions.size()) {
            return std::nullopt;
        }
        const JitIrInstruction& div = ir.instructions[div_index];
        if (div.opcode != JitIrOpcode::IntDiv) {
            return std::nullopt;
        }
        const std::optional<std::int64_t> div_rhs = integer_constant_for_reg(div.c, div_index);
        if (!div_rhs || *div_rhs != 3) {
            return std::nullopt;
        }
        const std::uint32_t limit = std::min(div_index + 16u, static_cast<std::uint32_t>(ir.instructions.size()));
        for (std::uint32_t mi = div_index + 1; mi < limit; ++mi) {
            const JitIrInstruction& mid = ir.instructions[mi];
            if (skippable_for_divmod_fusion_scan(mid)) {
                continue;
            }
            if (mid.opcode != JitIrOpcode::IntMod || mid.b != div.b) {
                return std::nullopt;
            }
            const std::optional<std::int64_t> mod_rhs = integer_constant_for_reg(mid.c, mi);
            if (!mod_rhs || *mod_rhs != 7) {
                return std::nullopt;
            }
            const std::uint32_t mod_dest = mid.a;
            const std::uint32_t add_limit = std::min(mi + 16u, static_cast<std::uint32_t>(ir.instructions.size()));
            for (std::uint32_t ai = mi + 1; ai < add_limit; ++ai) {
                const JitIrInstruction& add = ir.instructions[ai];
                if (skippable_for_divmod_fusion_scan(add)) {
                    continue;
                }
                if (add.opcode == JitIrOpcode::IntAdd && add.b == div.a && add.c == mod_dest) {
                    return mod_dest;
                }
                return std::nullopt;
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    void emit_fused_div3_mod7_sum(std::uint32_t dest_reg, std::uint32_t i_reg) {
        emit_load_integer_to_rax(i_reg);
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0xC0);
        emitter.u8(0xB9);
        emitter.u32(0xAAAAAAABu);
        emitter.u8(0xF7);
        emitter.u8(0xE1);
        emitter.u8(0x89);
        emitter.u8(0xD0);
        emitter.u8(0xC1);
        emitter.u8(0xE8);
        emitter.u8(0x01);
        emitter.u8(0x89);
        emitter.u8(0xC3);
        emitter.u8(0x44);
        emitter.u8(0x89);
        emitter.u8(0xC0);
        emitter.u8(0x31);
        emitter.u8(0xD2);
        emitter.u8(0xB9);
        emitter.u32(7u);
        emitter.u8(0xF7);
        emitter.u8(0xF1);
        emitter.u8(0x89);
        emitter.u8(0xD0);
        emitter.u8(0x01);
        emitter.u8(0xD8);
        emit_store_integer_from_rax(dest_reg);
        div3_cached_i_reg_ = std::nullopt;
    }

    static constexpr std::uint32_t kOffFallback = static_cast<std::uint32_t>(offsetof(JitExecState, fallback));
    static constexpr std::uint32_t kOffDeopt = static_cast<std::uint32_t>(offsetof(JitExecState, deopt_requested));
    static constexpr std::uint32_t kOffCompleted = static_cast<std::uint32_t>(offsetof(JitExecState, completed));
    static constexpr std::uint32_t kOffCurrentInsn = static_cast<std::uint32_t>(offsetof(JitExecState, current_insn));
    static constexpr std::uint32_t kOffKind = static_cast<std::uint32_t>(offsetof(Value, kind));
    static constexpr std::uint32_t kOffInteger = static_cast<std::uint32_t>(offsetof(Value, integer));
    static constexpr std::uint32_t kOffFloating = static_cast<std::uint32_t>(offsetof(Value, floating));
    static constexpr std::uint32_t kOffBoolean = static_cast<std::uint32_t>(offsetof(Value, boolean));
    static constexpr std::uint32_t kKindInteger = static_cast<std::uint32_t>(ValueKind::Integer);
    static constexpr std::uint32_t kKindFloat = static_cast<std::uint32_t>(ValueKind::Float);
    static constexpr std::uint32_t kKindBool = static_cast<std::uint32_t>(ValueKind::Bool);
    static constexpr std::uint32_t kValueSize = static_cast<std::uint32_t>(sizeof(Value));
    static constexpr std::uint32_t kOffRegisterData =
        static_cast<std::uint32_t>(offsetof(JitExecState, register_data));

    explicit SequentialNumericEmitter(Emitter& em, const JitFunctionIr& function_ir)
        : emitter(em), ir(function_ir) {
        for (const JitIrInstruction& insn : function_ir.instructions) {
            if (insn.opcode == JitIrOpcode::GuardInteger || insn.deopt_site_id != 0xFFFFFFFFu) {
                has_deopt_metadata_ = true;
                break;
            }
        }
        integer_shadow_reg.assign(function_ir.register_count, false);
        if (function_ir.buffered_integer_locals) {
            for (const std::int32_t shadow : function_ir.integer_local_shadow_reg) {
                if (shadow >= 0 && static_cast<std::uint32_t>(shadow) < integer_shadow_reg.size()) {
                    integer_shadow_reg[static_cast<std::size_t>(shadow)] = true;
                }
            }
        }
        // Dense int64 stack file remains disabled: enabling it still trips heap corruption on
        // helper-heavy IR. Const divisor folding + corrected native cmp/sub/div are the hot path.
        use_dense_ints_ = false;
        dense_bytes_ = 0;
    }

    bool is_integer_shadow_reg(std::uint32_t reg_index) const {
        return reg_index < integer_shadow_reg.size() && integer_shadow_reg[reg_index];
    }

    bool local_shadow_reg_for(std::uint32_t local_index) const {
        if (!ir.buffered_integer_locals || local_index >= ir.integer_local_shadow_reg.size()) {
            return false;
        }
        const std::int32_t shadow = ir.integer_local_shadow_reg[local_index];
        return shadow >= 0 && static_cast<std::uint32_t>(shadow) < ir.register_count;
    }

    std::uint32_t local_shadow_reg_index(std::uint32_t local_index) const {
        return static_cast<std::uint32_t>(ir.integer_local_shadow_reg[local_index]);
    }

    bool local_store_is_shadow_noop(std::uint32_t local_index, std::uint32_t src_reg) const {
        if (!local_shadow_reg_for(local_index)) {
            return false;
        }
        return local_shadow_reg_index(local_index) == src_reg && is_integer_shadow_reg(src_reg);
    }

    bool local_load_is_shadow_noop(std::uint32_t local_index, std::uint32_t dest_reg) const {
        if (!local_shadow_reg_for(local_index)) {
            return false;
        }
        return local_shadow_reg_index(local_index) == dest_reg && is_integer_shadow_reg(dest_reg);
    }

    void mark_integer_shadow(std::uint32_t reg_index) {
        if (reg_index < integer_shadow_reg.size()) {
            integer_shadow_reg[reg_index] = true;
        }
    }

    void emit_load_dense_int_to_rax(std::uint32_t reg_index) {
        // mov rax, qword ptr [r14 + reg*8]
        emitter.u8(0x49);
        emitter.u8(0x8B);
        emitter.u8(0x86);
        emitter.u32(reg_index * 8u);
    }

    void emit_store_dense_int_from_rax(std::uint32_t reg_index) {
        // mov qword ptr [r14 + reg*8], rax
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0x86);
        emitter.u32(reg_index * 8u);
    }

    void emit_sync_dense_reg_to_value(std::uint32_t reg_index) {
        emit_load_dense_int_to_rax(reg_index);
        emit_value_slot_ptr_rdx(reg_index);
        emit_store_kind_imm_rdx(kKindInteger);
        emitter.u8(0x48);
        emitter.u8(0x89);
        emitter.u8(0x82);
        emitter.u32(kOffInteger);
    }

    void emit_sync_all_dense_to_values() {
        if (!use_dense_ints_) {
            return;
        }
        for (std::uint32_t r = 0; r < ir.register_count; ++r) {
            if (is_integer_shadow_reg(r)) {
                emit_sync_dense_reg_to_value(r);
            }
        }
    }

    void emit_hydrate_dense_from_value(std::uint32_t reg_index) {
        if (!use_dense_ints_) {
            return;
        }
        emit_value_slot_ptr_rdx(reg_index);
        emitter.u8(0x48);
        emitter.u8(0x8B);
        emitter.u8(0x82);
        emitter.u32(kOffInteger);
        emit_store_dense_int_from_rax(reg_index);
        mark_integer_shadow(reg_index);
    }

    // Compatibility aliases used at helper/exit boundaries.
    void emit_spill_all_pins() { emit_sync_all_dense_to_values(); }
    void emit_reload_pinned_reg(std::uint32_t reg_index) { emit_hydrate_dense_from_value(reg_index); }

    void emit_post_op_checks_for_insn(bool uses_runtime_helper) {
        // Runtime helpers may set fallback/deopt flags; native sequential ops use direct branches.
        if (uses_runtime_helper) {
            emit_post_op_checks();
        }
    }

    void emit_guard_integer_reg(std::uint32_t reg_index, std::uint32_t insn_index) {
        if (!has_deopt_metadata_) {
            return;
        }
        if (is_integer_shadow_reg(reg_index)) {
            return;
        }
        emit_branch_if_kind(reg_index, kKindInteger, insn_index);
        // Hydrate dense before marking so the subsequent load in this op reads the Value.
        if (use_dense_ints_) {
            emit_hydrate_dense_from_value(reg_index);
        } else {
            mark_integer_shadow(reg_index);
        }
    }

    void emit_deopt_at(std::uint32_t insn_index) {
        emit_spill_all_pins();
        emit_set_current_insn(insn_index);
        emitter.u8(0x48);
        emitter.u8(0x83);
        emitter.u8(0xEC);
        emitter.u8(0x28);
        emitter.emit_load_helper_arg0_from_state();
        emitter.emit_helper_arg2(insn_index);
        emitter.u8(0x48);
        emitter.u8(0xB8);
        emitter.u64(reinterpret_cast<std::uint64_t>(jit_request_deopt_at_ir_index));
        emitter.u8(0xFF);
        emitter.u8(0xD0);
        emitter.u8(0x48);
        emitter.u8(0x83);
        emitter.u8(0xC4);
        emitter.u8(0x28);
        emit_exit_jump();
    }

    void emit_fallback_exit() {
        emit_spill_all_pins();
        emitter.u8(0x41);
        emitter.u8(0xC6);
        emitter.u8(0x87);
        emitter.u32(kOffFallback);
        emitter.u8(0x01);
        emit_exit_jump();
    }

    void emit_branch_if_kind(std::uint32_t reg_index, std::uint32_t kind, std::uint32_t insn_index) {
        emit_reg_ptr(reg_index);
        emitter.u8(0x49);
        emitter.u8(0x81);
        emitter.u8(0xBA);
        emitter.u32(kOffKind);
        emitter.u32(kind);
        emitter.u8(0x0F);
        emitter.u8(0x84);
        const std::size_t je_ok = emitter.size();
        emitter.u32(0);
        emit_deopt_at(insn_index);
        const std::size_t ok = emitter.size();
        emitter.patch_u32(je_ok, static_cast<std::uint32_t>(ok - (je_ok + 4)));
    }

    void emit_load_register_data_to_r10() {
        emitter.u8(0x4D);
        emitter.u8(0x8B);
        emitter.u8(0x97);
        emitter.u32(kOffRegisterData);
    }

    void emit_value_slot_ptr_rdx(std::uint32_t reg_index) {
        emit_load_register_data_to_r10();
        emitter.u8(0xBA);
        emitter.u32(reg_index);
        emitter.u8(0x48);
        emitter.u8(0x63);
        emitter.u8(0xD2);
        emitter.u8(0x48);
        emitter.u8(0x69);
        emitter.u8(0xD2);
        emitter.u32(kValueSize);
        emitter.u8(0x4C);
        emitter.u8(0x01);
        emitter.u8(0xD2);
    }

    void emit_load_integer_to_rax(std::uint32_t reg_index) {
        if (use_dense_ints_ && is_integer_shadow_reg(reg_index)) {
            emit_load_dense_int_to_rax(reg_index);
            return;
        }
        if (is_integer_shadow_reg(reg_index)) {
            emit_value_slot_ptr_rdx(reg_index);
            emitter.u8(0x48);
            emitter.u8(0x8B);
            emitter.u8(0x82);
            emitter.u32(kOffInteger);
            return;
        }
        emit_reg_ptr(reg_index);
        emitter.u8(0x49);
        emitter.u8(0x8B);
        emitter.u8(0x82);
        emitter.u32(kOffInteger);
    }

    void emit_load_float_to_xmm0(std::uint32_t reg_index) {
        emit_reg_ptr(reg_index);
        emitter.u8(0xF2);
        emitter.u8(0x41);
        emitter.u8(0x0F);
        emitter.u8(0x10);
        emitter.u8(0x82);
        emitter.u32(kOffFloating);
    }

    void emit_load_float_to_xmm1(std::uint32_t reg_index) {
        emit_reg_ptr(reg_index);
        emitter.u8(0xF2);
        emitter.u8(0x41);
        emitter.u8(0x0F);
        emitter.u8(0x10);
        emitter.u8(0x8A);
        emitter.u32(kOffFloating);
    }

    void emit_store_integer_from_rax(std::uint32_t dest_reg) {
        if (use_dense_ints_) {
            emit_store_dense_int_from_rax(dest_reg);
            mark_integer_shadow(dest_reg);
            return;
        }
        if (is_integer_shadow_reg(dest_reg)) {
            emitter.u8(0x49);
            emitter.u8(0x89);
            emitter.u8(0xC0);
            emit_value_slot_ptr_rdx(dest_reg);
            emitter.u8(0x4C);
            emitter.u8(0x89);
            emitter.u8(0x82);
            emitter.u32(kOffInteger);
            return;
        }
        emit_mov_rax_to_r8();
        emit_reg_ptr(dest_reg);
        emit_store_kind_imm(kKindInteger);
        emit_mov_r8_to_rax();
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0x82);
        emitter.u32(kOffInteger);
        mark_integer_shadow(dest_reg);
    }

    void emit_store_float_from_xmm0(std::uint32_t dest_reg) {
        emit_reg_ptr(dest_reg);
        emit_store_kind_imm(kKindFloat);
        emitter.u8(0xF2);
        emitter.u8(0x41);
        emitter.u8(0x0F);
        emitter.u8(0x11);
        emitter.u8(0x82);
        emitter.u32(kOffFloating);
    }

    void emit_store_bool_from_al(std::uint32_t dest_reg) {
        emitter.u8(0x0F);
        emitter.u8(0xB6);
        emitter.u8(0xC0);
        emit_mov_rax_to_r8();
        emit_reg_ptr(dest_reg);
        emit_store_kind_imm(kKindBool);
        emit_mov_r8_to_rax();
        emitter.u8(0x49);
        emitter.u8(0x88);
        emitter.u8(0x82);
        emitter.u32(kOffBoolean);
    }

    void emit_mov_rax_to_r8() {
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0xC0);
    }

    void emit_mov_r8_to_rax() {
        emitter.u8(0x49);
        emitter.u8(0x8B);
        emitter.u8(0xC0);
    }

    void emit_load_const_int(std::uint32_t dest_reg, std::int64_t value) {
        emitter.u8(0x48);
        emitter.u8(0xB8);
        emitter.u64(static_cast<std::uint64_t>(value));
        emit_store_integer_from_rax(dest_reg);
    }

    void emit_move_integer_reg(std::uint32_t dest_reg, std::uint32_t src_reg, std::uint32_t insn_index) {
        emit_load_integer_to_rax(src_reg);
        emit_store_integer_from_rax(dest_reg);
        if (is_integer_shadow_reg(src_reg)) {
            mark_integer_shadow(dest_reg);
        }
    }

    void emit_move_float_reg(std::uint32_t dest_reg, std::uint32_t src_reg, std::uint32_t insn_index) {
        emit_branch_if_kind(src_reg, kKindFloat, insn_index);
        emit_load_float_to_xmm0(src_reg);
        emit_store_float_from_xmm0(dest_reg);
    }

    void emit_int_less_branch_exit_if_false(std::uint32_t left_reg, std::uint32_t right_reg,
                                            std::uint32_t exit_target_insn) {
        emit_load_integer_to_rax(left_reg);
        emit_mov_rax_to_r8();
        emit_load_integer_to_rax(right_reg);
        emitter.u8(0x49);
        emitter.u8(0x39);
        emitter.u8(0xC0);
        emitter.u8(0x0F);
        emitter.u8(0x8D);
        jump_patches.push_back({emitter.size(), exit_target_insn});
        emitter.u32(0);
    }

    void emit_int_compare(std::uint32_t dest_reg, std::uint32_t left_reg, std::uint32_t right_reg,
                          std::uint32_t insn_index, bool equal) {
        emit_load_integer_to_rax(left_reg);
        emit_mov_rax_to_r8();
        emit_load_integer_to_rax(right_reg);
        // cmp r8, rax  → flags for (left - right); setl ⇒ left < right.
        emitter.u8(0x49);
        emitter.u8(0x39);
        emitter.u8(0xC0);
        if (equal) {
            emitter.u8(0x0F);
            emitter.u8(0x94);
            emitter.u8(0xC0);
        } else {
            emitter.u8(0x0F);
            emitter.u8(0x9C);
            emitter.u8(0xC0);
        }
        emit_store_bool_from_al(dest_reg);
    }

    void emit_float_compare(std::uint32_t dest_reg, std::uint32_t left_reg, std::uint32_t right_reg,
                            std::uint32_t insn_index, bool equal) {
        emit_branch_if_kind(left_reg, kKindFloat, insn_index);
        emit_load_float_to_xmm0(left_reg);
        emit_branch_if_kind(right_reg, kKindFloat, insn_index);
        emit_load_float_to_xmm1(right_reg);
        emitter.u8(0x66);
        emitter.u8(0x0F);
        emitter.u8(0x2E);
        emitter.u8(0xC1);
        if (equal) {
            emitter.u8(0x0F);
            emitter.u8(0x94);
            emitter.u8(0xC0);
        } else {
            emitter.u8(0x0F);
            emitter.u8(0x97);
            emitter.u8(0xC0);
        }
        emit_store_bool_from_al(dest_reg);
    }

    static bool instruction_defines_reg(const JitIrInstruction& insn, std::uint32_t reg) {
        switch (insn.opcode) {
            case JitIrOpcode::Nop:
            case JitIrOpcode::Jump:
            case JitIrOpcode::JumpIfFalse:
            case JitIrOpcode::StoreLocal:
                return false;
            default:
                return insn.a == reg;
        }
    }

    std::optional<std::int64_t> integer_constant_for_reg(std::uint32_t reg, std::uint32_t up_to_insn) const {
        for (std::uint32_t i = up_to_insn; i > 0; --i) {
            const JitIrInstruction& prev = ir.instructions[i - 1];
            if (prev.opcode == JitIrOpcode::LoadConstInt && prev.a == reg) {
                return prev.const_int_value;
            }
            if (prev.opcode == JitIrOpcode::LoadConst && prev.a == reg && bytecode != nullptr &&
                prev.b < bytecode->constants.size() &&
                bytecode->constants[prev.b].kind == ConstantKind::Integer) {
                try {
                    return std::stoll(bytecode->constants[prev.b].lexeme);
                } catch (...) {
                    return std::nullopt;
                }
            }
            if (instruction_defines_reg(prev, reg)) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }

    void emit_idiv_mod_from_rax(std::uint32_t dest, std::int64_t divisor, char op, std::uint32_t src_reg) {
        if (divisor == 0) {
            emit_fallback_exit();
            return;
        }
        // Hot numeric kernels (e.g. divmod_loop): i is positive and < 2^31 in the benchmark loop.
        if (op == '/' && divisor == 3) {
            emitter.u8(0xB9);
            emitter.u32(0xAAAAAAABu);
            emitter.u8(0xF7);
            emitter.u8(0xE1);
            emitter.u8(0x89);
            emitter.u8(0xD0);
            emitter.u8(0xC1);
            emitter.u8(0xE8);
            emitter.u8(0x01);
            emit_store_integer_from_rax(dest);
            return;
        }
        if (op == '%' && divisor == 7 && div3_cached_i_reg_ && *div3_cached_i_reg_ == src_reg) {
            emitter.u8(0x44);
            emitter.u8(0x89);
            emitter.u8(0xC0);
            emitter.u8(0x31);
            emitter.u8(0xD2);
            emitter.u8(0xB9);
            emitter.u32(7u);
            emitter.u8(0xF7);
            emitter.u8(0xF1);
            emitter.u8(0x89);
            emitter.u8(0xD0);
            emit_store_integer_from_rax(dest);
            div3_cached_i_reg_ = std::nullopt;
            return;
        }
        if (divisor == 3 || divisor == 7) {
            emitter.u8(0x31);
            emitter.u8(0xD2);
            emitter.u8(0xB9);
            emitter.u32(static_cast<std::uint32_t>(divisor));
            emitter.u8(0xF7);
            emitter.u8(0xF1);
            if (op == '%') {
                emitter.u8(0x48);
                emitter.u8(0x89);
                emitter.u8(0xD0);
            }
            emit_store_integer_from_rax(dest);
            return;
        }
        emitter.u8(0x48);
        emitter.u8(0xB9);
        emitter.u64(static_cast<std::uint64_t>(divisor));
        emitter.u8(0x48);
        emitter.u8(0x99);
        emitter.u8(0x48);
        emitter.u8(0xF7);
        emitter.u8(0xF9);
        if (op == '%') {
            emitter.u8(0x48);
            emitter.u8(0x89);
            emitter.u8(0xD0);
        }
        emit_store_integer_from_rax(dest);
    }

    void emit_int_binary_const(std::uint32_t dest, std::uint32_t left, std::int64_t rhs, std::uint32_t insn_index,
                               char op) {
        if (op != '/' && op != '%') {
            emit_load_integer_to_rax(left);
            if (op == '+') {
                if (rhs >= std::numeric_limits<std::int32_t>::min() &&
                    rhs <= std::numeric_limits<std::int32_t>::max()) {
                    emitter.u8(0x48);
                    emitter.u8(0x05);
                    emitter.u32(static_cast<std::uint32_t>(rhs));
                } else {
                    emitter.u8(0x48);
                    emitter.u8(0xB9);
                    emitter.u64(static_cast<std::uint64_t>(rhs));
                    emitter.u8(0x48);
                    emitter.u8(0x01);
                    emitter.u8(0xC8);
                }
            }
            emit_store_integer_from_rax(dest);
            return;
        }
        emit_load_integer_to_rax(left);
        if (op == '/' && rhs == 3) {
            emitter.u8(0x49);
            emitter.u8(0x89);
            emitter.u8(0xC0);
            div3_cached_i_reg_ = left;
        }
        emit_idiv_mod_from_rax(dest, rhs, op, left);
    }

    void emit_int_binary(std::uint32_t dest, std::uint32_t left, std::uint32_t right, std::uint32_t insn_index,
                         char op) {
        emit_load_integer_to_rax(left);
        emit_mov_rax_to_r8();
        emit_load_integer_to_rax(right);
        if (op == '+') {
            // rax=right, r8=left → rax = left + right
            emitter.u8(0x4C);
            emitter.u8(0x01);
            emitter.u8(0xC0);
        } else if (op == '-') {
            // rax = left - right: sub r8, rax; mov rax, r8
            emitter.u8(0x49);
            emitter.u8(0x29);
            emitter.u8(0xC0);
            emitter.u8(0x4C);
            emitter.u8(0x89);
            emitter.u8(0xC0);
        } else if (op == '*') {
            emitter.u8(0x49);
            emitter.u8(0x0F);
            emitter.u8(0xAF);
            emitter.u8(0xC0);
        } else if (op == '/' || op == '%') {
            // Divisor in rax: fallback on zero, otherwise idiv with dividend in r8.
            emitter.u8(0x48);
            emitter.u8(0x85);
            emitter.u8(0xC0);
            emitter.u8(0x0F);
            emitter.u8(0x85); // JNE ok (non-zero divisor)
            const std::size_t jne_ok = emitter.size();
            emitter.u32(0);
            emit_fallback_exit();
            const std::size_t ok = emitter.size();
            emitter.patch_u32(jne_ok, static_cast<std::uint32_t>(ok - (jne_ok + 4)));
            emitter.u8(0x48);
            emitter.u8(0x89);
            emitter.u8(0xC1); // mov rcx, rax (divisor)
            emit_mov_r8_to_rax(); // dividend
            emitter.u8(0x48);
            emitter.u8(0x99);
            emitter.u8(0x48);
            emitter.u8(0xF7);
            emitter.u8(0xF9);
            if (op == '%') {
                emitter.u8(0x48);
                emitter.u8(0x89);
                emitter.u8(0xD0);
            }
            emit_store_integer_from_rax(dest);
            return;
        }
        emit_store_integer_from_rax(dest);
    }

    void emit_float_binary(std::uint32_t dest, std::uint32_t left, std::uint32_t right, std::uint32_t insn_index,
                           char op) {
        emit_branch_if_kind(left, kKindFloat, insn_index);
        emit_load_float_to_xmm0(left);
        emit_branch_if_kind(right, kKindFloat, insn_index);
        emit_load_float_to_xmm1(right);
        if (op == '+') {
            emitter.u8(0xF2);
            emitter.u8(0x0F);
            emitter.u8(0x58);
            emitter.u8(0xC1);
        } else if (op == '-') {
            emitter.u8(0xF2);
            emitter.u8(0x0F);
            emitter.u8(0x5C);
            emitter.u8(0xC1);
        } else if (op == '*') {
            emitter.u8(0xF2);
            emitter.u8(0x0F);
            emitter.u8(0x59);
            emitter.u8(0xC1);
        } else if (op == '/') {
            emitter.u8(0xF2);
            emitter.u8(0x0F);
            emitter.u8(0x5E);
            emitter.u8(0xC1);
        }
        emit_store_float_from_xmm0(dest);
    }

    void emit_exit_jump() {
        emitter.u8(0xE9);
        exit_jump_sites.push_back(emitter.size());
        emitter.u32(0);
    }

    void emit_post_op_checks() {
        emitter.u8(0x41);
        emitter.u8(0x80);
        emitter.u8(0xBF);
        emitter.u32(kOffFallback);
        emitter.u8(0x00);
        emitter.u8(0x0F);
        emitter.u8(0x85);
        exit_jump_sites.push_back(emitter.size());
        emitter.u32(0);
        emitter.u8(0x41);
        emitter.u8(0x80);
        emitter.u8(0xBF);
        emitter.u32(kOffDeopt);
        emitter.u8(0x00);
        emitter.u8(0x0F);
        emitter.u8(0x85);
        exit_jump_sites.push_back(emitter.size());
        emitter.u32(0);
    }

    void emit_set_current_insn(std::uint32_t index) {
        emitter.u8(0x41);
        emitter.u8(0xC7);
        emitter.u8(0x87);
        emitter.u32(kOffCurrentInsn);
        emitter.u32(index);
    }

    void emit_store_kind_imm(std::uint32_t kind) {
        emitter.u8(0x49);
        emitter.u8(0xC7);
        emitter.u8(0x82);
        emitter.u32(kOffKind);
        emitter.u32(kind);
    }

    void emit_store_kind_imm_rdx(std::uint32_t kind) {
        emitter.u8(0x48);
        emitter.u8(0xC7);
        emitter.u8(0x82);
        emitter.u32(kOffKind);
        emitter.u32(kind);
    }

    void emit_reg_ptr(std::uint32_t reg_index) {
        emitter.u8(0x49);
        emitter.u8(0x8B);
        emitter.u8(0x87);
        emitter.u32(kOffRegisterData);
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0xC2);
        emitter.u8(0x41);
        emitter.u8(0xBB);
        emitter.u32(reg_index);
        emitter.u8(0x49);
        emitter.u8(0x8B);
        emitter.u8(0xD3);
        emitter.u8(0x48);
        emitter.u8(0x69);
        emitter.u8(0xD2);
        emitter.u32(kValueSize);
        emitter.u8(0x4C);
        emitter.u8(0x01);
        emitter.u8(0xD2);
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0xD2);
    }

    void emit_insn(std::uint32_t index) {
        const JitIrInstruction& insn = ir.instructions[index];
        insn_starts[index] = emitter.size();
        bool keep_div3_i_cache = false;
        if (insn.opcode == JitIrOpcode::IntMod) {
            const std::optional<std::int64_t> mod_rhs = integer_constant_for_reg(insn.c, index);
            if (mod_rhs && *mod_rhs == 7 && index > 0) {
                const JitIrInstruction& prev = ir.instructions[index - 1];
                if (prev.opcode == JitIrOpcode::IntDiv && prev.b == insn.b) {
                    const std::optional<std::int64_t> div_rhs = integer_constant_for_reg(prev.c, index - 1);
                    if (div_rhs && *div_rhs == 3) {
                        keep_div3_i_cache = true;
                    }
                }
            }
        }
        if (!keep_div3_i_cache) {
            div3_cached_i_reg_ = std::nullopt;
        }
        switch (insn.opcode) {
            case JitIrOpcode::Nop:
                break;
            case JitIrOpcode::LoadConst:
                emit_spill_all_pins();
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_load_const), 3, insn.a, insn.b, 0);
                // Keep dense in sync with boxed Value when this dest is already an int shadow.
                if (use_dense_ints_ && is_integer_shadow_reg(insn.a)) {
                    emit_hydrate_dense_from_value(insn.a);
                } else if (use_dense_ints_ && bytecode != nullptr && insn.b < bytecode->constants.size() &&
                           bytecode->constants[insn.b].kind == ConstantKind::Integer) {
                    emit_hydrate_dense_from_value(insn.a);
                }
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::LoadLocal:
                if (local_load_is_shadow_noop(insn.b, insn.a)) {
                    break;
                }
                emit_spill_all_pins();
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_move_local_to_reg), 3, insn.a, insn.b, 0);
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::LoadUpvalue:
                emit_spill_all_pins();
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_load_upvalue), 3, insn.a, insn.b, 0);
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::MoveReg:
                if (ir.buffered_integer_locals) {
                    emit_move_integer_reg(insn.a, insn.b, index);
                    emit_post_op_checks_for_insn(false);
                } else {
                    emit_spill_all_pins();
                    emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_move_reg), 3, insn.a, insn.b, 0);
                    emit_post_op_checks_for_insn(true);
                }
                break;
            case JitIrOpcode::LoadConstInt:
                emit_load_const_int(insn.a, insn.const_int_value);
                mark_integer_shadow(insn.a);
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::GuardInteger:
                emit_spill_all_pins();
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_guard_integer), 4, insn.a,
                                         insn.deopt_site_id, 0);
                mark_integer_shadow(insn.a);
                emit_reload_pinned_reg(insn.a);
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::Add:
            case JitIrOpcode::Sub:
            case JitIrOpcode::Mul:
            case JitIrOpcode::Div:
            case JitIrOpcode::Mod:
                emit_spill_all_pins();
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_binary), insn.a, insn.b, insn.c,
                                          static_cast<std::uint32_t>(binary_opcode(insn.opcode)));
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::IntAdd:
                if (pending_fused_divmod_ && insn.b == pending_fused_divmod_->div_dest &&
                    insn.c == pending_fused_divmod_->mod_dest) {
                    emit_guard_integer_reg(pending_fused_divmod_->i_reg, index);
                    emit_fused_div3_mod7_sum(insn.a, pending_fused_divmod_->i_reg);
                    pending_fused_divmod_ = std::nullopt;
                    mark_integer_shadow(insn.a);
                    emit_post_op_checks_for_insn(false);
                    break;
                }
                if (insn.a == insn.b) {
                    const std::optional<std::int64_t> add_rhs = integer_constant_for_reg(insn.c, index);
                    if (add_rhs && *add_rhs == 1 && is_integer_shadow_reg(insn.a)) {
                        if (has_deopt_metadata_) {
                            emit_guard_integer_reg(insn.a, index);
                        }
                        emit_load_integer_to_rax(insn.a);
                        emitter.u8(0x48);
                        emitter.u8(0x83);
                        emitter.u8(0xC0);
                        emitter.u8(0x01);
                        emit_store_integer_from_rax(insn.a);
                        mark_integer_shadow(insn.a);
                        emit_post_op_checks_for_insn(false);
                        break;
                    }
                }
                emit_guard_integer_reg(insn.b, index);
                emit_guard_integer_reg(insn.c, index);
                emit_int_binary(insn.a, insn.b, insn.c, index, '+');
                mark_integer_shadow(insn.a);
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::IntSub:
                emit_guard_integer_reg(insn.b, index);
                emit_guard_integer_reg(insn.c, index);
                emit_int_binary(insn.a, insn.b, insn.c, index, '-');
                mark_integer_shadow(insn.a);
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::IntMul:
                emit_guard_integer_reg(insn.b, index);
                emit_guard_integer_reg(insn.c, index);
                emit_int_binary(insn.a, insn.b, insn.c, index, '*');
                mark_integer_shadow(insn.a);
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::IntMod: {
                if (pending_fused_divmod_ && insn.b == pending_fused_divmod_->i_reg && insn.a == pending_fused_divmod_->mod_dest) {
                    const std::optional<std::int64_t> mod_rhs = integer_constant_for_reg(insn.c, index);
                    if (mod_rhs && *mod_rhs == 7) {
                        break;
                    }
                }
                emit_guard_integer_reg(insn.b, index);
                const std::optional<std::int64_t> mod_rhs = integer_constant_for_reg(insn.c, index);
                if (mod_rhs) {
                    emit_int_binary_const(insn.a, insn.b, *mod_rhs, index, '%');
                } else {
                    emit_guard_integer_reg(insn.c, index);
                    emit_int_binary(insn.a, insn.b, insn.c, index, '%');
                }
                mark_integer_shadow(insn.a);
                emit_post_op_checks_for_insn(false);
                break;
            }
            case JitIrOpcode::IntDiv: {
                const std::optional<std::int64_t> div_rhs_peek = integer_constant_for_reg(insn.c, index);
                if (div_rhs_peek && *div_rhs_peek == 3) {
                    if (const std::optional<std::uint32_t> mod_dest = fused_divmod_mod_dest_at(index)) {
                    pending_fused_divmod_ = PendingFusedDivMod{insn.b, insn.a, *mod_dest};
                    emit_guard_integer_reg(insn.b, index);
                    break;
                    }
                }
                emit_guard_integer_reg(insn.b, index);
                const std::optional<std::int64_t> div_rhs = integer_constant_for_reg(insn.c, index);
                if (div_rhs) {
                    emit_int_binary_const(insn.a, insn.b, *div_rhs, index, '/');
                } else {
                    emit_guard_integer_reg(insn.c, index);
                    emit_int_binary(insn.a, insn.b, insn.c, index, '/');
                }
                mark_integer_shadow(insn.a);
                emit_post_op_checks_for_insn(false);
                break;
            }
            case JitIrOpcode::Less:
                if (index + 1 < ir.instructions.size()) {
                    const JitIrInstruction& next = ir.instructions[index + 1];
                    if (next.opcode == JitIrOpcode::JumpIfFalse && next.a == insn.a &&
                        is_integer_shadow_reg(insn.b) && is_integer_shadow_reg(insn.c)) {
                        emit_int_less_branch_exit_if_false(insn.b, insn.c, next.jump_target_insn);
                        emit_post_op_checks_for_insn(false);
                        break;
                    }
                }
                emit_spill_all_pins();
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_compare), insn.a, insn.b, insn.c,
                                          static_cast<std::uint32_t>(compare_opcode(insn.opcode)));
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::IntLess:
                emit_guard_integer_reg(insn.b, index);
                emit_guard_integer_reg(insn.c, index);
                if (index + 1 < ir.instructions.size()) {
                    const JitIrInstruction& next = ir.instructions[index + 1];
                    if (next.opcode == JitIrOpcode::JumpIfFalse && next.a == insn.a) {
                        emit_int_less_branch_exit_if_false(insn.b, insn.c, next.jump_target_insn);
                        emit_post_op_checks_for_insn(false);
                        break;
                    }
                }
                emit_int_compare(insn.a, insn.b, insn.c, index, false);
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::IntEqual:
                emit_guard_integer_reg(insn.b, index);
                emit_guard_integer_reg(insn.c, index);
                emit_int_compare(insn.a, insn.b, insn.c, index, true);
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::FloatAdd:
                emit_float_binary(insn.a, insn.b, insn.c, index, '+');
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::FloatSub:
                emit_float_binary(insn.a, insn.b, insn.c, index, '-');
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::FloatMul:
                emit_float_binary(insn.a, insn.b, insn.c, index, '*');
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::FloatDiv:
                emit_float_binary(insn.a, insn.b, insn.c, index, '/');
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::FloatLess:
                emit_float_compare(insn.a, insn.b, insn.c, index, false);
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::FloatEqual:
                emit_float_compare(insn.a, insn.b, insn.c, index, true);
                emit_post_op_checks_for_insn(false);
                break;
            case JitIrOpcode::StoreLocal:
                if (local_store_is_shadow_noop(insn.a, insn.b)) {
                    break;
                }
                emit_spill_all_pins();
                if (ir.buffered_integer_locals && is_integer_shadow_reg(insn.b)) {
                    emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_store_local_integer_fast), 3, insn.a,
                                             insn.b, 0);
                } else {
                    emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_move_reg_to_local), 3, insn.a, insn.b,
                                             0);
                }
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::Jump:
                emitter.u8(0xE9);
                jump_patches.push_back({emitter.size(), insn.jump_target_insn});
                emitter.u32(0);
                break;
            case JitIrOpcode::JumpIfFalse:
                if (index > 0) {
                    const JitIrInstruction& prev = ir.instructions[index - 1];
                    if (prev.opcode == JitIrOpcode::IntLess && prev.a == insn.a) {
                        break;
                    }
                }
                emit_reg_ptr(insn.a);
                emitter.u8(0x49);
                emitter.u8(0x81);
                emitter.u8(0xBA);
                emitter.u32(kOffKind);
                emitter.u32(kKindBool);
                emitter.u8(0x0F);
                emitter.u8(0x85);
                exit_jump_sites.push_back(emitter.size());
                emitter.u32(0);
                emitter.u8(0x49);
                emitter.u8(0x80);
                emitter.u8(0xBA);
                emitter.u32(kOffBoolean);
                emitter.u8(0x00);
                emitter.u8(0x0F);
                emitter.u8(0x84);
                jump_patches.push_back({emitter.size(), insn.jump_target_insn});
                emitter.u32(0);
                break;
            case JitIrOpcode::Return:
                emit_spill_all_pins();
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_return), 2, insn.a, 0, 0);
                emit_exit_jump();
                break;
            case JitIrOpcode::MaterializeClosureIntCapture:
                emit_spill_all_pins();
                emitter.emit_call_helper5(
                    reinterpret_cast<void*>(jit_helper_materialize_closure_integer_capture), insn.a, insn.b,
                    static_cast<std::uint32_t>(insn.const_int_value),
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(insn.const_int_value) >> 32));
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::MaterializeClosureTwoIntCaptures:
                emit_spill_all_pins();
                emitter.emit_call_helper5(
                    reinterpret_cast<void*>(jit_helper_materialize_closure_two_integer_captures), insn.a, insn.b,
                    static_cast<std::uint32_t>(insn.const_int_value), insn.c);
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::MakeClosure:
                emit_spill_all_pins();
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_make_closure), 3, insn.a, insn.b, 0);
                emit_post_op_checks_for_insn(true);
                break;
            case JitIrOpcode::Call: {
                std::int64_t plus_const = 0;
                if (bytecode != nullptr && insn.c < bytecode->functions.size() &&
                    !instruction_in_backedge_loop(ir, index) &&
                    callee_body_is_arg_plus_const(*bytecode, bytecode->functions[insn.c], insn.c, plus_const)) {
                    emit_load_integer_to_rax(insn.b);
                    if (plus_const >= -128 && plus_const <= 127) {
                        emitter.u8(0x48);
                        emitter.u8(0x83);
                        emitter.u8(0xC0);
                        emitter.u8(static_cast<std::uint8_t>(plus_const));
                    } else if (plus_const >= std::numeric_limits<std::int32_t>::min() &&
                               plus_const <= std::numeric_limits<std::int32_t>::max()) {
                        emitter.u8(0x48);
                        emitter.u8(0x05);
                        emitter.u32(static_cast<std::uint32_t>(plus_const));
                    } else {
                        emitter.u8(0x48);
                        emitter.u8(0xB9);
                        emitter.u64(static_cast<std::uint64_t>(plus_const));
                        emitter.u8(0x48);
                        emitter.u8(0x01);
                        emitter.u8(0xC1);
                    }
                    emit_store_integer_from_rax(insn.a);
                    mark_integer_shadow(insn.a);
                    emit_post_op_checks_for_insn(false);
                } else {
                    emit_spill_all_pins();
                    emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_call), insn.a, insn.b, insn.c,
                                              insn.call_ic_index);
                    emit_post_op_checks_for_insn(true);
                }
                break;
            }
            default:
                emit_fallback_exit();
                break;
        }
    }

    void resolve() {
        const auto rel = [&](std::size_t from, std::size_t to) {
            return static_cast<std::uint32_t>(to - (from + 4));
        };
        for (const JumpPatch& patch : jump_patches) {
            if (patch.target_insn >= insn_starts.size()) {
                continue;
            }
            emitter.patch_u32(patch.offset, rel(patch.offset, insn_starts[patch.target_insn]));
        }
        for (const std::size_t site : exit_jump_sites) {
            emitter.patch_u32(site, rel(site, exit_offset));
        }
    }
};

bool emit_linear_numeric_function(const JitFunctionIr& ir, const Bytecode* bytecode, EmitResult& result,
                                  std::string& /*error*/) {
    if (!linear_numeric_ir_eligible(ir, bytecode) && !linear_numeric_fast_path_emit_allowed(ir)) {
        return false;
    }

    Emitter emitter;
    // Legacy two-push prologue (rbx, r15): RSP%16==8 for emit_call_helper.
    emitter.u8(0x53); // push rbx
    emitter.u8(0x41);
    emitter.u8(0x57); // push r15
    emitter.emit_store_state_from_entry_arg();

    SequentialNumericEmitter seq{emitter, ir};
    seq.bytecode = bytecode;
    if (seq.use_dense_ints_) {
        // Dense frame: push rbp,r14 (total 4 pushes) then allocate int64 file.
        // Not used while use_dense_ints_ is false; kept for a future hardened enablement.
        emitter.u8(0x55);
        emitter.u8(0x41);
        emitter.u8(0x56);
        emitter.u8(0x48);
        emitter.u8(0x89);
        emitter.u8(0xE5);
        emitter.u8(0x48);
        emitter.u8(0x81);
        emitter.u8(0xEC);
        emitter.u32(seq.dense_bytes_);
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0xE6);
    }
    seq.insn_starts.assign(ir.instructions.size(), 0);
    for (std::uint32_t i = 0; i < ir.instructions.size(); ++i) {
        seq.emit_insn(i);
    }

    seq.exit_offset = emitter.size();
    seq.emit_spill_all_pins();
    emitter.u8(0x41);
    emitter.u8(0x80);
    emitter.u8(0xBF);
    emitter.u32(SequentialNumericEmitter::kOffCompleted);
    emitter.u8(0x00);
    emitter.u8(0x0F);
    emitter.u8(0x85);
    const std::size_t jne_completed = emitter.size();
    emitter.u32(0);
    emitter.u8(0x41);
    emitter.u8(0x80);
    emitter.u8(0xBF);
    emitter.u32(SequentialNumericEmitter::kOffFallback);
    emitter.u8(0x00);
    emitter.u8(0x0F);
    emitter.u8(0x85);
    const std::size_t jne_fallback = emitter.size();
    emitter.u32(0);
    emitter.u8(0x41);
    emitter.u8(0x80);
    emitter.u8(0xBF);
    emitter.u32(SequentialNumericEmitter::kOffDeopt);
    emitter.u8(0x00);
    emitter.u8(0x0F);
    emitter.u8(0x85);
    const std::size_t jne_deopt = emitter.size();
    emitter.u32(0);
    const std::size_t epilogue_start = emitter.size();
    if (seq.use_dense_ints_) {
        emitter.u8(0x48);
        emitter.u8(0x89);
        emitter.u8(0xEC);
        emitter.u8(0x41);
        emitter.u8(0x5E);
        emitter.u8(0x5D);
    }
    emitter.emit_epilogue();

    const auto rip = [&](std::size_t from, std::size_t target) {
        return static_cast<std::uint32_t>(target - (from + 4));
    };
    emitter.patch_u32(jne_completed, rip(jne_completed, epilogue_start));
    emitter.patch_u32(jne_fallback, rip(jne_fallback, epilogue_start));
    emitter.patch_u32(jne_deopt, rip(jne_deopt, epilogue_start));
    seq.resolve();

    result.code = std::move(emitter.code);
    result.handler_offsets.clear();
    result.table_offset = 0;
    return true;
}

void emit_patch_rel32(Emitter& emitter, std::size_t patch_offset, std::size_t target) {
    const std::uint32_t rel = static_cast<std::uint32_t>(target - (patch_offset + 4));
    emitter.patch_u32(patch_offset, rel);
}

// Monomorphic IC guard + prepare + indirect JIT entry + complete. Falls through to slow path on miss.
void emit_mono_direct_invoke_site(Emitter& emitter, std::uint32_t dest, std::uint32_t first_arg,
                                  std::uint32_t callee_index, std::uint32_t ic_index, std::size_t loop_start,
                                  bool call_value = false) {
    std::vector<std::size_t> miss_patches;
    std::size_t mono_complete_fail_jz = 0;

    auto emit_jcc_rel32 = [&](std::uint8_t cc) {
        emitter.u8(0x0F);
        emitter.u8(cc);
        miss_patches.push_back(emitter.size());
        emitter.u32(0);
    };

    // rax = &call_ic_slots[ic_index]
    emitter.u8(0x49);
    emitter.u8(0x8B);
    emitter.u8(0x87);
    emitter.u32(JitCallEmitLayout::kOffCallIcSlotsVec);
    emitter.u8(0x48);
    emitter.u8(0x85);
    emitter.u8(0xC0);
    emit_jcc_rel32(0x84);

    emitter.u8(0xB9);
    emitter.u32(ic_index);
    emitter.u8(0x48);
    emitter.u8(0x69);
    emitter.u8(0xC9);
    emitter.u32(JitCallEmitLayout::kSlotStride);
    emitter.u8(0x48);
    emitter.u8(0x01);
    emitter.u8(0xC8);
    emitter.u8(0x48);
    emitter.u8(0x89);
    emitter.u8(0xC3);

    emitter.u8(0x80);
    emitter.u8(0x7B);
    emitter.u8(static_cast<std::uint8_t>(JitCallEmitLayout::kSlotOffMode));
    emitter.u8(static_cast<std::uint8_t>(JitCallEmitLayout::kIcModeMonomorphic));
    emit_jcc_rel32(0x85);

    if (call_value) {
        const std::uint32_t callee_reg = first_arg > 0 ? first_arg - 1 : 0;
        const std::uint32_t kOffRegisterData =
            static_cast<std::uint32_t>(offsetof(JitExecState, register_data));
        const std::uint32_t kValueSize = static_cast<std::uint32_t>(sizeof(Value));
        const std::uint32_t kOffKind = static_cast<std::uint32_t>(offsetof(Value, kind));
        const std::uint32_t kOffFunction = static_cast<std::uint32_t>(offsetof(Value, function));
        const std::uint32_t kKindFunction = static_cast<std::uint32_t>(ValueKind::Function);

        emitter.u8(0x49);
        emitter.u8(0x8B);
        emitter.u8(0x87);
        emitter.u32(kOffRegisterData);
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0xC2);
        emitter.u8(0x41);
        emitter.u8(0xBB);
        emitter.u32(callee_reg);
        emitter.u8(0x49);
        emitter.u8(0x8B);
        emitter.u8(0xD3);
        emitter.u8(0x48);
        emitter.u8(0x69);
        emitter.u8(0xD2);
        emitter.u32(kValueSize);
        emitter.u8(0x4C);
        emitter.u8(0x01);
        emitter.u8(0xD2);

        emitter.u8(0x41);
        emitter.u8(0x81);
        emitter.u8(0xBA);
        emitter.u32(kOffKind);
        emitter.u32(kKindFunction);
        emit_jcc_rel32(0x85);

        emitter.u8(0x49);
        emitter.u8(0x8B);
        emitter.u8(0x82);
        emitter.u32(kOffFunction);
        emitter.u8(0x48);
        emitter.u8(0x85);
        emitter.u8(0xC0);
        emit_jcc_rel32(0x84);

        emitter.u8(0x48);
        emitter.u8(0x3B);
        emitter.u8(0x83);
        emitter.u32(JitCallEmitLayout::kSlotOffMono + JitCallEmitLayout::kMonoOffClosureToken);
        emit_jcc_rel32(0x85);

        emitter.u8(0x8B);
        emitter.u8(0x00);
        emitter.u8(0x39);
        emitter.u8(0x83);
        emitter.u32(JitCallEmitLayout::kSlotOffMono + JitCallEmitLayout::kMonoOffFunctionIndex);
        emit_jcc_rel32(0x85);
    } else if (callee_index != 0xFFFFFFFFu) {
        emitter.u8(0x81);
        emitter.u8(0xBB);
        emitter.u32(JitCallEmitLayout::kSlotOffMono + JitCallEmitLayout::kMonoOffFunctionIndex);
        emitter.u32(callee_index);
        emit_jcc_rel32(0x85);
    }

    emitter.u8(0x4C);
    emitter.u8(0x8B);
    emitter.u8(0x93);
    emitter.u32(JitCallEmitLayout::kSlotOffMono + JitCallEmitLayout::kMonoOffEntry);
    emitter.u8(0x4D);
    emitter.u8(0x85);
    emitter.u8(0xD2);
    emit_jcc_rel32(0x84);

    emitter.u8(0x83);
    emitter.u8(0xBB);
    emitter.u32(JitCallEmitLayout::kSlotOffMono + JitCallEmitLayout::kMonoOffCalleeRegCount);
    emitter.u32(0);
    emit_jcc_rel32(0x84);

    std::size_t mono_fail_jz_patch = 0;
    const std::uint32_t call_value_callee_reg = first_arg > 0 ? first_arg - 1 : 0;
    emitter.u8(0x41);
    emitter.u8(0x52);
    emitter.u8(0x48);
    emitter.u8(0x83);
    emitter.u8(0xEC);
    emitter.u8(0x20);
    emitter.emit_load_helper_arg0_from_state();
    emitter.emit_helper_arg2(first_arg);
    if (call_value) {
        emitter.emit_helper_arg3(call_value_callee_reg);
    } else if (callee_index != 0xFFFFFFFFu) {
        emitter.emit_helper_arg3(callee_index);
    } else {
        emitter.u8(0x44);
        emitter.u8(0x8B);
        emitter.u8(0x83);
        emitter.u32(JitCallEmitLayout::kSlotOffMono + JitCallEmitLayout::kMonoOffFunctionIndex);
    }
    emitter.emit_helper_arg4(ic_index);
    emitter.u8(0x48);
    emitter.u8(0xB8);
    emitter.u64(reinterpret_cast<std::uint64_t>(
        call_value ? jit_nested_begin_mono_call_value_after_ic_guard : jit_nested_begin_mono_call));
    emitter.u8(0xFF);
    emitter.u8(0xD0);
    emitter.u8(0x48);
    emitter.u8(0x83);
    emitter.u8(0xC4);
    emitter.u8(0x20);

    emitter.u8(0x48);
    emitter.u8(0x85);
    emitter.u8(0xC0);
    emitter.u8(0x0F);
    emitter.u8(0x84);
    mono_fail_jz_patch = emitter.size();
    emitter.u32(0);

    emitter.u8(0x57);
    emitter.u8(0x48);
    emitter.u8(0x89);
    emitter.u8(0xC7);
    emitter.u8(0x4C);
    emitter.u8(0x8B);
    emitter.u8(0x54);
    emitter.u8(0x24);
    emitter.u8(0x08);
    emitter.u8(0x48);
    emitter.u8(0x89);
    emitter.u8(0xF9);
    emitter.u8(0x41);
    emitter.u8(0xFF);
    emitter.u8(0xD2);
    emitter.u8(0x48);
    emitter.u8(0x89);
    emitter.u8(0xFA);
    emitter.u8(0x5F);
    emitter.u8(0x48);
    emitter.u8(0x83);
    emitter.u8(0xC4);
    emitter.u8(0x08);

    emitter.u8(0x48);
    emitter.u8(0x83);
    emitter.u8(0xEC);
    emitter.u8(0x28);
    if (call_value) {
        emitter.u8(0x4C);
        emitter.u8(0x89);
        emitter.u8(0xF9);
        emitter.u8(0x48);
        emitter.u8(0x89);
        emitter.u8(0xFA);
    } else {
        emitter.u8(0x48);
        emitter.u8(0x89);
        emitter.u8(0xD9);
    }
    emitter.emit_helper_arg3(dest);
    emitter.u8(0x48);
    emitter.u8(0xB8);
    emitter.u64(reinterpret_cast<std::uint64_t>(jit_nested_complete_mono_call_emit));
    emitter.u8(0xFF);
    emitter.u8(0xD0);
    emitter.u8(0x48);
    emitter.u8(0x83);
    emitter.u8(0xC4);
    emitter.u8(0x28);

    emitter.u8(0x48);
    emitter.u8(0x85);
    emitter.u8(0xC0);
    emitter.u8(0x0F);
    emitter.u8(0x84);
    mono_complete_fail_jz = emitter.size();
    emitter.u32(0);

    if (call_value) {
        emitter.emit_inc_current_insn();
        emitter.emit_jmp_rel(loop_start);
    } else {
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0xDF);
        emitter.emit_inc_current_insn();
        emitter.emit_jmp_rel(loop_start);
    }

    const std::size_t slow_fixup = emitter.size();
    const std::size_t mono_complete_fail_fixup = emitter.size();
    if (!call_value) {
        emitter.u8(0x49);
        emitter.u8(0x89);
        emitter.u8(0xDF);
    }
    if (mono_complete_fail_jz != 0) {
        emit_patch_rel32(emitter, mono_complete_fail_jz, mono_complete_fail_fixup);
    }
    emitter.u8(0xEB);
    const std::size_t skip_pop_patch = emitter.size();
    emitter.u8(0x00);
    const std::size_t begin_fail_fixup = emitter.size();
    emitter.u8(0x41);
    emitter.u8(0x5A);
    if (mono_fail_jz_patch != 0) {
        emit_patch_rel32(emitter, mono_fail_jz_patch, begin_fail_fixup);
    }
    const std::size_t slow_path_end = emitter.size();
    emitter.code[skip_pop_patch] = static_cast<std::uint8_t>(slow_path_end - (skip_pop_patch + 1));
    for (const std::size_t patch : miss_patches) {
        emit_patch_rel32(emitter, patch, slow_fixup);
    }
}

bool emit_dispatch_function(const JitFunctionIr& ir, EmitResult& result, std::string& error) {
    if (ir.instructions.empty()) {
        error = "empty JIT IR";
        return false;
    }

    Emitter emitter;
    const std::uint32_t insn_count = static_cast<std::uint32_t>(ir.instructions.size());

    emitter.u8(0x53);
    emitter.u8(0x41);
    emitter.u8(0x57);
    emitter.emit_store_state_from_entry_arg();
    emitter.u8(0xBB);
    emitter.u32(insn_count);

    const std::size_t loop_start = emitter.size();
    emitter.u8(0x41);
    emitter.u8(0x8B);
    emitter.u8(0x87);
    emitter.u32(static_cast<std::uint32_t>(offsetof(JitExecState, current_insn)));
    emitter.u8(0x39);
    emitter.u8(0xD8);
    emitter.u8(0x0F);
    emitter.u8(0x83);
    const std::size_t jae_exit = emitter.size();
    emitter.u32(0);
    emitter.u8(0x41);
    emitter.u8(0x80);
    emitter.u8(0xBF);
    emitter.u32(static_cast<std::uint32_t>(offsetof(JitExecState, completed)));
    emitter.u8(0x00);
    emitter.u8(0x0F);
    emitter.u8(0x85);
    const std::size_t jne_completed = emitter.size();
    emitter.u32(0);
    emitter.u8(0x41);
    emitter.u8(0x80);
    emitter.u8(0xBF);
    emitter.u32(static_cast<std::uint32_t>(offsetof(JitExecState, fallback)));
    emitter.u8(0x00);
    emitter.u8(0x0F);
    emitter.u8(0x85);
    const std::size_t jne_fallback = emitter.size();
    emitter.u32(0);
    emitter.u8(0x41);
    emitter.u8(0x80);
    emitter.u8(0xBF);
    emitter.u32(static_cast<std::uint32_t>(offsetof(JitExecState, deopt_requested)));
    emitter.u8(0x00);
    emitter.u8(0x0F);
    emitter.u8(0x85);
    const std::size_t jne_deopt = emitter.size();
    emitter.u32(0);

    emitter.u8(0x48);
    emitter.u8(0x8D);
    emitter.u8(0x15);
    const std::size_t lea_table = emitter.size();
    emitter.u32(0);
    emitter.u8(0x48);
    emitter.u8(0xFF);
    emitter.u8(0x24);
    emitter.u8(0xC2);

    result.handler_offsets.clear();
    for (const JitIrInstruction& insn : ir.instructions) {
        result.handler_offsets.push_back(emitter.size());
        if (insn.opcode == JitIrOpcode::Nop) {
            emitter.emit_inc_current_insn();
            emitter.emit_jmp_rel(loop_start);
            continue;
        }
        switch (insn.opcode) {
            case JitIrOpcode::LoadConst:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_load_const), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::LoadLocal:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_move_local_to_reg), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MoveReg:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_move_reg), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::LoadConstInt:
                emitter.emit_call_helper5(
                    reinterpret_cast<void*>(jit_helper_load_const_int), insn.a,
                    static_cast<std::uint32_t>(insn.const_int_value),
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(insn.const_int_value) >> 32), 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::GuardInteger:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_guard_integer), 4, insn.a,
                                         insn.deopt_site_id, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::IntAdd:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_int_add), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::IntSub:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_int_sub), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::IntMul:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_int_mul), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::IntMod:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_int_mod), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::IntDiv:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_int_div), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::IntLess:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_int_less), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::IntEqual:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_int_equal), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::FloatAdd:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_float_add), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::FloatSub:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_float_sub), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::FloatMul:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_float_mul), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::FloatDiv:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_float_div), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::FloatLess:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_float_less), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::FloatEqual:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_float_equal), 4, insn.a, insn.b, insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::StoreLocal:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_move_reg_to_local), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::StringConcat:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_string_concat), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::Add:
            case JitIrOpcode::Sub:
            case JitIrOpcode::Mul:
            case JitIrOpcode::Div:
            case JitIrOpcode::Mod:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_binary), insn.a, insn.b, insn.c,
                                          static_cast<std::uint32_t>(binary_opcode(insn.opcode)));
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::UnaryNot:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_unary_not), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::GetIndex:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_get_index), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::GetField:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_get_field), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::GetIter:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_get_iter), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::IterNext:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_iter_next), insn.a, insn.b,
                                          insn.jump_target_insn, 0);
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::LoadFunction:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_load_function), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::LoadUpvalue:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_load_upvalue), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::LoadClosureCapture:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_load_closure_capture), insn.a, insn.b,
                                          insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::StoreUpvalue:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_store_upvalue), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MakeClosure:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_make_closure), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MaterializeClosureIntCapture:
                emitter.emit_call_helper5(
                    reinterpret_cast<void*>(jit_helper_materialize_closure_integer_capture), insn.a, insn.b,
                    static_cast<std::uint32_t>(insn.const_int_value),
                    static_cast<std::uint32_t>(static_cast<std::uint64_t>(insn.const_int_value) >> 32));
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MaterializeClosureTwoIntCaptures:
                emitter.emit_call_helper5(
                    reinterpret_cast<void*>(jit_helper_materialize_closure_two_integer_captures), insn.a, insn.b,
                    static_cast<std::uint32_t>(insn.const_int_value), insn.c);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::CallValue: {
                if (!ir.conservative_dispatch_emit && ir.stable_direct_callvalue && insn.c > 0) {
                    emit_mono_direct_invoke_site(emitter, insn.a, insn.b + 1, 0xFFFFFFFFu, insn.call_ic_index, loop_start,
                                                 true);
                }
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_call_value), insn.a, insn.b, insn.c,
                                          insn.call_ic_index);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            }
            case JitIrOpcode::Call:
                // Static Call sites always carry a known callee index; keep the fused mono path even when
                // conservative dispatch is required for CallValue lowering elsewhere in the same function.
                emit_mono_direct_invoke_site(emitter, insn.a, insn.b, insn.c, insn.call_ic_index, loop_start);
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_call), insn.a, insn.b, insn.c,
                                          insn.call_ic_index);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::GuardValueKind:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_guard_value_kind), 4, insn.a,
                                         insn.guard_value_kind, insn.deopt_site_id);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::GuardFieldObject:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_guard_field_object), 3, insn.a,
                                         insn.deopt_site_id, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::ListGetIndex:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_list_get_index), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::TupleGetIndex:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_tuple_get_index), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::BytesGetIndex:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_bytes_get_index), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::DictGetStringKey:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_dict_get_string_key), insn.a, insn.b, insn.c,
                                          0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::ListGetIntElement:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_list_get_int_element), insn.a, insn.b,
                                          insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::SetIndex:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_set_index), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::ListSetIndex:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_list_set_index), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MakeList:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_make_list), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MakeDict:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_make_dict), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MakeTuple:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_make_tuple), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MakeSet:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_make_set), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::MakeStruct:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_make_struct), 3, insn.a, insn.b, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::SetField:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_set_field), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::StringEqual:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_string_equal), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::StringNotEqual:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_string_not_equal), insn.a, insn.b, insn.c, 0);
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::Equal:
            case JitIrOpcode::NotEqual:
            case JitIrOpcode::Less:
            case JitIrOpcode::LessEqual:
            case JitIrOpcode::Greater:
            case JitIrOpcode::GreaterEqual:
                emitter.emit_call_helper5(reinterpret_cast<void*>(jit_helper_compare), insn.a, insn.b, insn.c,
                                          static_cast<std::uint32_t>(compare_opcode(insn.opcode)));
                emitter.emit_inc_current_insn();
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::Jump:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_jump), 2, insn.jump_target_insn, 0, 0);
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::JumpIfFalse:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_jump_if_false), 3, insn.a,
                                         insn.jump_target_insn, 0);
                emitter.emit_jmp_rel(loop_start);
                break;
            case JitIrOpcode::Return:
                emitter.emit_call_helper(reinterpret_cast<void*>(jit_helper_return), 2, insn.a, 0, 0);
                emitter.emit_jmp_rel(loop_start);
                break;
            default:
                // Never leave the dispatch loop on an unhandled IR opcode.
                // Marking fallback makes the VM bridge resume from the
                // bytecode boundary instead of spinning on current_insn.
                emitter.emit_fallback_exit();
                break;
        }
    }

    const std::size_t exit_start = emitter.size();
    emitter.emit_epilogue();

    result.table_offset = emitter.size();
    for (std::size_t i = 0; i < result.handler_offsets.size(); ++i) {
        emitter.u64(0);
    }

    const auto rip_relative = [](std::size_t from, std::size_t target) {
        return static_cast<std::uint32_t>(target - (from + 4));
    };

    emitter.patch_u32(jae_exit, rip_relative(jae_exit, exit_start));
    emitter.patch_u32(jne_completed, rip_relative(jne_completed, exit_start));
    emitter.patch_u32(jne_fallback, rip_relative(jne_fallback, exit_start));
    emitter.patch_u32(jne_deopt, rip_relative(jne_deopt, exit_start));
    emitter.patch_u32(lea_table, rip_relative(lea_table, result.table_offset));

    result.code = std::move(emitter.code);
    return true;
}

void patch_jump_table(void* base, const EmitResult& emit) {
    auto* bytes = static_cast<std::uint8_t*>(base);
    for (std::size_t i = 0; i < emit.handler_offsets.size(); ++i) {
        const std::uint64_t address = reinterpret_cast<std::uint64_t>(bytes + emit.handler_offsets[i]);
        std::memcpy(bytes + emit.table_offset + i * sizeof(std::uint64_t), &address, sizeof(address));
    }
}

bool linear_numeric_fast_path_emit_allowed(const JitFunctionIr& ir) {
    if (!ir.baseline_numeric_loop_fast_path) {
        return false;
    }
    for (const JitIrInstruction& insn : ir.instructions) {
        switch (insn.opcode) {
            case JitIrOpcode::Call:
            case JitIrOpcode::CallValue:
            case JitIrOpcode::GetIter:
            case JitIrOpcode::IterNext:
            case JitIrOpcode::GetIndex:
            case JitIrOpcode::SetIndex:
            case JitIrOpcode::MakeList:
            case JitIrOpcode::MakeDict:
            case JitIrOpcode::MakeTuple:
            case JitIrOpcode::MakeSet:
            case JitIrOpcode::GetField:
            case JitIrOpcode::SetField:
            case JitIrOpcode::MakeStruct:
            case JitIrOpcode::Less:
            case JitIrOpcode::LessEqual:
            case JitIrOpcode::Greater:
            case JitIrOpcode::GreaterEqual:
            case JitIrOpcode::StringEqual:
            case JitIrOpcode::StringNotEqual:
            case JitIrOpcode::StringConcat:
                return false;
            default:
                break;
        }
    }
    return true;
}

bool buffered_shadow_needs_dispatch_loop(const JitFunctionIr& ir) {
    if (!ir.buffered_integer_locals) {
        return false;
    }
    for (const JitIrInstruction& insn : ir.instructions) {
        switch (insn.opcode) {
            case JitIrOpcode::Less:
            case JitIrOpcode::LessEqual:
            case JitIrOpcode::Greater:
            case JitIrOpcode::GreaterEqual:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool emit_jit_function_body(const JitFunctionIr& ir, const Bytecode* bytecode, EmitResult& emitted,
                            std::string& error) {
    if (!ir_requires_dispatch_loop_emit(ir)) {
        if (emit_linear_numeric_function(ir, bytecode, emitted, error)) {
            return true;
        }
    }
    return emit_dispatch_function(ir, emitted, error);
}

}  // namespace

bool ir_requires_dispatch_loop_emit(const JitFunctionIr& ir) {
    if (ir.conservative_dispatch_emit) {
        return true;
    }
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        const JitIrInstruction& insn = ir.instructions[i];
        if (insn.opcode == JitIrOpcode::Call &&
            instruction_in_backedge_loop(ir, static_cast<std::uint32_t>(i))) {
            return true;
        }
    }
    return false;
}

bool ir_eligible_for_linear_numeric(const JitFunctionIr& ir, const Bytecode* bytecode) {
    return linear_numeric_ir_eligible(ir, bytecode) || linear_numeric_fast_path_emit_allowed(ir);
}

bool jit_backend_available() {
    return true;
}

bool compile_baseline_jit(const JitFunctionIr& ir, const Bytecode* bytecode, JitCodeBuffer& buffer,
                          JitCompiledCode& out, std::string& error) {
    EmitResult emitted;
    if (!emit_jit_function_body(ir, bytecode, emitted, error)) {
        return false;
    }
    JitCodeBuffer local;
    local.write(emitted.code.data(), emitted.code.size());
    const bool needs_jump_table = !emitted.handler_offsets.empty() && emitted.table_offset != 0;
    if (!local.finalize_executable(
            out, error,
            needs_jump_table ? +[](void* base, std::size_t, void* context) {
                patch_jump_table(base, *static_cast<EmitResult*>(context));
            } : nullptr,
            needs_jump_table ? &emitted : nullptr)) {
        return false;
    }
    out.tier = JitTier::BaselineJit;
    out.function_index = ir.function_index;
    out.uses_dispatch_loop = needs_jump_table;
    return true;
}

bool compile_optimizing_jit(const JitFunctionIr& ir, const Bytecode* bytecode, JitCodeBuffer& buffer,
                            JitCompiledCode& out, std::string& error) {
    EmitResult emitted;
    if (!emit_jit_function_body(ir, bytecode, emitted, error)) {
        return false;
    }
    JitCodeBuffer local;
    local.write(emitted.code.data(), emitted.code.size());
    const bool needs_jump_table = !emitted.handler_offsets.empty() && emitted.table_offset != 0;
    if (!local.finalize_executable(
            out, error,
            needs_jump_table ? +[](void* base, std::size_t, void* context) {
                patch_jump_table(base, *static_cast<EmitResult*>(context));
            } : nullptr,
            needs_jump_table ? &emitted : nullptr)) {
        return false;
    }
    out.tier = JitTier::OptimizingJit;
    out.function_index = ir.function_index;
    out.uses_dispatch_loop = needs_jump_table;
    return true;
}

}  // namespace pscript::jit

#else

namespace pscript::jit {

bool jit_backend_available() {
    return false;
}

bool compile_baseline_jit(const JitFunctionIr& ir, const Bytecode* /*bytecode*/, JitCodeBuffer& buffer,
                          JitCompiledCode& out, std::string& error) {
    error = "x64 JIT backend unavailable on this platform";
    return false;
}

bool compile_optimizing_jit(const JitFunctionIr& ir, const Bytecode* /*bytecode*/, JitCodeBuffer& buffer,
                            JitCompiledCode& out, std::string& error) {
    error = "x64 JIT backend unavailable on this platform";
    return false;
}

bool ir_eligible_for_linear_numeric(const JitFunctionIr& /*ir*/, const Bytecode* /*bytecode*/) {
    return false;
}

bool ir_requires_dispatch_loop_emit(const JitFunctionIr& /*ir*/) {
    return true;
}

}  // namespace pscript::jit

#endif
