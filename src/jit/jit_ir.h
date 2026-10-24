#pragma once

#include "compiler/bytecode.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pscript::jit {

enum class JitValueKind : std::uint8_t {
    Unknown,
    Integer,
    Float,
    Bool,
    String,
    ConstInteger,
    ConstFloat,
};

enum class JitIrOpcode : std::uint8_t {
    LoadConst,
    LoadLocal,
    MoveReg,
    StoreLocal,
    Add,
    Sub,
    Mul,
    Div,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Jump,
    JumpIfFalse,
    Return,
    // Optimizing tier (Section 2)
    LoadConstInt,
    GuardInteger,
    IntAdd,
    IntSub,
    IntMul,
    IntMod,
    IntDiv,
    IntLess,
    IntEqual,
    FloatAdd,
    FloatSub,
    FloatMul,
    FloatDiv,
    FloatLess,
    FloatEqual,
    StringEqual,
    StringNotEqual,
    StringConcat,
    Mod,
    UnaryNot,
    GetIndex,
    GetField,
    GetIter,
    IterNext,
    LoadFunction,
    LoadUpvalue,
    LoadClosureCapture,
    StoreUpvalue,
    MakeClosure,
    CallValue,
    Call,
    GuardValueKind,
    GuardFieldObject,
    ListGetIndex,
    TupleGetIndex,
    BytesGetIndex,
    DictGetStringKey,
    ListGetIntElement,
    ListSetIndex,
    MakeList,
    MakeDict,
    MakeTuple,
    MakeSet,
    SetIndex,
    MakeStruct,
    SetField,
    // Single integer capture; inner function in b, dest reg in a, value in const_int_value.
    MaterializeClosureIntCapture,
    // Two integer captures; inner in b, cap0 in const_int_value, cap1 in c (must fit int32).
    MaterializeClosureTwoIntCaptures,
    Nop,
};

struct JitCfgBlock {
    std::uint32_t id = 0;
    std::uint32_t first_insn = 0;
    std::uint32_t insn_count = 0;
    std::vector<std::uint32_t> successors;
    std::vector<std::uint32_t> predecessors;
    bool loop_header = false;
    double branch_taken_probability = 0.5;
};

struct JitIrInstruction {
    JitIrOpcode opcode = JitIrOpcode::Return;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
    std::uint32_t bytecode_ip = 0;
    std::uint32_t jump_target_insn = 0;
    std::uint32_t block_id = 0;
    JitValueKind result_kind = JitValueKind::Unknown;
    bool pure = false;
    bool side_effect = false;
    std::int64_t const_int_value = 0;
    bool branch_fallthrough_hot = false;
    std::uint32_t deopt_site_id = 0xFFFFFFFFu;
    std::uint32_t call_ic_index = 0xFFFFFFFFu;
    std::uint32_t guard_value_kind = 0;
};

struct JitBasicBlock {
    std::uint32_t id = 0;
    std::uint32_t start_bytecode_ip = 0;
    std::vector<JitIrInstruction> instructions;
};

struct JitFunctionIr {
    std::uint32_t function_index = 0;
    std::uint32_t register_count = 0;
    std::uint32_t local_count = 0;
    std::vector<JitBasicBlock> blocks;
    std::vector<JitIrInstruction> instructions;
    std::vector<JitCfgBlock> cfg;
    bool profile_stable_integer_ops = false;
    bool profile_stable_float_ops = false;
    bool buffered_integer_locals = false;
    // Optimizer proof: every Call/CallValue is outside the loop body. Default false so
    // baseline and unproven functions stay on the dispatch emitter.
    bool cold_calls_outside_loop = false;
    // Set when optimizing pipeline used the same IR path as baseline JIT (call-free numeric loops).
    bool baseline_numeric_loop_fast_path = false;
    // local index -> shadow register index, or -1 when not buffered.
    std::vector<std::int32_t> integer_local_shadow_reg;
    // True when every CallValue site calls a single known function (no polymorphic closure dispatch).
    bool stable_direct_callvalue = false;
    // True when the function uses the baseline dispatch-loop emitter (polymorphic calls in loops).
    bool conservative_dispatch_emit = false;
};

void build_cfg_for_function(JitFunctionIr& ir);

bool build_jit_ir_from_bytecode(const Bytecode& bytecode, std::uint32_t function_index, JitFunctionIr& out_ir,
                                std::string& error);

bool jit_function_is_eligible(const Bytecode& bytecode, std::uint32_t function_index, std::string& reason);

struct JitCallIcSlot;
void assign_call_ic_slots(JitFunctionIr& ir, std::vector<JitCallIcSlot>& slots);

bool ir_callvalue_is_only_self_recursive(const Bytecode& bytecode, const JitFunctionIr& ir);

bool instruction_in_backedge_loop(const JitFunctionIr& ir, std::size_t insn_index);

bool ir_callvalue_sites_are_stable_direct(const Bytecode& bytecode, const JitFunctionIr& ir);

}  // namespace pscript::jit
