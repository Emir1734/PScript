#include "jit/jit_opt.h"

#include "compiler/bytecode.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pscript::jit {
namespace {

bool insn_terminates_block(const JitIrInstruction& insn) {
    return insn.opcode == JitIrOpcode::Jump || insn.opcode == JitIrOpcode::JumpIfFalse ||
           insn.opcode == JitIrOpcode::Return;
}

const JitCfgBlock* cfg_block_by_id(const JitFunctionIr& ir, std::uint32_t block_id) {
    for (const JitCfgBlock& block : ir.cfg) {
        if (block.id == block_id) {
            return &block;
        }
    }
    return nullptr;
}

std::unordered_set<std::uint32_t> loop_body_block_ids(const JitFunctionIr& ir) {
    std::unordered_set<std::uint32_t> body;
    for (const JitCfgBlock& header : ir.cfg) {
        if (!header.loop_header) {
            continue;
        }
        std::vector<std::uint32_t> stack;
        stack.push_back(header.id);
        while (!stack.empty()) {
            const std::uint32_t block_id = stack.back();
            stack.pop_back();
            if (!body.insert(block_id).second) {
                continue;
            }
            const JitCfgBlock* block = cfg_block_by_id(ir, block_id);
            if (!block) {
                continue;
            }
            for (std::uint32_t succ : block->successors) {
                stack.push_back(succ);
            }
        }
    }
    return body;
}

bool is_pure_arith(const JitIrInstruction& insn) {
    switch (insn.opcode) {
        case JitIrOpcode::Add:
        case JitIrOpcode::Sub:
        case JitIrOpcode::Mul:
        case JitIrOpcode::Div:
        case JitIrOpcode::Mod:
        case JitIrOpcode::Equal:
        case JitIrOpcode::NotEqual:
        case JitIrOpcode::Less:
        case JitIrOpcode::LessEqual:
        case JitIrOpcode::Greater:
        case JitIrOpcode::GreaterEqual:
        case JitIrOpcode::IntAdd:
        case JitIrOpcode::IntSub:
        case JitIrOpcode::IntMul:
        case JitIrOpcode::IntMod:
        case JitIrOpcode::IntDiv:
        case JitIrOpcode::IntLess:
        case JitIrOpcode::IntEqual:
        case JitIrOpcode::FloatAdd:
        case JitIrOpcode::FloatSub:
        case JitIrOpcode::FloatMul:
        case JitIrOpcode::FloatDiv:
        case JitIrOpcode::FloatLess:
        case JitIrOpcode::FloatEqual:
            return true;
        default:
            return false;
    }
}

void mark_side_effects(JitFunctionIr& ir) {
    for (JitIrInstruction& insn : ir.instructions) {
        insn.pure = is_pure_arith(insn) || insn.opcode == JitIrOpcode::LoadConst ||
                    insn.opcode == JitIrOpcode::LoadConstInt || insn.opcode == JitIrOpcode::LoadLocal ||
                    insn.opcode == JitIrOpcode::LoadUpvalue;
        insn.side_effect = insn.opcode == JitIrOpcode::StoreLocal || insn.opcode == JitIrOpcode::Call ||
                           insn.opcode == JitIrOpcode::CallValue || insn.opcode == JitIrOpcode::GetField ||
                           insn.opcode == JitIrOpcode::GetIndex || insn.opcode == JitIrOpcode::ListGetIndex ||
                           insn.opcode == JitIrOpcode::TupleGetIndex || insn.opcode == JitIrOpcode::BytesGetIndex ||
                           insn.opcode == JitIrOpcode::DictGetStringKey ||
                           insn.opcode == JitIrOpcode::ListSetIndex || insn.opcode == JitIrOpcode::SetIndex ||
                           insn.opcode == JitIrOpcode::MakeList || insn.opcode == JitIrOpcode::MakeDict ||
                           insn.opcode == JitIrOpcode::MakeTuple || insn.opcode == JitIrOpcode::MakeSet ||
                           insn.opcode == JitIrOpcode::MakeStruct || insn.opcode == JitIrOpcode::SetField;
    }
}

std::uint32_t max_register_operand(const JitFunctionIr& ir) {
    std::uint32_t max_reg = 0;
    for (const JitIrInstruction& insn : ir.instructions) {
        max_reg = std::max(max_reg, insn.a);
        max_reg = std::max(max_reg, insn.b);
        max_reg = std::max(max_reg, insn.c);
    }
    return max_reg;
}

void recompute_register_count(JitFunctionIr& ir) {
    ir.register_count = std::max(ir.register_count, max_register_operand(ir) + 1);
}

std::optional<std::int64_t> integer_from_const(const Bytecode& bytecode, std::uint32_t index) {
    if (index >= bytecode.constants.size()) {
        return std::nullopt;
    }
    const Constant& constant = bytecode.constants[index];
    if (constant.kind != ConstantKind::Integer) {
        return std::nullopt;
    }
    try {
        return std::stoll(constant.lexeme);
    } catch (...) {
        return std::nullopt;
    }
}

void rewrite_integer_load_consts(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::LoadConst) {
            continue;
        }
        const std::optional<std::int64_t> value = integer_from_const(bytecode, insn.b);
        if (!value) {
            continue;
        }
        insn.opcode = JitIrOpcode::LoadConstInt;
        insn.const_int_value = *value;
        insn.b = 0;
        insn.c = 0;
        insn.result_kind = JitValueKind::ConstInteger;
        ++stats.constant_folds;
    }
}

void constant_fold_and_propagate(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    const bool has_loops =
        std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& b) { return b.loop_header; });
    const std::unordered_set<std::uint32_t> loop_body = has_loops ? loop_body_block_ids(ir) : std::unordered_set<std::uint32_t>{};
    std::vector<std::optional<std::int64_t>> reg_const(ir.register_count);
    std::uint32_t prev_block = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        JitIrInstruction& insn = ir.instructions[i];
        if (insn.block_id != prev_block) {
            prev_block = insn.block_id;
            const JitCfgBlock* cfg_block = cfg_block_by_id(ir, insn.block_id);
            if (cfg_block && cfg_block->loop_header) {
                reg_const.assign(ir.register_count, std::nullopt);
            }
        }
        if (insn.opcode == JitIrOpcode::LoadConst) {
            if (const std::optional<std::int64_t> value = integer_from_const(bytecode, insn.b)) {
                if (insn.a < reg_const.size()) {
                    reg_const[insn.a] = *value;
                }
            } else {
                reg_const.assign(ir.register_count, std::nullopt);
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadConstInt) {
            if (insn.a < reg_const.size()) {
                reg_const[insn.a] = insn.const_int_value;
            }
            insn.result_kind = JitValueKind::ConstInteger;
            continue;
        }
        if (insn.opcode == JitIrOpcode::MoveReg) {
            if (insn.b < reg_const.size() && reg_const[insn.b].has_value()) {
                if (insn.a < reg_const.size()) {
                    reg_const[insn.a] = reg_const[insn.b];
                }
            } else if (insn.a < reg_const.size()) {
                reg_const[insn.a] = std::nullopt;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal || insn.opcode == JitIrOpcode::StoreLocal) {
            reg_const.assign(ir.register_count, std::nullopt);
            continue;
        }
        const bool in_loop_body = loop_body.find(insn.block_id) != loop_body.end();
        if (insn.opcode == JitIrOpcode::Add && !in_loop_body && insn.b < reg_const.size() && insn.c < reg_const.size() &&
            reg_const[insn.b].has_value() && reg_const[insn.c].has_value()) {
            insn.opcode = JitIrOpcode::LoadConstInt;
            insn.const_int_value = *reg_const[insn.b] + *reg_const[insn.c];
            insn.b = 0;
            insn.c = 0;
            if (insn.a < reg_const.size()) {
                reg_const[insn.a] = insn.const_int_value;
            }
            insn.result_kind = JitValueKind::ConstInteger;
            ++stats.constant_folds;
            continue;
        }
        if (insn.opcode == JitIrOpcode::Mul && !in_loop_body && insn.b < reg_const.size() && insn.c < reg_const.size() &&
            reg_const[insn.b].has_value() && reg_const[insn.c].has_value()) {
            insn.opcode = JitIrOpcode::LoadConstInt;
            insn.const_int_value = *reg_const[insn.b] * *reg_const[insn.c];
            insn.b = 0;
            insn.c = 0;
            if (insn.a < reg_const.size()) {
                reg_const[insn.a] = insn.const_int_value;
            }
            insn.result_kind = JitValueKind::ConstInteger;
            ++stats.constant_folds;
            continue;
        }
        if (is_pure_arith(insn)) {
            reg_const.assign(ir.register_count, std::nullopt);
        }
        if (!has_loops && insn.opcode == JitIrOpcode::JumpIfFalse && insn.a < reg_const.size() &&
            reg_const[insn.a].has_value()) {
            if (*reg_const[insn.a] == 0) {
                insn.opcode = JitIrOpcode::Jump;
                insn.a = 0;
                ++stats.constant_folds;
            } else {
                insn.opcode = JitIrOpcode::Nop;
                ++stats.constant_folds;
            }
        }
    }
}

void remove_nops(JitFunctionIr& ir, OptimizingPassStats& stats) {
    std::vector<JitIrInstruction> kept;
    kept.reserve(ir.instructions.size());
    std::unordered_map<std::uint32_t, std::uint32_t> remap;
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        if (ir.instructions[i].opcode == JitIrOpcode::Nop) {
            ++stats.dead_removed;
            continue;
        }
        remap[static_cast<std::uint32_t>(i)] = static_cast<std::uint32_t>(kept.size());
        kept.push_back(ir.instructions[i]);
    }
    for (JitIrInstruction& insn : kept) {
        if (insn.opcode == JitIrOpcode::Jump || insn.opcode == JitIrOpcode::JumpIfFalse) {
            const auto it = remap.find(insn.jump_target_insn);
            if (it != remap.end()) {
                insn.jump_target_insn = it->second;
            }
        }
    }
    ir.instructions = std::move(kept);
}

bool closure_factory_body_is_pure(const Bytecode& bytecode, std::uint32_t function_index) {
    std::uint32_t inner = 0;
    if (!bytecode_function_returns_single_closure(bytecode, function_index, inner)) {
        return false;
    }
    if (function_index >= bytecode.functions.size()) {
        return false;
    }
    const FunctionInfo& fn = bytecode.functions[function_index];
    if (fn.entry >= bytecode.instructions.size()) {
        return false;
    }
    std::uint32_t end_ip = static_cast<std::uint32_t>(bytecode.instructions.size());
    for (const FunctionInfo& other : bytecode.functions) {
        if (other.entry > fn.entry && other.entry < end_ip) {
            end_ip = other.entry;
        }
    }
    for (std::uint32_t ip = fn.entry; ip < end_ip; ++ip) {
        switch (bytecode.instructions[ip].opcode) {
            case Opcode::LoadConst:
            case Opcode::LoadLocal:
            case Opcode::LoadUpvalue:
            case Opcode::StoreLocal:
            case Opcode::MakeClosure:
                break;
            case Opcode::Return:
                return true;
            default:
                return false;
        }
    }
    return false;
}

std::optional<std::int64_t> reg_integer_constant(const std::vector<std::optional<std::int64_t>>& reg_const,
                                                const Bytecode& bytecode, const JitFunctionIr& ir,
                                                std::uint32_t reg_index, std::uint32_t before_insn) {
    if (reg_index >= reg_const.size()) {
        return std::nullopt;
    }
    if (reg_const[reg_index]) {
        return reg_const[reg_index];
    }
    for (std::uint32_t i = before_insn; i > 0; --i) {
        const JitIrInstruction& prev = ir.instructions[i - 1];
        if (prev.a != reg_index) {
            continue;
        }
        if (prev.opcode == JitIrOpcode::LoadConstInt) {
            return prev.const_int_value;
        }
        if (prev.opcode == JitIrOpcode::LoadConst) {
            return integer_from_const(bytecode, prev.b);
        }
        break;
    }
    return std::nullopt;
}

void eliminate_dead_closure_factories(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    if (ir.instructions.empty()) {
        return;
    }
    if (ir.cfg.empty()) {
        build_cfg_for_function(ir);
    }
    if (ir.cfg.empty()) {
        return;
    }
    std::vector<std::optional<std::int64_t>> reg_const(ir.register_count);
    std::unordered_map<std::uint32_t, std::vector<std::optional<std::int64_t>>> closure_capture_const;
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::LoadConstInt && insn.a < reg_const.size()) {
            reg_const[insn.a] = insn.const_int_value;
        } else if (insn.opcode == JitIrOpcode::LoadConst && insn.a < reg_const.size()) {
            reg_const[insn.a] = integer_from_const(bytecode, insn.b);
        } else if (insn.opcode == JitIrOpcode::MoveReg && insn.a < reg_const.size()) {
            if (insn.b < reg_const.size() && reg_const[insn.b]) {
                reg_const[insn.a] = reg_const[insn.b];
            } else {
                reg_const[insn.a] = std::nullopt;
            }
        } else if (insn.opcode == JitIrOpcode::Call || insn.opcode == JitIrOpcode::CallValue ||
                   insn.opcode == JitIrOpcode::StoreLocal || insn.opcode == JitIrOpcode::LoadLocal) {
            if (insn.a < reg_const.size()) {
                reg_const[insn.a] = std::nullopt;
            }
        }
    }
    std::unordered_set<std::uint32_t> persist_closure_locals;
    for (std::size_t i = 0; i + 1 < ir.instructions.size(); ++i) {
        const JitIrInstruction& insn = ir.instructions[i];
        if (insn.opcode != JitIrOpcode::Call && insn.opcode != JitIrOpcode::CallValue) {
            continue;
        }
        for (std::size_t j = i + 1; j < std::min(i + 6, ir.instructions.size()); ++j) {
            const JitIrInstruction& next = ir.instructions[j];
            if (next.opcode == JitIrOpcode::StoreLocal && next.b == insn.a && next.a < ir.local_count) {
                persist_closure_locals.insert(next.a);
                break;
            }
            if (next.opcode == JitIrOpcode::Call || next.opcode == JitIrOpcode::CallValue ||
                next.opcode == JitIrOpcode::Jump || next.opcode == JitIrOpcode::JumpIfFalse) {
                break;
            }
        }
    }
    std::vector<std::optional<std::uint32_t>> stable_fn(ir.register_count);
    std::vector<std::optional<std::uint32_t>> stable_local(ir.local_count);
    auto clear_reg = [&](std::uint32_t reg) {
        if (reg < stable_fn.size()) {
            stable_fn[reg] = std::nullopt;
        }
    };
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::Call && insn.a < stable_fn.size()) {
            clear_reg(insn.a);
            std::uint32_t inner = 0;
            if (bytecode_function_returns_single_closure(bytecode, insn.c, inner)) {
                stable_fn[insn.a] = inner;
            }
        } else if (insn.opcode == JitIrOpcode::CallValue && insn.a < stable_fn.size()) {
            const std::optional<std::uint32_t> callee =
                insn.b < stable_fn.size() ? stable_fn[insn.b] : std::nullopt;
            clear_reg(insn.a);
            std::uint32_t inner = 0;
            if (callee && bytecode_function_returns_single_closure(bytecode, *callee, inner)) {
                stable_fn[insn.a] = inner;
            }
        } else if (insn.opcode == JitIrOpcode::MoveReg && insn.a < stable_fn.size()) {
            const std::optional<std::uint32_t> src = insn.b < stable_fn.size() ? stable_fn[insn.b] : std::nullopt;
            clear_reg(insn.a);
            if (src) {
                stable_fn[insn.a] = src;
            }
        } else if (insn.opcode == JitIrOpcode::StoreLocal && insn.a < stable_local.size()) {
            stable_local[insn.a] = insn.b < stable_fn.size() ? stable_fn[insn.b] : std::nullopt;
        } else if (insn.opcode == JitIrOpcode::LoadLocal && insn.a < stable_fn.size()) {
            clear_reg(insn.a);
            if (insn.b < stable_local.size() && stable_local[insn.b]) {
                stable_fn[insn.a] = stable_local[insn.b];
            }
        } else if (insn.opcode == JitIrOpcode::LoadConst || insn.opcode == JitIrOpcode::LoadConstInt ||
                   insn.opcode == JitIrOpcode::LoadUpvalue || insn.opcode == JitIrOpcode::LoadFunction ||
                   insn.opcode == JitIrOpcode::LoadClosureCapture || insn.opcode == JitIrOpcode::MakeClosure ||
                   is_pure_arith(insn)) {
            clear_reg(insn.a);
        }
    }

    auto links_persist_store = [&](std::size_t call_index, std::uint32_t result_reg) -> bool {
        for (std::size_t j = call_index + 1; j < std::min(call_index + 6, ir.instructions.size()); ++j) {
            const JitIrInstruction& next = ir.instructions[j];
            if (next.opcode == JitIrOpcode::StoreLocal && next.b == result_reg &&
                persist_closure_locals.contains(next.a)) {
                return true;
            }
            if (next.opcode == JitIrOpcode::Call || next.opcode == JitIrOpcode::CallValue ||
                next.opcode == JitIrOpcode::Jump || next.opcode == JitIrOpcode::JumpIfFalse) {
                return false;
            }
        }
        return false;
    };

    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        JitIrInstruction& insn = ir.instructions[i];
        if (insn.opcode == JitIrOpcode::Call && closure_factory_body_is_pure(bytecode, insn.c) &&
            links_persist_store(i, insn.a)) {
            std::uint32_t inner = 0;
            if (!bytecode_function_returns_single_closure(bytecode, insn.c, inner)) {
                continue;
            }
            const FunctionInfo& inner_fn = bytecode.functions[inner];
            if (inner_fn.captures.size() != 1) {
                continue;
            }
            const std::optional<std::int64_t> capture =
                reg_integer_constant(reg_const, bytecode, ir, insn.b, static_cast<std::uint32_t>(i));
            if (!capture) {
                continue;
            }
            insn.opcode = JitIrOpcode::MaterializeClosureIntCapture;
            insn.b = inner;
            insn.c = 0;
            insn.const_int_value = *capture;
            if (insn.a < stable_fn.size()) {
                stable_fn[insn.a] = inner;
            }
            closure_capture_const[inner] = {*capture};
            ++stats.specialized_ops;
            continue;
        }
        if (insn.opcode == JitIrOpcode::CallValue && insn.b < stable_fn.size() && stable_fn[insn.b] &&
            closure_factory_body_is_pure(bytecode, *stable_fn[insn.b]) && insn.c == 1 &&
            links_persist_store(i, insn.a)) {
            const std::uint32_t factory_fn = *stable_fn[insn.b];
            std::uint32_t inner = 0;
            if (!bytecode_function_returns_single_closure(bytecode, factory_fn, inner)) {
                continue;
            }
            const std::optional<std::int64_t> capture = reg_integer_constant(reg_const, bytecode, ir, insn.b + 1,
                                                                             static_cast<std::uint32_t>(i));
            if (!capture) {
                continue;
            }
            const FunctionInfo& inner_fn = bytecode.functions[inner];
            if (inner_fn.captures.size() == 2) {
                std::optional<std::int64_t> parent_cap;
                if (const auto parent_it = closure_capture_const.find(factory_fn);
                    parent_it != closure_capture_const.end() && !parent_it->second.empty() &&
                    parent_it->second[0].has_value()) {
                    parent_cap = parent_it->second[0];
                }
                if (!parent_cap) {
                    continue;
                }
                insn.opcode = JitIrOpcode::MaterializeClosureTwoIntCaptures;
                insn.b = inner;
                insn.const_int_value = *parent_cap;
                insn.c = static_cast<std::uint32_t>(*capture);
                if (insn.a < stable_fn.size()) {
                    stable_fn[insn.a] = inner;
                }
                closure_capture_const[inner] = {*parent_cap, *capture};
                ++stats.specialized_ops;
                continue;
            }
            if (inner_fn.captures.size() != 1) {
                continue;
            }
            insn.opcode = JitIrOpcode::MaterializeClosureIntCapture;
            insn.b = inner;
            insn.c = 0;
            insn.const_int_value = *capture;
            if (insn.a < stable_fn.size()) {
                stable_fn[insn.a] = inner;
            }
            closure_capture_const[inner] = {*capture};
            ++stats.specialized_ops;
        }
    }
}

void dead_code_elimination(JitFunctionIr& ir, OptimizingPassStats& stats) {
    std::vector<bool> used_reg(ir.register_count, false);
    for (auto it = ir.instructions.rbegin(); it != ir.instructions.rend(); ++it) {
        JitIrInstruction& insn = *it;
        if (insn.opcode == JitIrOpcode::Return) {
            if (insn.a < used_reg.size()) {
                used_reg[insn.a] = true;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::JumpIfFalse) {
            if (insn.a < used_reg.size()) {
                used_reg[insn.a] = true;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal) {
            if (insn.b < used_reg.size()) {
                used_reg[insn.b] = true;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::Call || insn.opcode == JitIrOpcode::CallValue) {
            if (insn.a < used_reg.size()) {
                used_reg[insn.a] = true;
            }
            const std::uint32_t arg_regs =
                insn.opcode == JitIrOpcode::CallValue ? insn.c + 1 : 8;
            for (std::uint32_t arg = 0; arg < arg_regs && arg < 16; ++arg) {
                const std::uint32_t reg = insn.b + arg;
                if (reg < used_reg.size()) {
                    used_reg[reg] = true;
                }
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::GuardInteger) {
            if (insn.a < used_reg.size()) {
                used_reg[insn.a] = true;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::MoveReg) {
            const bool dest_used = insn.a < used_reg.size() && used_reg[insn.a];
            if (!dest_used) {
                insn.opcode = JitIrOpcode::Nop;
                ++stats.dead_removed;
                continue;
            }
            if (insn.b < used_reg.size()) {
                used_reg[insn.b] = true;
            }
            continue;
        }
        if (is_pure_arith(insn) || insn.opcode == JitIrOpcode::LoadConst || insn.opcode == JitIrOpcode::LoadConstInt ||
            insn.opcode == JitIrOpcode::LoadLocal) {
            const bool dest_used = insn.a < used_reg.size() && used_reg[insn.a];
            if (!dest_used && insn.opcode != JitIrOpcode::LoadLocal) {
                insn.opcode = JitIrOpcode::Nop;
                ++stats.dead_removed;
                continue;
            }
            if (insn.b < used_reg.size()) {
                used_reg[insn.b] = true;
            }
            if (insn.c < used_reg.size()) {
                used_reg[insn.c] = true;
            }
        }
    }
    remove_nops(ir, stats);
}

void common_subexpression_elimination(JitFunctionIr& ir, OptimizingPassStats& stats) {
    struct Key {
        JitIrOpcode op;
        std::uint32_t b;
        std::uint32_t c;
        bool operator==(const Key& other) const { return op == other.op && b == other.b && c == other.c; }
    };
    struct KeyHash {
        std::size_t operator()(const Key& key) const {
            return (static_cast<std::size_t>(key.op) << 32) ^ (static_cast<std::size_t>(key.b) << 16) ^ key.c;
        }
    };
    std::unordered_map<Key, std::uint32_t, KeyHash> available;
    std::uint32_t block = 0;
    for (std::size_t idx = 0; idx < ir.instructions.size(); ++idx) {
        JitIrInstruction& insn = ir.instructions[idx];
        if (insn.block_id != block) {
            available.clear();
            block = insn.block_id;
            for (const JitCfgBlock& cfg_block : ir.cfg) {
                if (cfg_block.id == insn.block_id && cfg_block.loop_header) {
                    available.clear();
                    break;
                }
            }
        }
        if (insn.opcode == JitIrOpcode::StoreLocal || insn.opcode == JitIrOpcode::LoadLocal ||
            insn.opcode == JitIrOpcode::Call || insn.opcode == JitIrOpcode::CallValue ||
            insn.opcode == JitIrOpcode::MakeStruct || insn.opcode == JitIrOpcode::SetField ||
            insn.opcode == JitIrOpcode::MakeList || insn.opcode == JitIrOpcode::MakeDict ||
            insn.opcode == JitIrOpcode::MakeTuple || insn.opcode == JitIrOpcode::MakeSet) {
            available.clear();
        }
        if (insn_terminates_block(insn)) {
            available.clear();
        }
        if (insn.opcode == JitIrOpcode::Jump && insn.jump_target_insn <= idx) {
            available.clear();
        } else if (insn.opcode == JitIrOpcode::JumpIfFalse && insn.jump_target_insn <= idx) {
            available.clear();
        }
        if (!is_pure_arith(insn) || insn.opcode == JitIrOpcode::LoadConstInt) {
            continue;
        }
        const Key key{insn.opcode, insn.b, insn.c};
        const auto it = available.find(key);
        if (it != available.end()) {
            insn.opcode = JitIrOpcode::MoveReg;
            insn.b = it->second;
            insn.c = 0;
            ++stats.cse_hits;
            continue;
        }
        available[key] = insn.a;
    }
}

bool ir_has_dynamic_value_compare_ops(const JitFunctionIr& ir) {
    for (const JitIrInstruction& insn : ir.instructions) {
        switch (insn.opcode) {
            case JitIrOpcode::Equal:
            case JitIrOpcode::NotEqual:
            case JitIrOpcode::StringEqual:
            case JitIrOpcode::StringNotEqual:
                return true;
            default:
                break;
        }
    }
    return false;
}

bool ir_loads_string_constants(const Bytecode& bytecode, const JitFunctionIr& ir) {
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::LoadConst) {
            continue;
        }
        if (insn.b >= bytecode.constants.size()) {
            continue;
        }
        if (bytecode.constants[insn.b].kind == ConstantKind::String) {
            return true;
        }
    }
    return false;
}

void remap_jump_targets(std::vector<JitIrInstruction>& instructions, const std::vector<std::uint32_t>& index_map) {
    for (JitIrInstruction& insn : instructions) {
        if (insn.opcode == JitIrOpcode::Jump || insn.opcode == JitIrOpcode::JumpIfFalse ||
            insn.opcode == JitIrOpcode::IterNext) {
            const std::uint32_t target = insn.jump_target_insn;
            if (target < index_map.size()) {
                insn.jump_target_insn = index_map[target];
            }
        }
    }
}

std::vector<JitIrInstruction> insert_instruction_block(std::vector<JitIrInstruction> instructions,
                                                     std::uint32_t position,
                                                     const std::vector<JitIrInstruction>& block) {
    if (block.empty()) {
        return instructions;
    }
    position = std::min(position, static_cast<std::uint32_t>(instructions.size()));
    std::vector<std::uint32_t> index_map(instructions.size());
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        index_map[i] = static_cast<std::uint32_t>(i < position ? i : i + block.size());
    }
    std::vector<JitIrInstruction> out;
    out.reserve(instructions.size() + block.size());
    out.insert(out.end(), instructions.begin(), instructions.begin() + static_cast<std::ptrdiff_t>(position));
    out.insert(out.end(), block.begin(), block.end());
    out.insert(out.end(), instructions.begin() + static_cast<std::ptrdiff_t>(position), instructions.end());
    remap_jump_targets(out, index_map);
    return out;
}

std::vector<JitIrInstruction> make_shadow_preload_block(const std::vector<std::int32_t>& local_to_shadow,
                                                        std::uint32_t bytecode_ip) {
    std::vector<JitIrInstruction> block;
    for (std::uint32_t local = 0; local < local_to_shadow.size(); ++local) {
        if (local_to_shadow[local] < 0) {
            continue;
        }
        JitIrInstruction load;
        load.opcode = JitIrOpcode::LoadLocal;
        load.a = static_cast<std::uint32_t>(local_to_shadow[local]);
        load.b = local;
        load.bytecode_ip = bytecode_ip;
        block.push_back(load);
    }
    return block;
}

std::vector<JitIrInstruction> make_shadow_flush_block(const std::vector<std::int32_t>& local_to_shadow,
                                                      std::uint32_t bytecode_ip) {
    std::vector<JitIrInstruction> block;
    for (std::uint32_t local = 0; local < local_to_shadow.size(); ++local) {
        if (local_to_shadow[local] < 0) {
            continue;
        }
        JitIrInstruction store;
        store.opcode = JitIrOpcode::StoreLocal;
        store.a = local;
        store.b = static_cast<std::uint32_t>(local_to_shadow[local]);
        store.bytecode_ip = bytecode_ip;
        block.push_back(store);
    }
    return block;
}

std::vector<bool> infer_integer_only_locals(const Bytecode& bytecode, const JitFunctionIr& ir);

bool ir_allocates_containers(const JitFunctionIr& ir) {
    for (const JitIrInstruction& insn : ir.instructions) {
        switch (insn.opcode) {
            case JitIrOpcode::MakeList:
            case JitIrOpcode::MakeDict:
            case JitIrOpcode::MakeTuple:
            case JitIrOpcode::MakeSet:
                return true;
            default:
                break;
        }
    }
    return false;
}

void restore_shadow_integer_after_conditional_branch(JitFunctionIr& ir, OptimizingPassStats& stats) {
    if (!ir.buffered_integer_locals || ir.integer_local_shadow_reg.empty()) {
        return;
    }
    // Shadow integer registers stay valid across IntLess/IntEqual; reloading from locals here
    // reads potentially stale cells when loop stores are buffered in shadow registers only.
}

void buffer_integer_locals_impl(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    if (!ir.profile_stable_integer_ops || ir.local_count == 0) {
        return;
    }
    if (ir_allocates_containers(ir)) {
        return;
    }
    if (ir.cfg.empty()) {
        build_cfg_for_function(ir);
    }
    const bool has_loop_header =
        std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& block) { return block.loop_header; });
    if (!has_loop_header) {
        return;
    }
    const std::vector<bool> integer_locals = infer_integer_only_locals(bytecode, ir);
    std::vector<bool> local_used(ir.local_count, false);
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::LoadLocal && insn.b < local_used.size()) {
            local_used[insn.b] = true;
        } else if (insn.opcode == JitIrOpcode::StoreLocal && insn.a < local_used.size()) {
            local_used[insn.a] = true;
        }
    }
    const std::uint32_t shadow_base = ir.register_count;
    std::uint32_t shadow_count = 0;
    std::vector<std::int32_t> local_to_shadow(ir.local_count, -1);
    for (std::uint32_t local = 0; local < ir.local_count; ++local) {
        if (!local_used[local] || local >= integer_locals.size() || !integer_locals[local]) {
            continue;
        }
        local_to_shadow[local] = static_cast<std::int32_t>(shadow_base + shadow_count);
        ++shadow_count;
    }
    if (shadow_count == 0) {
        return;
    }
    ir.register_count += shadow_count;
    ir.buffered_integer_locals = true;
    ir.integer_local_shadow_reg = std::move(local_to_shadow);
    std::vector<bool> captured_local(ir.local_count, false);
    if (ir.function_index < bytecode.functions.size()) {
        for (const Capture& capture : bytecode.functions[ir.function_index].captures) {
            if (capture.from_local && capture.index < captured_local.size()) {
                captured_local[capture.index] = true;
            }
        }
    }
    const auto& local_to_shadow_ref = ir.integer_local_shadow_reg;
    std::vector<JitIrInstruction> rewritten;
    rewritten.reserve(ir.instructions.size() + shadow_count * 4 + 16);
    const std::uint32_t entry_ip = ir.instructions.empty() ? 0 : ir.instructions.front().bytecode_ip;
    const std::vector<JitIrInstruction> entry_preload = make_shadow_preload_block(local_to_shadow_ref, entry_ip);
    rewritten.insert(rewritten.end(), entry_preload.begin(), entry_preload.end());
    std::vector<std::uint32_t> old_to_new(ir.instructions.size());
    for (std::size_t old_i = 0; old_i < ir.instructions.size(); ++old_i) {
        old_to_new[old_i] = static_cast<std::uint32_t>(rewritten.size());
        const JitIrInstruction& insn = ir.instructions[old_i];
        if (insn.opcode == JitIrOpcode::LoadLocal && insn.b < local_to_shadow_ref.size() &&
            local_to_shadow_ref[insn.b] >= 0) {
            JitIrInstruction move = insn;
            move.opcode = JitIrOpcode::MoveReg;
            move.b = static_cast<std::uint32_t>(local_to_shadow_ref[insn.b]);
            rewritten.push_back(move);
            ++stats.specialized_ops;
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal && insn.a < local_to_shadow_ref.size() &&
            local_to_shadow_ref[insn.a] >= 0) {
            JitIrInstruction move;
            move.opcode = JitIrOpcode::MoveReg;
            move.a = static_cast<std::uint32_t>(local_to_shadow_ref[insn.a]);
            move.b = insn.b;
            move.bytecode_ip = insn.bytecode_ip;
            move.block_id = insn.block_id;
            rewritten.push_back(move);
            if (insn.a < captured_local.size() && captured_local[insn.a]) {
                rewritten.push_back(insn);
            }
            ++stats.specialized_ops;
            continue;
        }
        rewritten.push_back(insn);
    }
    remap_jump_targets(rewritten, old_to_new);

    const std::vector<JitIrInstruction> flush_block = make_shadow_flush_block(local_to_shadow_ref, entry_ip);

    auto collect_loop_headers = [&](const std::vector<JitIrInstruction>& insns) {
        ir.instructions = insns;
        build_cfg_for_function(ir);
        std::vector<std::uint32_t> headers;
        for (const JitCfgBlock& block : ir.cfg) {
            if (block.loop_header) {
                headers.push_back(block.first_insn);
            }
        }
        std::sort(headers.begin(), headers.end());
        headers.erase(std::unique(headers.begin(), headers.end()), headers.end());
        return headers;
    };

    std::vector<std::uint32_t> loop_headers = collect_loop_headers(rewritten);
    // Loop-header shadow preload is omitted on backedges: shadow registers remain authoritative
    // between flush-at-backedge and the next iteration. Entry preload covers function entry;
    // OSR lands on the loop header bytecode insn with interpreter locals already materialized.
    loop_headers = collect_loop_headers(rewritten);
    std::vector<std::uint32_t> insert_sites;
    for (std::size_t i = 0; i < rewritten.size(); ++i) {
        const JitIrInstruction& insn = rewritten[i];
        if (insn.opcode == JitIrOpcode::Return && !flush_block.empty()) {
            insert_sites.push_back(static_cast<std::uint32_t>(i));
        }
    }
    std::sort(insert_sites.begin(), insert_sites.end(), std::greater<std::uint32_t>());
    for (const std::uint32_t site : insert_sites) {
        rewritten = insert_instruction_block(std::move(rewritten), site, flush_block);
    }

    ir.instructions = std::move(rewritten);
    build_cfg_for_function(ir);
}

}  // namespace

namespace {

bool ir_loads_float_constants(const Bytecode& bytecode, const JitFunctionIr& ir) {
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::LoadConst) {
            continue;
        }
        if (insn.b >= bytecode.constants.size()) {
            continue;
        }
        if (bytecode.constants[insn.b].kind == ConstantKind::Float) {
            return true;
        }
    }
    return false;
}

std::optional<JitValueKind> kind_from_load_const(const Bytecode& bytecode, std::uint32_t const_index) {
    if (const_index >= bytecode.constants.size()) {
        return std::nullopt;
    }
    switch (bytecode.constants[const_index].kind) {
        case ConstantKind::Integer:
            return JitValueKind::Integer;
        case ConstantKind::Float:
            return JitValueKind::Float;
        case ConstantKind::Bool:
            return JitValueKind::Bool;
        case ConstantKind::String:
            return JitValueKind::String;
        default:
            return std::nullopt;
    }
}

void fold_constant_string_comparisons(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    for (std::size_t i = 0; i + 2 < ir.instructions.size(); ++i) {
        JitIrInstruction& left_load = ir.instructions[i];
        JitIrInstruction& right_load = ir.instructions[i + 1];
        JitIrInstruction& cmp = ir.instructions[i + 2];
        if (left_load.opcode != JitIrOpcode::LoadConst || right_load.opcode != JitIrOpcode::LoadConst) {
            continue;
        }
        if (cmp.opcode != JitIrOpcode::StringEqual && cmp.opcode != JitIrOpcode::StringNotEqual) {
            continue;
        }
        if (kind_from_load_const(bytecode, left_load.b) != JitValueKind::String ||
            kind_from_load_const(bytecode, right_load.b) != JitValueKind::String) {
            continue;
        }
        const Constant& left = bytecode.constants[left_load.b];
        const Constant& right = bytecode.constants[right_load.b];
        const bool same = Runtime::strings_equal(left.lexeme, right.lexeme);
        const bool value = cmp.opcode == JitIrOpcode::StringEqual ? same : !same;
        left_load.opcode = JitIrOpcode::Nop;
        right_load.opcode = JitIrOpcode::Nop;
        cmp.opcode = JitIrOpcode::LoadConstInt;
        cmp.const_int_value = value ? 1 : 0;
        cmp.b = 0;
        cmp.c = 0;
        cmp.result_kind = JitValueKind::ConstInteger;
        ++stats.constant_folds;
    }
    OptimizingPassStats cleanup;
    remove_nops(ir, cleanup);
    stats.dead_removed += cleanup.dead_removed;

    std::vector<std::optional<std::uint32_t>> reg_string_const(ir.register_count);
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::LoadConst) {
            if (insn.a < reg_string_const.size()) {
                if (kind_from_load_const(bytecode, insn.b) == JitValueKind::String) {
                    reg_string_const[insn.a] = insn.b;
                } else {
                    reg_string_const[insn.a] = std::nullopt;
                }
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::MoveReg) {
            if (insn.a < reg_string_const.size()) {
                reg_string_const[insn.a] =
                    insn.b < reg_string_const.size() ? reg_string_const[insn.b] : std::optional<std::uint32_t>{};
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal || insn.opcode == JitIrOpcode::StoreLocal) {
            reg_string_const.assign(ir.register_count, std::nullopt);
            continue;
        }
        const auto fold_cmp = [&](bool equal) {
            if (insn.b >= reg_string_const.size() || insn.c >= reg_string_const.size()) {
                return;
            }
            const std::optional<std::uint32_t> left_idx = reg_string_const[insn.b];
            const std::optional<std::uint32_t> right_idx = reg_string_const[insn.c];
            if (!left_idx || !right_idx || *left_idx >= bytecode.constants.size() ||
                *right_idx >= bytecode.constants.size()) {
                return;
            }
            const Constant& left = bytecode.constants[*left_idx];
            const Constant& right = bytecode.constants[*right_idx];
            if (left.kind != ConstantKind::String || right.kind != ConstantKind::String) {
                return;
            }
            const bool same = Runtime::strings_equal(left.lexeme, right.lexeme);
            insn.opcode = JitIrOpcode::LoadConstInt;
            insn.const_int_value = (equal ? same : !same) ? 1 : 0;
            insn.b = 0;
            insn.c = 0;
            insn.result_kind = JitValueKind::ConstInteger;
            if (insn.a < reg_string_const.size()) {
                reg_string_const[insn.a] = std::nullopt;
            }
            ++stats.constant_folds;
        };
        if (insn.opcode == JitIrOpcode::StringEqual) {
            fold_cmp(true);
        } else if (insn.opcode == JitIrOpcode::StringNotEqual) {
            fold_cmp(false);
        }
    }
}

void fold_constant_condition_branches(JitFunctionIr& ir, OptimizingPassStats& stats) {
    for (std::size_t i = 0; i + 1 < ir.instructions.size(); ++i) {
        const JitIrInstruction& cond = ir.instructions[i];
        JitIrInstruction& branch = ir.instructions[i + 1];
        if (cond.opcode != JitIrOpcode::LoadConstInt || branch.opcode != JitIrOpcode::JumpIfFalse ||
            branch.a != cond.a) {
            continue;
        }
        if (cond.const_int_value == 0) {
            branch.opcode = JitIrOpcode::Jump;
            branch.a = 0;
            ++stats.constant_folds;
        } else if (cond.const_int_value != 0) {
            branch.opcode = JitIrOpcode::Nop;
            branch.a = 0;
            ++stats.constant_folds;
        }
    }
    OptimizingPassStats cleanup;
    remove_nops(ir, cleanup);
    stats.dead_removed += cleanup.dead_removed;
}

void specialize_string_ops(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    std::vector<std::optional<JitValueKind>> reg_kind(ir.register_count);
    std::vector<std::optional<JitValueKind>> local_kind(ir.local_count);
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::LoadConstInt) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = JitValueKind::Integer;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadConst) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = kind_from_load_const(bytecode, insn.b);
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal) {
            if (insn.a < reg_kind.size() && insn.b < local_kind.size() && local_kind[insn.b]) {
                reg_kind[insn.a] = local_kind[insn.b];
            } else if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = std::nullopt;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal) {
            if (insn.b < reg_kind.size() && insn.a < local_kind.size() && reg_kind[insn.b]) {
                local_kind[insn.a] = reg_kind[insn.b];
            } else if (insn.a < local_kind.size()) {
                local_kind[insn.a] = std::nullopt;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::MoveReg) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] =
                    insn.b < reg_kind.size() ? reg_kind[insn.b] : std::optional<JitValueKind>{std::nullopt};
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::Add) {
            continue;
        }
        if (insn.opcode == JitIrOpcode::Equal || insn.opcode == JitIrOpcode::NotEqual) {
            const std::optional<JitValueKind> left =
                insn.b < reg_kind.size() ? reg_kind[insn.b] : std::optional<JitValueKind>{std::nullopt};
            const std::optional<JitValueKind> right =
                insn.c < reg_kind.size() ? reg_kind[insn.c] : std::optional<JitValueKind>{std::nullopt};
            if (left == JitValueKind::String && right == JitValueKind::String) {
                insn.opcode = insn.opcode == JitIrOpcode::Equal ? JitIrOpcode::StringEqual : JitIrOpcode::StringNotEqual;
                ++stats.specialized_ops;
                if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] = JitValueKind::Bool;
                }
            }
        }
    }
}

std::optional<JitValueKind> join_numeric_kind(const std::optional<JitValueKind>& left,
                                              const std::optional<JitValueKind>& right) {
    if (!left || !right) {
        return std::nullopt;
    }
    if (*left == JitValueKind::Integer && *right == JitValueKind::Integer) {
        return JitValueKind::Integer;
    }
    if ((*left == JitValueKind::Integer || *left == JitValueKind::Float) &&
        (*right == JitValueKind::Integer || *right == JitValueKind::Float)) {
        if (*left == JitValueKind::Float || *right == JitValueKind::Float) {
            return JitValueKind::Float;
        }
    }
    return std::nullopt;
}

std::vector<bool> infer_integer_only_locals(const Bytecode& bytecode, const JitFunctionIr& ir) {
    std::vector<bool> eligible(ir.local_count, false);
    if (ir.local_count == 0) {
        return eligible;
    }
    std::vector<std::optional<JitValueKind>> reg_kind(ir.register_count);
    std::vector<std::optional<JitValueKind>> local_kind(ir.local_count);
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::LoadConstInt) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = JitValueKind::Integer;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadConst) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = kind_from_load_const(bytecode, insn.b);
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::MakeList || insn.opcode == JitIrOpcode::MakeDict ||
            insn.opcode == JitIrOpcode::MakeTuple || insn.opcode == JitIrOpcode::MakeSet ||
            insn.opcode == JitIrOpcode::MakeStruct || insn.opcode == JitIrOpcode::GetIndex ||
            insn.opcode == JitIrOpcode::ListGetIndex || insn.opcode == JitIrOpcode::TupleGetIndex ||
            insn.opcode == JitIrOpcode::BytesGetIndex || insn.opcode == JitIrOpcode::DictGetStringKey ||
            insn.opcode == JitIrOpcode::ListGetIntElement || insn.opcode == JitIrOpcode::Call ||
            insn.opcode == JitIrOpcode::CallValue || insn.opcode == JitIrOpcode::GetIter ||
            insn.opcode == JitIrOpcode::IterNext || insn.opcode == JitIrOpcode::LoadFunction ||
            insn.opcode == JitIrOpcode::MakeClosure) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = std::nullopt;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal) {
            if (insn.a < reg_kind.size() && insn.b < local_kind.size() && local_kind[insn.b]) {
                reg_kind[insn.a] = local_kind[insn.b];
            } else if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = std::nullopt;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal) {
            if (insn.b < reg_kind.size() && insn.a < local_kind.size() && reg_kind[insn.b]) {
                local_kind[insn.a] = reg_kind[insn.b];
            } else if (insn.a < local_kind.size()) {
                local_kind[insn.a] = std::nullopt;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::MoveReg) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] =
                    insn.b < reg_kind.size() ? reg_kind[insn.b] : std::optional<JitValueKind>{std::nullopt};
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::IntAdd || insn.opcode == JitIrOpcode::IntSub ||
            insn.opcode == JitIrOpcode::IntMul || insn.opcode == JitIrOpcode::IntMod ||
            insn.opcode == JitIrOpcode::IntDiv) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = JitValueKind::Integer;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::Add || insn.opcode == JitIrOpcode::Sub || insn.opcode == JitIrOpcode::Mul ||
            insn.opcode == JitIrOpcode::Mod) {
            const std::optional<JitValueKind> left =
                insn.b < reg_kind.size() ? reg_kind[insn.b] : std::optional<JitValueKind>{std::nullopt};
            const std::optional<JitValueKind> right =
                insn.c < reg_kind.size() ? reg_kind[insn.c] : std::optional<JitValueKind>{std::nullopt};
            const std::optional<JitValueKind> joined = join_numeric_kind(left, right);
            if (joined && insn.a < reg_kind.size()) {
                reg_kind[insn.a] = *joined;
            } else if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = std::nullopt;
            }
        }
    }
    for (std::uint32_t local = 0; local < ir.local_count; ++local) {
        if (local_kind[local] == JitValueKind::Integer) {
            eligible[local] = true;
        }
    }
    return eligible;
}

void set_numeric_specialization(JitIrInstruction& insn, JitValueKind kind, OptimizingPassStats& stats) {
    switch (insn.opcode) {
        case JitIrOpcode::Add:
            insn.opcode = kind == JitValueKind::Float ? JitIrOpcode::FloatAdd : JitIrOpcode::IntAdd;
            ++stats.specialized_ops;
            break;
        case JitIrOpcode::Sub:
            insn.opcode = kind == JitValueKind::Float ? JitIrOpcode::FloatSub : JitIrOpcode::IntSub;
            ++stats.specialized_ops;
            break;
        case JitIrOpcode::Mul:
            insn.opcode = kind == JitValueKind::Float ? JitIrOpcode::FloatMul : JitIrOpcode::IntMul;
            ++stats.specialized_ops;
            break;
        case JitIrOpcode::Div:
            if (kind == JitValueKind::Float) {
                insn.opcode = JitIrOpcode::FloatDiv;
            } else if (kind == JitValueKind::Integer) {
                insn.opcode = JitIrOpcode::IntDiv;
            }
            ++stats.specialized_ops;
            break;
        case JitIrOpcode::Less:
            insn.opcode = kind == JitValueKind::Float ? JitIrOpcode::FloatLess : JitIrOpcode::IntLess;
            ++stats.specialized_ops;
            break;
        case JitIrOpcode::Equal:
            insn.opcode = kind == JitValueKind::Float ? JitIrOpcode::FloatEqual : JitIrOpcode::IntEqual;
            ++stats.specialized_ops;
            break;
        case JitIrOpcode::Mod:
            if (kind == JitValueKind::Integer) {
                insn.opcode = JitIrOpcode::IntMod;
                ++stats.specialized_ops;
            }
            break;
        default:
            break;
    }
}

void specialize_integer_ops(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                            OptimizingPassStats& stats) {
    const FunctionOptHints* hints =
        profile ? profile->function_hints(ir.function_index) : nullptr;
    if (hints && hints->type_integer_unstable) {
        return;
    }
    const bool specialize = ir.profile_stable_integer_ops || (hints && (hints->stable_integer_ops || hints->very_hot));
    if (!specialize) {
        return;
    }
    const bool has_float_constants = ir_loads_float_constants(bytecode, ir);
    if (has_float_constants) {
        ir.profile_stable_float_ops = true;
    }
    build_cfg_for_function(ir);
    const bool has_loop_headers =
        std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& block) { return block.loop_header; });
    std::vector<std::optional<JitValueKind>> reg_kind(ir.register_count);
    std::vector<std::optional<JitValueKind>> local_kind(ir.local_count);
    if (hints != nullptr && hints->stable_integer_ops && !has_loop_headers &&
        ir.function_index < bytecode.functions.size()) {
        const FunctionInfo& fn = bytecode.functions[ir.function_index];
        const std::size_t orig_count = ir.instructions.size();
        std::vector<JitIrInstruction> guarded;
        std::vector<std::uint32_t> old_to_new(orig_count, 0xFFFFFFFFu);
        guarded.reserve(orig_count + fn.arity);
        for (std::size_t old_idx = 0; old_idx < orig_count; ++old_idx) {
            const JitIrInstruction& insn = ir.instructions[old_idx];
            old_to_new[old_idx] = static_cast<std::uint32_t>(guarded.size());
            guarded.push_back(insn);
            if (insn.opcode == JitIrOpcode::LoadLocal && insn.b < fn.arity) {
                JitIrInstruction guard;
                guard.opcode = JitIrOpcode::GuardInteger;
                guard.a = insn.a;
                guard.bytecode_ip = insn.bytecode_ip;
                guarded.push_back(guard);
            }
        }
        if (guarded.size() != orig_count) {
            for (JitIrInstruction& insn : guarded) {
                if (insn.opcode != JitIrOpcode::Jump && insn.opcode != JitIrOpcode::JumpIfFalse &&
                    insn.opcode != JitIrOpcode::IterNext) {
                    continue;
                }
                if (insn.jump_target_insn < old_to_new.size() && old_to_new[insn.jump_target_insn] != 0xFFFFFFFFu) {
                    insn.jump_target_insn = old_to_new[insn.jump_target_insn];
                }
            }
            ir.instructions = std::move(guarded);
            build_cfg_for_function(ir);
        }
        for (std::uint32_t p = 0; p < fn.arity && p < local_kind.size(); ++p) {
            local_kind[p] = JitValueKind::Integer;
        }
    }
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::GuardInteger) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = JitValueKind::Integer;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadConstInt) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = JitValueKind::Integer;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadConst) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = kind_from_load_const(bytecode, insn.b);
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal) {
            if (insn.a < reg_kind.size() && insn.b < local_kind.size() && local_kind[insn.b]) {
                reg_kind[insn.a] = local_kind[insn.b];
            } else if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] = std::nullopt;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal) {
            if (insn.b < reg_kind.size() && insn.a < local_kind.size() && reg_kind[insn.b]) {
                local_kind[insn.a] = reg_kind[insn.b];
            } else if (insn.a < local_kind.size()) {
                local_kind[insn.a] = std::nullopt;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::MoveReg) {
            if (insn.a < reg_kind.size()) {
                reg_kind[insn.a] =
                    insn.b < reg_kind.size() ? reg_kind[insn.b] : std::optional<JitValueKind>{std::nullopt};
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::Add || insn.opcode == JitIrOpcode::Sub || insn.opcode == JitIrOpcode::Mul ||
            insn.opcode == JitIrOpcode::Div || insn.opcode == JitIrOpcode::Less || insn.opcode == JitIrOpcode::Equal ||
            insn.opcode == JitIrOpcode::Mod) {
            const std::optional<JitValueKind> left =
                insn.b < reg_kind.size() ? reg_kind[insn.b] : std::optional<JitValueKind>{std::nullopt};
            const std::optional<JitValueKind> right =
                insn.c < reg_kind.size() ? reg_kind[insn.c] : std::optional<JitValueKind>{std::nullopt};
            std::optional<JitValueKind> joined = join_numeric_kind(left, right);
            if (!joined && !has_float_constants && !ir_loads_string_constants(bytecode, ir)) {
                const bool left_unknown = insn.b >= reg_kind.size() || !reg_kind[insn.b];
                const bool right_unknown = insn.c >= reg_kind.size() || !reg_kind[insn.c];
                if (!left_unknown && !right_unknown) {
                    joined = JitValueKind::Integer;
                }
            }
            if (joined) {
                set_numeric_specialization(insn, *joined, stats);
                if (insn.a < reg_kind.size()) {
                    if (insn.opcode == JitIrOpcode::IntLess || insn.opcode == JitIrOpcode::IntEqual ||
                        insn.opcode == JitIrOpcode::FloatLess || insn.opcode == JitIrOpcode::FloatEqual) {
                        reg_kind[insn.a] = JitValueKind::Bool;
                    } else {
                        reg_kind[insn.a] = *joined;
                    }
                }
            }
        }
    }
    build_cfg_for_function(ir);
}

void branch_optimization(JitFunctionIr& ir, const JitOptProfile* profile) {
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::JumpIfFalse) {
            continue;
        }
        bool found = false;
        double bias = 0.5;
        if (profile) {
            bias = profile->branch_bias_for_ip(ir.function_index, insn.bytecode_ip, found);
        }
        insn.branch_fallthrough_hot = !found || bias >= 0.5;
    }
}

std::optional<std::int64_t> int_reg_constant_at(const Bytecode& bytecode, const JitFunctionIr& ir, std::uint32_t reg,
                                                std::uint32_t up_to_insn) {
    for (std::uint32_t i = up_to_insn; i > 0; --i) {
        const JitIrInstruction& prev = ir.instructions[i - 1];
        if (prev.a != reg) {
            continue;
        }
        if (prev.opcode == JitIrOpcode::LoadConstInt) {
            return prev.const_int_value;
        }
        if (prev.opcode == JitIrOpcode::LoadConst) {
            return integer_from_const(bytecode, prev.b);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void fuse_inplace_int_increment_before_store(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    for (std::size_t i = 0; i + 1 < ir.instructions.size(); ++i) {
        JitIrInstruction& add = ir.instructions[i];
        if (add.opcode != JitIrOpcode::IntAdd) {
            continue;
        }
        const std::optional<std::int64_t> rhs = int_reg_constant_at(bytecode, ir, add.c, static_cast<std::uint32_t>(i));
        if (!rhs || *rhs != 1) {
            continue;
        }
        const JitIrInstruction& store = ir.instructions[i + 1];
        if (store.opcode != JitIrOpcode::StoreLocal || store.b != add.a || add.a == add.b) {
            continue;
        }
        add.a = add.b;
        ++stats.specialized_ops;
    }
}

void remap_jump_targets_after_insert(JitFunctionIr& ir, std::uint32_t insert_pos, std::uint32_t delta) {
    if (delta == 0) {
        return;
    }
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::Jump || insn.opcode == JitIrOpcode::JumpIfFalse ||
            insn.opcode == JitIrOpcode::IterNext) {
            if (insn.jump_target_insn >= insert_pos) {
                insn.jump_target_insn += delta;
            }
        }
    }
    for (JitCfgBlock& block : ir.cfg) {
        if (block.first_insn >= insert_pos) {
            block.first_insn += delta;
        }
    }
}

void loop_invariant_motion(JitFunctionIr& ir, OptimizingPassStats& stats) {
    if (ir.cfg.empty()) {
        return;
    }
    std::vector<const JitCfgBlock*> loop_headers;
    for (const JitCfgBlock& block : ir.cfg) {
        if (block.loop_header) {
            loop_headers.push_back(&block);
        }
    }
    std::sort(loop_headers.begin(), loop_headers.end(),
              [](const JitCfgBlock* a, const JitCfgBlock* b) { return a->first_insn > b->first_insn; });
    for (const JitCfgBlock* header : loop_headers) {
        const JitCfgBlock& block = *header;
        if (block.first_insn >= ir.instructions.size()) {
            continue;
        }
        const std::uint32_t body_end =
            std::min(block.first_insn + block.insn_count, static_cast<std::uint32_t>(ir.instructions.size()));
        std::unordered_set<std::uint32_t> stored_locals;
        for (std::uint32_t idx = block.first_insn; idx < body_end; ++idx) {
            const JitIrInstruction& insn = ir.instructions[idx];
            if (insn.opcode == JitIrOpcode::StoreLocal) {
                stored_locals.insert(insn.a);
            }
        }
        std::unordered_map<std::uint32_t, std::uint32_t> invariant_local_reg;
        std::vector<JitIrInstruction> hoisted;
        for (std::uint32_t idx = block.first_insn; idx < body_end; ++idx) {
            if (idx <= block.first_insn) {
                continue;
            }
            JitIrInstruction& insn = ir.instructions[idx];
            if (insn.opcode == JitIrOpcode::LoadConstInt) {
                hoisted.push_back(insn);
                insn.opcode = JitIrOpcode::Nop;
                ++stats.licm_moves;
                continue;
            }
            if (insn.opcode != JitIrOpcode::LoadLocal || stored_locals.contains(insn.b)) {
                continue;
            }
            const auto it = invariant_local_reg.find(insn.b);
            if (it == invariant_local_reg.end()) {
                invariant_local_reg.emplace(insn.b, insn.a);
                hoisted.push_back(insn);
                insn.opcode = JitIrOpcode::Nop;
                ++stats.licm_moves;
            } else if (it->second != insn.a) {
                insn.opcode = JitIrOpcode::MoveReg;
                insn.b = it->second;
                insn.c = 0;
                ++stats.licm_moves;
            } else {
                insn.opcode = JitIrOpcode::Nop;
                ++stats.licm_moves;
            }
        }
        if (!hoisted.empty()) {
            const std::uint32_t insert_pos = block.first_insn;
            const std::uint32_t delta = static_cast<std::uint32_t>(hoisted.size());
            ir.instructions.insert(ir.instructions.begin() + static_cast<std::ptrdiff_t>(insert_pos), hoisted.begin(),
                                   hoisted.end());
            remap_jump_targets_after_insert(ir, insert_pos, delta);
        }
    }
    OptimizingPassStats cleanup;
    remove_nops(ir, cleanup);
    stats.dead_removed += cleanup.dead_removed;
    build_cfg_for_function(ir);
}

std::uint32_t function_insn_count(const Bytecode& bytecode, std::uint32_t function_index) {
    std::uint32_t end = static_cast<std::uint32_t>(bytecode.instructions.size());
    if (function_index + 1 < bytecode.functions.size()) {
        end = bytecode.functions[function_index + 1].entry;
    }
    const std::uint32_t start = bytecode.functions[function_index].entry;
    return end > start ? end - start : 0;
}

bool ir_has_container_index_ops(const JitFunctionIr& ir) {
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::GetIndex || insn.opcode == JitIrOpcode::SetIndex) {
            return true;
        }
    }
    return false;
}

void infer_container_value_kinds(const JitFunctionIr& ir, std::vector<std::optional<ValueKind>>& reg_kind,
                                 std::vector<std::optional<ValueKind>>& local_kind) {
    reg_kind.assign(ir.register_count, std::nullopt);
    local_kind.assign(ir.local_count, std::nullopt);
    for (const JitIrInstruction& insn : ir.instructions) {
        switch (insn.opcode) {
            case JitIrOpcode::MakeList:
                if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] = ValueKind::List;
                }
                break;
            case JitIrOpcode::MakeTuple:
                if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] = ValueKind::Tuple;
                }
                break;
            case JitIrOpcode::MakeDict:
                if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] = ValueKind::Dict;
                }
                break;
            case JitIrOpcode::MakeSet:
                if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] = ValueKind::Set;
                }
                break;
            case JitIrOpcode::LoadLocal:
                if (insn.a < reg_kind.size() && insn.b < local_kind.size() && local_kind[insn.b]) {
                    reg_kind[insn.a] = local_kind[insn.b];
                } else if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] = std::nullopt;
                }
                break;
            case JitIrOpcode::StoreLocal:
                if (insn.a < local_kind.size() && insn.b < reg_kind.size() && reg_kind[insn.b]) {
                    local_kind[insn.a] = reg_kind[insn.b];
                } else if (insn.a < local_kind.size()) {
                    local_kind[insn.a] = std::nullopt;
                }
                break;
            case JitIrOpcode::MoveReg:
                if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] =
                        insn.b < reg_kind.size() ? reg_kind[insn.b] : std::optional<ValueKind>{std::nullopt};
                }
                break;
            case JitIrOpcode::GetIndex:
            case JitIrOpcode::ListGetIndex:
            case JitIrOpcode::TupleGetIndex:
            case JitIrOpcode::BytesGetIndex:
            case JitIrOpcode::DictGetStringKey:
                if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] = std::nullopt;
                }
                break;
            case JitIrOpcode::Call:
            case JitIrOpcode::CallValue:
                if (insn.a < reg_kind.size()) {
                    reg_kind[insn.a] = std::nullopt;
                }
                break;
            default:
                break;
        }
    }
}

void specialize_memory_access(JitFunctionIr& ir, OptimizingPassStats& stats) {
    const bool container_alloc = ir_allocates_containers(ir);
    std::vector<std::optional<ValueKind>> reg_kind;
    std::vector<std::optional<ValueKind>> local_kind;
    infer_container_value_kinds(ir, reg_kind, local_kind);

    std::vector<JitIrInstruction> rewritten;
    rewritten.reserve(ir.instructions.size() + 16);
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::GetField) {
            JitIrInstruction guard;
            guard.opcode = JitIrOpcode::GuardFieldObject;
            guard.a = insn.b;
            guard.bytecode_ip = insn.bytecode_ip;
            guard.block_id = insn.block_id;
            rewritten.push_back(guard);
            rewritten.push_back(insn);
            ++stats.specialized_ops;
            continue;
        }
        if (insn.opcode == JitIrOpcode::GetIndex) {
            const std::optional<ValueKind> object_kind =
                insn.b < reg_kind.size() ? reg_kind[insn.b] : std::optional<ValueKind>{std::nullopt};
            const auto emit_guarded = [&](ValueKind kind, JitIrOpcode specialized) {
                JitIrInstruction guard;
                guard.opcode = JitIrOpcode::GuardValueKind;
                guard.a = insn.b;
                guard.guard_value_kind = static_cast<std::uint32_t>(kind);
                guard.bytecode_ip = insn.bytecode_ip;
                guard.block_id = insn.block_id;
                rewritten.push_back(guard);
                JitIrInstruction specialized_insn = insn;
                specialized_insn.opcode = specialized;
                rewritten.push_back(specialized_insn);
                ++stats.specialized_ops;
            };
            if (object_kind == ValueKind::List) {
                if (container_alloc) {
                    rewritten.push_back(insn);
                } else {
                    emit_guarded(ValueKind::List, JitIrOpcode::ListGetIndex);
                }
                continue;
            }
            if (object_kind == ValueKind::Tuple) {
                rewritten.push_back(insn);
                continue;
            }
            if (object_kind == ValueKind::Bytes) {
                emit_guarded(ValueKind::Bytes, JitIrOpcode::BytesGetIndex);
                continue;
            }
            if (object_kind == ValueKind::Dict) {
                JitIrInstruction key_guard;
                key_guard.opcode = JitIrOpcode::GuardValueKind;
                key_guard.a = insn.c;
                key_guard.guard_value_kind = static_cast<std::uint32_t>(ValueKind::String);
                key_guard.bytecode_ip = insn.bytecode_ip;
                key_guard.block_id = insn.block_id;
                rewritten.push_back(key_guard);
                emit_guarded(ValueKind::Dict, JitIrOpcode::DictGetStringKey);
                continue;
            }
            rewritten.push_back(insn);
            continue;
        }
        if (insn.opcode == JitIrOpcode::SetIndex) {
            const std::optional<ValueKind> object_kind =
                insn.a < reg_kind.size() ? reg_kind[insn.a] : std::optional<ValueKind>{std::nullopt};
            if (object_kind == ValueKind::List) {
                if (container_alloc) {
                    rewritten.push_back(insn);
                } else {
                    JitIrInstruction guard;
                    guard.opcode = JitIrOpcode::GuardValueKind;
                    guard.a = insn.a;
                    guard.guard_value_kind = static_cast<std::uint32_t>(ValueKind::List);
                    guard.bytecode_ip = insn.bytecode_ip;
                    guard.block_id = insn.block_id;
                    rewritten.push_back(guard);
                    JitIrInstruction list_insn = insn;
                    list_insn.opcode = JitIrOpcode::ListSetIndex;
                    rewritten.push_back(list_insn);
                    ++stats.specialized_ops;
                }
                continue;
            }
        }
        rewritten.push_back(insn);
    }
    ir.instructions = std::move(rewritten);
}

void specialize_list_integer_element_loads(JitFunctionIr& ir, OptimizingPassStats& stats) {
    if (!ir.buffered_integer_locals) {
        return;
    }
    std::vector<std::uint32_t> use_count(ir.register_count, 0);
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::IntAdd || insn.opcode == JitIrOpcode::IntSub) {
            if (insn.b < use_count.size()) {
                ++use_count[insn.b];
            }
            if (insn.c < use_count.size()) {
                ++use_count[insn.c];
            }
        }
    }
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::ListGetIndex) {
            continue;
        }
        if (insn.a >= use_count.size() || use_count[insn.a] != 1) {
            continue;
        }
        insn.opcode = JitIrOpcode::ListGetIntElement;
        ++stats.specialized_ops;
    }
}

}  // namespace

void buffer_integer_locals(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    buffer_integer_locals_impl(bytecode, ir, stats);
    if (ir.buffered_integer_locals) {
        OptimizingPassStats respec_stats;
        specialize_integer_ops(bytecode, ir, nullptr, respec_stats);
        build_cfg_for_function(ir);
    }
}

void clear_stable_function_reg(std::vector<std::optional<std::uint32_t>>& stable_fn, std::uint32_t reg) {
    if (reg < stable_fn.size()) {
        stable_fn[reg] = std::nullopt;
    }
}

void clear_dest_register(std::vector<std::optional<std::uint32_t>>& stable_fn, const JitIrInstruction& insn) {
    switch (insn.opcode) {
        case JitIrOpcode::LoadConst:
        case JitIrOpcode::LoadConstInt:
        case JitIrOpcode::LoadLocal:
        case JitIrOpcode::MoveReg:
        case JitIrOpcode::Add:
        case JitIrOpcode::Sub:
        case JitIrOpcode::Mul:
        case JitIrOpcode::Div:
        case JitIrOpcode::Mod:
        case JitIrOpcode::UnaryNot:
        case JitIrOpcode::Equal:
        case JitIrOpcode::NotEqual:
        case JitIrOpcode::Less:
        case JitIrOpcode::LessEqual:
        case JitIrOpcode::Greater:
        case JitIrOpcode::GreaterEqual:
        case JitIrOpcode::IntAdd:
        case JitIrOpcode::IntSub:
        case JitIrOpcode::IntMul:
        case JitIrOpcode::IntMod:
        case JitIrOpcode::IntDiv:
        case JitIrOpcode::IntLess:
        case JitIrOpcode::IntEqual:
        case JitIrOpcode::StringEqual:
        case JitIrOpcode::StringNotEqual:
        case JitIrOpcode::StringConcat:
        case JitIrOpcode::FloatAdd:
        case JitIrOpcode::FloatSub:
        case JitIrOpcode::FloatMul:
        case JitIrOpcode::FloatDiv:
        case JitIrOpcode::FloatLess:
        case JitIrOpcode::FloatEqual:
        case JitIrOpcode::GetIndex:
        case JitIrOpcode::ListGetIndex:
        case JitIrOpcode::TupleGetIndex:
        case JitIrOpcode::BytesGetIndex:
        case JitIrOpcode::DictGetStringKey:
        case JitIrOpcode::ListGetIntElement:
        case JitIrOpcode::MakeList:
        case JitIrOpcode::MakeDict:
        case JitIrOpcode::MakeTuple:
        case JitIrOpcode::MakeSet:
        case JitIrOpcode::GetField:
        case JitIrOpcode::MakeStruct:
        case JitIrOpcode::Call:
        case JitIrOpcode::CallValue:
        case JitIrOpcode::GetIter:
        case JitIrOpcode::IterNext:
            clear_stable_function_reg(stable_fn, insn.a);
            break;
        default:
            break;
    }
}

void specialize_stable_call_value(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats) {
    std::vector<std::optional<std::uint32_t>> stable_fn(ir.register_count);
    std::vector<bool> reg_holds_closure(ir.register_count, false);
    std::vector<std::optional<std::uint32_t>> stable_local(ir.local_count);
    std::vector<bool> local_holds_closure(ir.local_count, false);
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::CallValue && insn.b < stable_fn.size() && stable_fn[insn.b] &&
            insn.b < reg_holds_closure.size() && !reg_holds_closure[insn.b]) {
            const std::uint32_t fn_index = *stable_fn[insn.b];
            if (fn_index < bytecode.functions.size() && bytecode.functions[fn_index].captures.empty()) {
                insn.opcode = JitIrOpcode::Call;
                insn.b = insn.b + 1;
                insn.c = fn_index;
                ++stats.specialized_ops;
                clear_stable_function_reg(stable_fn, insn.a);
                if (insn.a < reg_holds_closure.size()) {
                    reg_holds_closure[insn.a] = false;
                }
                continue;
            }
        }
        if (insn.opcode == JitIrOpcode::Call && insn.a < stable_fn.size()) {
            std::uint32_t inner = 0;
            if (bytecode_function_returns_single_closure(bytecode, insn.c, inner)) {
                stable_fn[insn.a] = inner;
                if (insn.a < reg_holds_closure.size()) {
                    reg_holds_closure[insn.a] = true;
                }
                continue;
            }
        }
        if (insn.opcode == JitIrOpcode::MakeClosure && insn.a < stable_fn.size()) {
            stable_fn[insn.a] = insn.b;
            if (insn.a < reg_holds_closure.size()) {
                reg_holds_closure[insn.a] = true;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadFunction && insn.a < stable_fn.size()) {
            stable_fn[insn.a] = insn.b;
            if (insn.a < reg_holds_closure.size()) {
                reg_holds_closure[insn.a] = false;
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::MoveReg && insn.a < stable_fn.size() && insn.b < stable_fn.size() &&
            stable_fn[insn.b]) {
            stable_fn[insn.a] = stable_fn[insn.b];
            if (insn.a < reg_holds_closure.size() && insn.b < reg_holds_closure.size()) {
                reg_holds_closure[insn.a] = reg_holds_closure[insn.b];
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal && insn.a < stable_local.size() && insn.b < stable_fn.size() &&
            stable_fn[insn.b]) {
            stable_local[insn.a] = stable_fn[insn.b];
            if (insn.a < local_holds_closure.size() && insn.b < reg_holds_closure.size()) {
                local_holds_closure[insn.a] = reg_holds_closure[insn.b];
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal && insn.b < stable_local.size() && stable_local[insn.b] &&
            insn.a < stable_fn.size()) {
            stable_fn[insn.a] = stable_local[insn.b];
            if (insn.a < reg_holds_closure.size() && insn.b < local_holds_closure.size()) {
                reg_holds_closure[insn.a] = local_holds_closure[insn.b];
            }
            continue;
        }
        clear_dest_register(stable_fn, insn);
        if (insn.a < reg_holds_closure.size()) {
            reg_holds_closure[insn.a] = false;
        }
    }
}

bool ir_has_call_sites(const JitFunctionIr& ir) {
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::Call || insn.opcode == JitIrOpcode::CallValue) {
            return true;
        }
    }
    return false;
}

bool ir_has_call_value_sites(const JitFunctionIr& ir) {
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::CallValue) {
            return true;
        }
    }
    return false;
}

namespace {

bool callee_ir_is_inline_leaf(const JitFunctionIr& callee_ir) {
    for (const JitIrInstruction& insn : callee_ir.instructions) {
        switch (insn.opcode) {
            case JitIrOpcode::MakeClosure:
            case JitIrOpcode::MaterializeClosureIntCapture:
            case JitIrOpcode::MaterializeClosureTwoIntCaptures:
                return false;
            case JitIrOpcode::Call:
            case JitIrOpcode::CallValue:
            case JitIrOpcode::Jump:
            case JitIrOpcode::JumpIfFalse:
            case JitIrOpcode::IterNext:
            case JitIrOpcode::GetIter:
                return false;
            default:
                break;
        }
    }
    return true;
}

bool callee_ir_mutates_upvalues(const JitFunctionIr& callee_ir) {
    for (const JitIrInstruction& insn : callee_ir.instructions) {
        if (insn.opcode == JitIrOpcode::StoreUpvalue) {
            return true;
        }
    }
    return false;
}

bool callee_creates_local_bound_closure(const Bytecode& bytecode, const JitFunctionIr& callee_ir) {
    for (const JitIrInstruction& insn : callee_ir.instructions) {
        if (insn.opcode != JitIrOpcode::MakeClosure || insn.b >= bytecode.functions.size()) {
            continue;
        }
        const FunctionInfo& closure_fn = bytecode.functions[insn.b];
        for (const Capture& capture : closure_fn.captures) {
            if (capture.from_local) {
                return true;
            }
        }
    }
    return false;
}

struct InlineCostLimits {
    std::uint32_t max_callee_insn = 24;
    std::size_t max_inline_sites = 8;
    std::size_t max_caller_growth_insn = 192;
    std::uint32_t cold_callee_max_cost = 10;
    std::uint32_t min_benefit_score = 55;
};

std::uint32_t estimate_callee_inline_cost(const JitFunctionIr& callee_ir) {
    std::uint32_t cost = 0;
    for (const JitIrInstruction& insn : callee_ir.instructions) {
        if (insn.opcode == JitIrOpcode::Nop || insn.opcode == JitIrOpcode::StoreLocal) {
            continue;
        }
        ++cost;
        switch (insn.opcode) {
            case JitIrOpcode::GuardInteger:
            case JitIrOpcode::GuardValueKind:
            case JitIrOpcode::GuardFieldObject:
            case JitIrOpcode::MakeStruct:
            case JitIrOpcode::SetField:
            case JitIrOpcode::GetField:
            case JitIrOpcode::GetIndex:
            case JitIrOpcode::ListGetIndex:
                cost += 1;
                break;
            default:
                break;
        }
    }
    return cost;
}

bool callee_calls_function(const JitFunctionIr& callee_ir, std::uint32_t target_index) {
    for (const JitIrInstruction& insn : callee_ir.instructions) {
        if (insn.opcode == JitIrOpcode::Call && insn.c == target_index) {
            return true;
        }
    }
    return false;
}

bool caller_qualifies_for_inlining(const JitOptProfile* profile, const FunctionOptHints* caller_hints) {
    if (profile && profile->usable) {
        if (!caller_hints) {
            return false;
        }
        if (caller_hints->very_hot) {
            return true;
        }
        if (caller_hints->hot_loop_iterations >= 500) {
            return true;
        }
        if (caller_hints->call_count >= 100) {
            return true;
        }
        return false;
    }
    return true;
}

bool call_site_is_worth_inlining(const FunctionOptHints* caller_hints, const FunctionOptHints* callee_hints,
                                 bool in_loop, std::uint32_t callee_cost, const InlineCostLimits& limits) {
    if (in_loop && callee_cost <= limits.max_callee_insn) {
        return true;
    }
    if (caller_hints && caller_hints->very_hot && callee_cost <= limits.max_callee_insn) {
        return true;
    }
    if (callee_hints && callee_hints->call_count >= 50 && callee_cost <= limits.max_callee_insn) {
        return true;
    }
    if (callee_cost <= limits.cold_callee_max_cost && caller_hints && caller_hints->hot_loop_iterations > 0) {
        return true;
    }
    return false;
}

std::uint32_t inline_benefit_score(bool in_loop, const FunctionOptHints* caller_hints, std::uint32_t callee_cost) {
    std::uint32_t score = 80;
    if (in_loop) {
        score += 35;
    }
    if (caller_hints && caller_hints->very_hot) {
        score += 25;
    }
    if (caller_hints && caller_hints->hot_loop_iterations >= 5000) {
        score += 15;
    }
    if (callee_cost <= 6) {
        score += 25;
    } else if (callee_cost <= 12) {
        score += 12;
    }
    if (callee_cost > 20) {
        score -= 20;
    }
    score = score > callee_cost ? score - callee_cost : 0;
    return score;
}

bool should_inline_call_site(const Bytecode& bytecode, const JitFunctionIr& ir, const JitOptProfile* profile,
                             std::size_t call_insn_index, const JitIrInstruction& call_insn,
                             std::uint32_t callee_index, const JitFunctionIr& callee_ir,
                             std::size_t accumulated_growth, const InlineCostLimits& limits) {
    if (call_insn.opcode != JitIrOpcode::Call) {
        return false;
    }
    if (callee_index >= bytecode.functions.size() || callee_index == ir.function_index) {
        return false;
    }
    if (!callee_ir_is_inline_leaf(callee_ir)) {
        return false;
    }
    if (callee_creates_local_bound_closure(bytecode, callee_ir)) {
        return false;
    }
    if (callee_calls_function(callee_ir, ir.function_index)) {
        return false;
    }
    const FunctionInfo& callee_fn = bytecode.functions[callee_index];
    if (!callee_fn.captures.empty() || callee_fn.is_async || callee_fn.is_native) {
        return false;
    }
    std::string reason;
    if (!jit_function_is_eligible(bytecode, callee_index, reason)) {
        return false;
    }
    const std::uint32_t callee_cost = estimate_callee_inline_cost(callee_ir);
    if (callee_cost == 0 || callee_cost > limits.max_callee_insn) {
        return false;
    }
    if (accumulated_growth + callee_cost > limits.max_caller_growth_insn) {
        return false;
    }
    const FunctionOptHints* caller_hints = profile ? profile->function_hints(ir.function_index) : nullptr;
    const FunctionOptHints* callee_hints = profile ? profile->function_hints(callee_index) : nullptr;
    if (!caller_qualifies_for_inlining(profile, caller_hints)) {
        return false;
    }
    const std::unordered_set<std::uint32_t> loop_blocks = loop_body_block_ids(ir);
    bool in_loop = loop_blocks.contains(call_insn.block_id);
    if (!in_loop) {
        in_loop = instruction_in_backedge_loop(ir, call_insn_index);
    }
    if (!call_site_is_worth_inlining(caller_hints, callee_hints, in_loop, callee_cost, limits)) {
        return false;
    }
    const std::uint32_t score = inline_benefit_score(in_loop, caller_hints, callee_cost);
    return score >= limits.min_benefit_score;
}

void remap_register_operands(JitIrInstruction& insn, std::uint32_t reg_base) {
    switch (insn.opcode) {
        case JitIrOpcode::LoadConst:
        case JitIrOpcode::LoadConstInt:
            insn.a += reg_base;
            break;
        case JitIrOpcode::MoveReg:
            insn.a += reg_base;
            insn.b += reg_base;
            break;
        case JitIrOpcode::UnaryNot:
        case JitIrOpcode::GuardInteger:
        case JitIrOpcode::GuardValueKind:
        case JitIrOpcode::GuardFieldObject:
            insn.a += reg_base;
            break;
        case JitIrOpcode::Add:
        case JitIrOpcode::Sub:
        case JitIrOpcode::Mul:
        case JitIrOpcode::Div:
        case JitIrOpcode::Mod:
        case JitIrOpcode::Equal:
        case JitIrOpcode::NotEqual:
        case JitIrOpcode::Less:
        case JitIrOpcode::LessEqual:
        case JitIrOpcode::Greater:
        case JitIrOpcode::GreaterEqual:
        case JitIrOpcode::IntAdd:
        case JitIrOpcode::IntSub:
        case JitIrOpcode::IntMul:
        case JitIrOpcode::IntMod:
        case JitIrOpcode::IntDiv:
        case JitIrOpcode::IntLess:
        case JitIrOpcode::IntEqual:
        case JitIrOpcode::StringEqual:
        case JitIrOpcode::StringNotEqual:
        case JitIrOpcode::StringConcat:
        case JitIrOpcode::FloatAdd:
        case JitIrOpcode::FloatSub:
        case JitIrOpcode::FloatMul:
        case JitIrOpcode::FloatDiv:
        case JitIrOpcode::FloatLess:
        case JitIrOpcode::FloatEqual:
        case JitIrOpcode::GetIndex:
        case JitIrOpcode::ListGetIndex:
        case JitIrOpcode::TupleGetIndex:
        case JitIrOpcode::BytesGetIndex:
        case JitIrOpcode::DictGetStringKey:
        case JitIrOpcode::ListGetIntElement:
            insn.a += reg_base;
            insn.b += reg_base;
            insn.c += reg_base;
            break;
        case JitIrOpcode::SetIndex:
        case JitIrOpcode::ListSetIndex:
            insn.a += reg_base;
            insn.b += reg_base;
            insn.c += reg_base;
            break;
        case JitIrOpcode::MakeList:
        case JitIrOpcode::MakeDict:
        case JitIrOpcode::MakeTuple:
        case JitIrOpcode::MakeSet:
            insn.a += reg_base;
            insn.b += reg_base;
            break;
        case JitIrOpcode::GetField:
            insn.a += reg_base;
            insn.b += reg_base;
            break;
        case JitIrOpcode::SetField:
            insn.a += reg_base;
            insn.c += reg_base;
            break;
        case JitIrOpcode::MakeStruct:
            insn.a += reg_base;
            break;
        case JitIrOpcode::LoadUpvalue:
            insn.a += reg_base;
            break;
        case JitIrOpcode::StoreUpvalue:
            insn.b += reg_base;
            break;
        case JitIrOpcode::MakeClosure:
            insn.a += reg_base;
            break;
        case JitIrOpcode::Return:
            insn.a += reg_base;
            break;
        default:
            break;
    }
}

std::vector<JitIrInstruction> remap_callee_for_callvalue_inline(
    const JitFunctionIr& callee_ir, std::uint32_t reg_base, std::uint32_t dest_reg, std::uint32_t first_arg_reg,
    std::uint32_t closure_reg, std::uint32_t bytecode_ip, std::uint32_t block_id,
    const std::vector<std::optional<std::int64_t>>& capture_consts) {
    std::vector<JitIrInstruction> out;
    out.reserve(callee_ir.instructions.size());
    for (const JitIrInstruction& insn : callee_ir.instructions) {
        if (insn.opcode == JitIrOpcode::Return) {
            JitIrInstruction move;
            move.opcode = JitIrOpcode::MoveReg;
            move.a = dest_reg;
            move.b = insn.a + reg_base;
            move.bytecode_ip = bytecode_ip;
            move.block_id = block_id;
            out.push_back(move);
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal) {
            JitIrInstruction move;
            move.opcode = JitIrOpcode::MoveReg;
            move.a = insn.a + reg_base;
            move.b = first_arg_reg + insn.b;
            move.bytecode_ip = bytecode_ip;
            move.block_id = block_id;
            out.push_back(move);
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadUpvalue) {
            if (insn.b < capture_consts.size() && capture_consts[insn.b] &&
                !callee_ir_mutates_upvalues(callee_ir)) {
                JitIrInstruction load;
                load.opcode = JitIrOpcode::LoadConstInt;
                load.a = insn.a + reg_base;
                load.const_int_value = *capture_consts[insn.b];
                load.bytecode_ip = bytecode_ip;
                load.block_id = block_id;
                out.push_back(load);
            } else {
                JitIrInstruction cap;
                cap.opcode = JitIrOpcode::LoadClosureCapture;
                cap.a = insn.a + reg_base;
                cap.b = closure_reg;
                cap.c = insn.b;
                cap.bytecode_ip = bytecode_ip;
                cap.block_id = block_id;
                out.push_back(cap);
            }
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal) {
            continue;
        }
        JitIrInstruction mapped = insn;
        mapped.bytecode_ip = bytecode_ip;
        mapped.block_id = block_id;
        remap_register_operands(mapped, reg_base);
        out.push_back(mapped);
    }
    return out;
}

std::vector<JitIrInstruction> remap_callee_for_inline(const JitFunctionIr& callee_ir, std::uint32_t reg_base,
                                                      std::uint32_t dest_reg, std::uint32_t first_arg_reg,
                                                      std::uint32_t bytecode_ip, std::uint32_t block_id) {
    std::vector<JitIrInstruction> out;
    out.reserve(callee_ir.instructions.size());
    for (const JitIrInstruction& insn : callee_ir.instructions) {
        if (insn.opcode == JitIrOpcode::Return) {
            JitIrInstruction move;
            move.opcode = JitIrOpcode::MoveReg;
            move.a = dest_reg;
            move.b = insn.a + reg_base;
            move.bytecode_ip = bytecode_ip;
            move.block_id = block_id;
            out.push_back(move);
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal) {
            JitIrInstruction move;
            move.opcode = JitIrOpcode::MoveReg;
            move.a = insn.a + reg_base;
            move.b = first_arg_reg + insn.b;
            move.bytecode_ip = bytecode_ip;
            move.block_id = block_id;
            out.push_back(move);
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal) {
            continue;
        }
        JitIrInstruction mapped = insn;
        mapped.bytecode_ip = bytecode_ip;
        mapped.block_id = block_id;
        remap_register_operands(mapped, reg_base);
        out.push_back(mapped);
    }
    return out;
}

bool inline_hot_callees_pass(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                             std::uint32_t max_inline_insn, OptimizingPassStats& stats) {
    InlineCostLimits limits;
    limits.max_callee_insn = max_inline_insn;
    const FunctionOptHints* self_hints = profile ? profile->function_hints(ir.function_index) : nullptr;
    if (!caller_qualifies_for_inlining(profile, self_hints)) {
        return false;
    }
    std::size_t inline_sites = 0;
    std::size_t pass_inlined = 0;
    std::size_t accumulated_growth = 0;
    std::vector<JitIrInstruction> rewritten;
    std::vector<std::uint32_t> old_to_new(ir.instructions.size(), 0xFFFFFFFFu);
    rewritten.reserve(ir.instructions.size() + max_inline_insn * 4);
    for (std::size_t old_idx = 0; old_idx < ir.instructions.size(); ++old_idx) {
        const JitIrInstruction& insn = ir.instructions[old_idx];
        if (insn.opcode != JitIrOpcode::Call || inline_sites >= limits.max_inline_sites) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        if (instruction_in_backedge_loop(ir, old_idx)) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        const std::uint32_t callee = insn.c;
        JitFunctionIr callee_ir;
        std::string error;
        if (!build_jit_ir_from_bytecode(bytecode, callee, callee_ir, error) || callee_ir.instructions.empty() ||
            !should_inline_call_site(bytecode, ir, profile, old_idx, insn, callee, callee_ir, accumulated_growth,
                                    limits)) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        const FunctionInfo& callee_fn = bytecode.functions[callee];
        const std::uint32_t callee_cost = estimate_callee_inline_cost(callee_ir);
        const std::uint32_t reg_base = ir.register_count;
        if (insn.a >= reg_base) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        const std::uint32_t slab = std::max({callee_fn.register_count, callee_ir.register_count,
                                             max_register_operand(callee_ir) + 1});
        ir.register_count += slab;
        old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
        const std::vector<JitIrInstruction> inlined =
            remap_callee_for_inline(callee_ir, reg_base, insn.a, insn.b, insn.bytecode_ip, insn.block_id);
        rewritten.insert(rewritten.end(), inlined.begin(), inlined.end());
        accumulated_growth += callee_cost;
        ++stats.inlined_calls;
        ++pass_inlined;
        ++inline_sites;
    }
    if (pass_inlined == 0) {
        return false;
    }
    for (JitIrInstruction& insn : rewritten) {
        if (insn.opcode != JitIrOpcode::Jump && insn.opcode != JitIrOpcode::JumpIfFalse &&
            insn.opcode != JitIrOpcode::IterNext) {
            continue;
        }
        if (insn.jump_target_insn < old_to_new.size() && old_to_new[insn.jump_target_insn] != 0xFFFFFFFFu) {
            insn.jump_target_insn = old_to_new[insn.jump_target_insn];
        }
    }
    ir.instructions = std::move(rewritten);
    recompute_register_count(ir);
    build_cfg_for_function(ir);
    return true;
}

void inline_hot_callees(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                        std::uint32_t max_inline_insn, OptimizingPassStats& stats) {
    for (int pass = 0; pass < 6; ++pass) {
        if (!inline_hot_callees_pass(bytecode, ir, profile, max_inline_insn, stats)) {
            break;
        }
    }
}

std::optional<std::int64_t> integer_constant_value(const Bytecode& bytecode, std::uint32_t const_index) {
    if (const_index >= bytecode.constants.size()) {
        return std::nullopt;
    }
    const Constant& constant = bytecode.constants[const_index];
    if (constant.kind != ConstantKind::Integer) {
        return std::nullopt;
    }
    try {
        std::size_t idx = 0;
        const long long value = std::stoll(constant.lexeme, &idx, 10);
        if (idx != constant.lexeme.size()) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

bool inline_stable_call_value_pass(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                                   std::uint32_t max_inline_insn, OptimizingPassStats& stats) {
    InlineCostLimits limits;
    limits.max_callee_insn = max_inline_insn;
    const FunctionOptHints* self_hints = profile ? profile->function_hints(ir.function_index) : nullptr;
    const bool has_loop_headers =
        std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& block) { return block.loop_header; });
    if (!has_loop_headers && !caller_qualifies_for_inlining(profile, self_hints)) {
        return false;
    }
    const FunctionInfo& fn = bytecode.functions[ir.function_index];
    std::vector<std::optional<std::uint32_t>> stable_fn(ir.register_count);
    std::vector<std::optional<std::uint32_t>> stable_local(fn.local_count);
    std::vector<std::optional<std::int64_t>> reg_const(ir.register_count);
    using CaptureConstVec = std::vector<std::optional<std::int64_t>>;
    std::unordered_map<std::uint32_t, CaptureConstVec> closure_capture_const;
    std::unordered_map<std::uint32_t, CaptureConstVec> closure_capture_by_reg;
    std::vector<std::optional<CaptureConstVec>> closure_capture_by_local(fn.local_count);
    const auto copy_closure_captures_reg = [&](std::uint32_t dst, std::uint32_t src) {
        const auto it = closure_capture_by_reg.find(src);
        if (it != closure_capture_by_reg.end()) {
            closure_capture_by_reg[dst] = it->second;
        } else {
            closure_capture_by_reg.erase(dst);
        }
    };
    for (int pass = 0; pass < 12; ++pass) {
        bool changed = false;
        for (const JitIrInstruction& insn : ir.instructions) {
            if (insn.opcode == JitIrOpcode::LoadConstInt && insn.a < reg_const.size() &&
                reg_const[insn.a] != insn.const_int_value) {
                reg_const[insn.a] = insn.const_int_value;
                changed = true;
            } else if (insn.opcode == JitIrOpcode::LoadConst && insn.a < reg_const.size()) {
                if (const std::optional<std::int64_t> value = integer_constant_value(bytecode, insn.b)) {
                    if (reg_const[insn.a] != value) {
                        reg_const[insn.a] = value;
                        changed = true;
                    }
                }
            } else if (insn.opcode == JitIrOpcode::MoveReg) {
                if (insn.a < reg_const.size() && insn.b < reg_const.size() && reg_const[insn.b] &&
                    reg_const[insn.a] != reg_const[insn.b]) {
                    reg_const[insn.a] = reg_const[insn.b];
                    changed = true;
                }
                if (insn.a < stable_fn.size() && insn.b < stable_fn.size() && stable_fn[insn.b] &&
                    stable_fn[insn.a] != stable_fn[insn.b]) {
                    stable_fn[insn.a] = stable_fn[insn.b];
                    changed = true;
                }
                if (insn.a < reg_const.size() && insn.b < reg_const.size()) {
                    copy_closure_captures_reg(insn.a, insn.b);
                    if (reg_const[insn.b] && reg_const[insn.a] != reg_const[insn.b]) {
                        reg_const[insn.a] = reg_const[insn.b];
                        changed = true;
                    }
                }
            } else if (insn.opcode == JitIrOpcode::Call && insn.a < stable_fn.size()) {
                std::uint32_t inner = 0;
                if (bytecode_function_returns_single_closure(bytecode, insn.c, inner) && stable_fn[insn.a] != inner) {
                    stable_fn[insn.a] = inner;
                    if (insn.b < reg_const.size() && reg_const[insn.b] && inner < bytecode.functions.size() &&
                        bytecode.functions[inner].captures.size() == 1) {
                        closure_capture_const[inner] = {*reg_const[insn.b]};
                        closure_capture_by_reg[insn.a] = {*reg_const[insn.b]};
                    }
                    changed = true;
                }
            } else if (insn.opcode == JitIrOpcode::CallValue && insn.a < stable_fn.size() &&
                       insn.b < stable_fn.size() && stable_fn[insn.b]) {
                std::uint32_t inner = 0;
                const std::uint32_t factory = *stable_fn[insn.b];
                if (bytecode_function_returns_single_closure(bytecode, factory, inner) &&
                    inner < bytecode.functions.size() && stable_fn[insn.a] != inner) {
                    stable_fn[insn.a] = inner;
                    const FunctionInfo& inner_fn = bytecode.functions[inner];
                    std::vector<std::optional<std::int64_t>> caps(inner_fn.captures.size());
                    bool all = !inner_fn.captures.empty();
                    const auto parent_caps = closure_capture_const.find(factory);
                    for (std::size_t ci = 0; ci < inner_fn.captures.size(); ++ci) {
                        const Capture& cap = inner_fn.captures[ci];
                        if (cap.from_local) {
                            const std::uint32_t arg_reg = insn.b + 1 + cap.index;
                            if (cap.index < insn.c && arg_reg < reg_const.size() && reg_const[arg_reg]) {
                                caps[ci] = *reg_const[arg_reg];
                            } else {
                                all = false;
                            }
                        } else if (parent_caps != closure_capture_const.end() &&
                                   cap.index < parent_caps->second.size() && parent_caps->second[cap.index]) {
                            caps[ci] = *parent_caps->second[cap.index];
                        } else {
                            all = false;
                        }
                    }
                    if (all) {
                        closure_capture_const[inner] = caps;
                        closure_capture_by_reg[insn.a] = caps;
                    }
                    changed = true;
                }
            } else if (insn.opcode == JitIrOpcode::MaterializeClosureIntCapture && insn.a < stable_fn.size()) {
                stable_fn[insn.a] = insn.b;
                if (insn.b < bytecode.functions.size()) {
                    closure_capture_const[insn.b] = {insn.const_int_value};
                    closure_capture_by_reg[insn.a] = {insn.const_int_value};
                }
                changed = true;
            } else if (insn.opcode == JitIrOpcode::MaterializeClosureTwoIntCaptures && insn.a < stable_fn.size()) {
                stable_fn[insn.a] = insn.b;
                if (insn.b < bytecode.functions.size()) {
                    const CaptureConstVec caps = {insn.const_int_value,
                                                  static_cast<std::int64_t>(static_cast<std::int32_t>(insn.c))};
                    closure_capture_const[insn.b] = caps;
                    closure_capture_by_reg[insn.a] = caps;
                }
                changed = true;
            } else if (insn.opcode == JitIrOpcode::MakeClosure && insn.a < stable_fn.size() &&
                       stable_fn[insn.a] != insn.b) {
                stable_fn[insn.a] = insn.b;
                changed = true;
            } else if (insn.opcode == JitIrOpcode::LoadFunction && insn.a < stable_fn.size() &&
                       stable_fn[insn.a] != insn.b) {
                stable_fn[insn.a] = insn.b;
                changed = true;
            } else if (insn.opcode == JitIrOpcode::StoreLocal && insn.a < stable_local.size() && insn.b < stable_fn.size() &&
                       stable_fn[insn.b] && stable_local[insn.a] != stable_fn[insn.b]) {
                stable_local[insn.a] = stable_fn[insn.b];
                if (insn.a < closure_capture_by_local.size()) {
                    const auto cap_it = closure_capture_by_reg.find(insn.b);
                    if (cap_it != closure_capture_by_reg.end()) {
                        closure_capture_by_local[insn.a] = cap_it->second;
                    } else {
                        closure_capture_by_local[insn.a] = std::nullopt;
                    }
                }
                changed = true;
            } else if (insn.opcode == JitIrOpcode::LoadLocal && insn.b < stable_local.size() && stable_local[insn.b] &&
                       insn.a < stable_fn.size() && stable_fn[insn.a] != stable_local[insn.b]) {
                stable_fn[insn.a] = stable_local[insn.b];
                if (insn.b < closure_capture_by_local.size() && closure_capture_by_local[insn.b]) {
                    closure_capture_by_reg[insn.a] = *closure_capture_by_local[insn.b];
                } else {
                    closure_capture_by_reg.erase(insn.a);
                }
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    std::size_t inline_sites = 0;
    std::size_t pass_inlined = 0;
    std::size_t accumulated_growth = 0;
    std::vector<JitIrInstruction> rewritten;
    std::vector<std::uint32_t> old_to_new(ir.instructions.size(), 0xFFFFFFFFu);
    rewritten.reserve(ir.instructions.size() + max_inline_insn * 4);
    for (std::size_t old_idx = 0; old_idx < ir.instructions.size(); ++old_idx) {
        const JitIrInstruction& insn = ir.instructions[old_idx];
        if (insn.opcode != JitIrOpcode::CallValue || insn.b >= stable_fn.size() || !stable_fn[insn.b] ||
            inline_sites >= limits.max_inline_sites) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        const std::uint32_t callee = *stable_fn[insn.b];
        if (closure_factory_body_is_pure(bytecode, callee)) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        JitFunctionIr callee_ir;
        std::string error;
        if (!build_jit_ir_from_bytecode(bytecode, callee, callee_ir, error) || callee_ir.instructions.empty() ||
            !callee_ir_is_inline_leaf(callee_ir) || callee_ir_mutates_upvalues(callee_ir)) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        const FunctionInfo& callee_fn = bytecode.functions[callee];
        CaptureConstVec capture_consts;
        if (const auto reg_cap_it = closure_capture_by_reg.find(insn.b); reg_cap_it != closure_capture_by_reg.end()) {
            capture_consts = reg_cap_it->second;
        } else if (const auto cap_it = closure_capture_const.find(callee); cap_it != closure_capture_const.end()) {
            capture_consts = cap_it->second;
        }
        bool all_captures_const = callee_fn.captures.empty();
        if (!callee_fn.captures.empty() && capture_consts.size() == callee_fn.captures.size()) {
            all_captures_const = std::all_of(capture_consts.begin(), capture_consts.end(),
                                             [](const std::optional<std::int64_t>& v) { return v.has_value(); });
        }
        // Multiple unknown captures stay on CallValue. Known integer captures inline as constants.
        if (callee_fn.captures.size() > 1 && !all_captures_const) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        const std::uint32_t callee_cost = estimate_callee_inline_cost(callee_ir);
        if (callee_cost > limits.max_callee_insn) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        const std::uint32_t reg_base = ir.register_count;
        if (insn.a >= reg_base) {
            old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
            rewritten.push_back(insn);
            continue;
        }
        const std::uint32_t slab = std::max({callee_fn.register_count, callee_ir.register_count,
                                             max_register_operand(callee_ir) + 1});
        ir.register_count += slab;
        old_to_new[old_idx] = static_cast<std::uint32_t>(rewritten.size());
        if (!all_captures_const) {
            capture_consts.clear();
        }
        const std::vector<JitIrInstruction> inlined = remap_callee_for_callvalue_inline(
            callee_ir, reg_base, insn.a, insn.b + 1, insn.b, insn.bytecode_ip, insn.block_id, capture_consts);
        rewritten.insert(rewritten.end(), inlined.begin(), inlined.end());
        accumulated_growth += callee_cost;
        ++stats.inlined_calls;
        ++pass_inlined;
        ++inline_sites;
    }
    if (pass_inlined == 0) {
        return false;
    }
    for (JitIrInstruction& mapped : rewritten) {
        if (mapped.opcode != JitIrOpcode::Jump && mapped.opcode != JitIrOpcode::JumpIfFalse &&
            mapped.opcode != JitIrOpcode::IterNext) {
            continue;
        }
        if (mapped.jump_target_insn < old_to_new.size() && old_to_new[mapped.jump_target_insn] != 0xFFFFFFFFu) {
            mapped.jump_target_insn = old_to_new[mapped.jump_target_insn];
        }
    }
    ir.instructions = std::move(rewritten);
    recompute_register_count(ir);
    build_cfg_for_function(ir);
    return true;
}

void inline_stable_call_value_closures(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                                       std::uint32_t max_inline_insn, OptimizingPassStats& stats) {
    for (int pass = 0; pass < 4; ++pass) {
        if (!inline_stable_call_value_pass(bytecode, ir, profile, max_inline_insn, stats)) {
            break;
        }
    }
}

void dedupe_redundant_guards(JitFunctionIr& ir, OptimizingPassStats& stats) {
    std::unordered_map<std::uint64_t, std::uint8_t> seen;
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::Call || insn.opcode == JitIrOpcode::CallValue ||
            insn.opcode == JitIrOpcode::StoreLocal || insn.opcode == JitIrOpcode::GetField ||
            insn.opcode == JitIrOpcode::GetIndex || insn.opcode == JitIrOpcode::ListGetIndex) {
            seen.clear();
            continue;
        }
        if (insn.opcode == JitIrOpcode::GuardInteger) {
            const std::uint64_t key = (static_cast<std::uint64_t>(insn.a) << 32) | 1U;
            if (seen.contains(key)) {
                insn.opcode = JitIrOpcode::Nop;
                ++stats.guard_dedup;
                continue;
            }
            seen[key] = 1;
        } else if (insn.opcode == JitIrOpcode::GuardValueKind) {
            const std::uint64_t key = (static_cast<std::uint64_t>(insn.a) << 32) | (2U + insn.guard_value_kind);
            if (seen.contains(key)) {
                insn.opcode = JitIrOpcode::Nop;
                ++stats.guard_dedup;
                continue;
            }
            seen[key] = 1;
        } else if (insn.opcode == JitIrOpcode::GuardFieldObject) {
            const std::uint64_t key = (static_cast<std::uint64_t>(insn.a) << 32) | 3U;
            if (seen.contains(key)) {
                insn.opcode = JitIrOpcode::Nop;
                ++stats.guard_dedup;
                continue;
            }
            seen[key] = 1;
        }
    }
    OptimizingPassStats cleanup;
    remove_nops(ir, cleanup);
    stats.dead_removed += cleanup.dead_removed;
}

}  // namespace

bool bytecode_function_returns_single_closure(const Bytecode& bytecode, std::uint32_t function_index,
                                              std::uint32_t& inner_function_index) {
    if (function_index >= bytecode.functions.size()) {
        return false;
    }
    const FunctionInfo& fn = bytecode.functions[function_index];
    if (fn.entry >= bytecode.instructions.size()) {
        return false;
    }
    std::uint32_t end_ip = static_cast<std::uint32_t>(bytecode.instructions.size());
    for (const FunctionInfo& other : bytecode.functions) {
        if (other.entry > fn.entry && other.entry < end_ip) {
            end_ip = other.entry;
        }
    }
    std::optional<std::uint32_t> closure_index;
    bool saw_make = false;
    for (std::uint32_t ip = fn.entry; ip < end_ip; ++ip) {
        const Instruction& insn = bytecode.instructions[ip];
        if (insn.opcode == Opcode::Return) {
            break;
        }
        if (insn.opcode == Opcode::Call || insn.opcode == Opcode::CallValue) {
            return false;
        }
        if (insn.opcode == Opcode::MakeClosure) {
            if (saw_make) {
                return false;
            }
            saw_make = true;
            closure_index = insn.b;
        }
    }
    if (!closure_index) {
        return false;
    }
    inner_function_index = *closure_index;
    return true;
}

void jit_common_subexpression_elimination(JitFunctionIr& ir, OptimizingPassStats& stats) {
    if (ir.cfg.empty()) {
        build_cfg_for_function(ir);
    }
    common_subexpression_elimination(ir, stats);
}

void apply_stable_call_value_lowering(const Bytecode& bytecode, JitFunctionIr& ir) {
    OptimizingPassStats stats;
    specialize_stable_call_value(bytecode, ir, stats);
}

void mark_cold_calls_outside_loop(JitFunctionIr& ir) {
    ir.cold_calls_outside_loop = false;
    if (ir.cfg.empty()) {
        build_cfg_for_function(ir);
    }
    std::unordered_set<std::size_t> loop_body_insns;
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        const JitIrInstruction& insn = ir.instructions[i];
        if (insn.opcode == JitIrOpcode::JumpIfFalse && insn.jump_target_insn < i) {
            for (std::size_t j = insn.jump_target_insn; j <= i; ++j) {
                loop_body_insns.insert(j);
            }
        }
    }
    if (loop_body_insns.empty()) {
        const std::unordered_set<std::uint32_t> loop_blocks = loop_body_block_ids(ir);
        if (loop_blocks.empty()) {
            return;
        }
        bool any_call = false;
        for (const JitIrInstruction& insn : ir.instructions) {
            if (insn.opcode != JitIrOpcode::Call && insn.opcode != JitIrOpcode::CallValue) {
                continue;
            }
            any_call = true;
            if (loop_blocks.contains(insn.block_id)) {
                return;
            }
        }
        ir.cold_calls_outside_loop = any_call;
        return;
    }
    bool any_call = false;
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        const JitIrInstruction& insn = ir.instructions[i];
        if (insn.opcode != JitIrOpcode::Call && insn.opcode != JitIrOpcode::CallValue) {
            continue;
        }
        any_call = true;
        if (loop_body_insns.contains(i)) {
            return;
        }
    }
    ir.cold_calls_outside_loop = any_call;
}

void apply_baseline_loop_integer_ops(const Bytecode& bytecode, JitFunctionIr& ir) {
    ir.profile_stable_integer_ops = true;
    OptimizingPassStats stats;
    specialize_integer_ops(bytecode, ir, nullptr, stats);
}

bool try_lower_accumulating_self_recursion(const Bytecode& bytecode, JitFunctionIr& ir) {
    if (!ir_callvalue_is_only_self_recursive(bytecode, ir)) {
        return false;
    }
    std::size_t call_idx = ir.instructions.size();
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        if (ir.instructions[i].opcode == JitIrOpcode::CallValue) {
            if (call_idx != ir.instructions.size()) {
                return false;
            }
            call_idx = i;
        }
    }
    if (call_idx == ir.instructions.size()) {
        return false;
    }
    const JitIrInstruction& call_insn = ir.instructions[call_idx];
    if (call_insn.c != 1) {
        return false;
    }
    const std::uint32_t arg_reg = call_insn.b + 1;
    if (call_idx < 2 || call_idx + 2 >= ir.instructions.size()) {
        return false;
    }
    const JitIrInstruction& ret_insn = ir.instructions[call_idx + 2];
    const JitIrInstruction& add_insn = ir.instructions[call_idx + 1];
    if (ret_insn.opcode != JitIrOpcode::Return || add_insn.opcode != JitIrOpcode::Add ||
        add_insn.a != ret_insn.a) {
        return false;
    }
    const std::uint32_t n_reg_add =
        (add_insn.b == call_insn.a) ? add_insn.c : (add_insn.c == call_insn.a ? add_insn.b : 0xFFFFFFFFu);
    if (n_reg_add == 0xFFFFFFFFu) {
        return false;
    }
    if (ir.instructions[call_idx - 1].opcode != JitIrOpcode::LoadUpvalue) {
        return false;
    }
    if (call_idx < 4) {
        return false;
    }
    const JitIrInstruction& sub_insn = ir.instructions[call_idx - 2];
    const JitIrInstruction& const_one = ir.instructions[call_idx - 3];
    const JitIrInstruction& n_for_sub = ir.instructions[call_idx - 4];
    if (sub_insn.opcode != JitIrOpcode::Sub || sub_insn.a != arg_reg) {
        return false;
    }
    if (n_for_sub.opcode != JitIrOpcode::LoadLocal || n_for_sub.b != 0 || n_for_sub.a != sub_insn.b) {
        return false;
    }
    if (const_one.opcode != JitIrOpcode::LoadConst || const_one.a != sub_insn.c) {
        return false;
    }
    const std::optional<std::int64_t> one_value = integer_from_const(bytecode, const_one.b);
    if (!one_value || *one_value != 1) {
        return false;
    }
    if (ir.instructions.size() < 6) {
        return false;
    }
    const JitIrInstruction& entry_n = ir.instructions[0];
    const JitIrInstruction& entry_zero = ir.instructions[1];
    const JitIrInstruction& entry_cmp = ir.instructions[2];
    const JitIrInstruction& entry_branch = ir.instructions[3];
    const JitIrInstruction& base_const = ir.instructions[4];
    const JitIrInstruction& base_ret = ir.instructions[5];
    if (entry_n.opcode != JitIrOpcode::LoadLocal || entry_n.b != 0) {
        return false;
    }
    if (entry_zero.opcode != JitIrOpcode::LoadConst || entry_cmp.opcode != JitIrOpcode::LessEqual ||
        entry_branch.opcode != JitIrOpcode::JumpIfFalse || base_ret.opcode != JitIrOpcode::Return ||
        base_const.opcode != JitIrOpcode::LoadConst) {
        return false;
    }
    if (entry_cmp.b != entry_n.a || entry_cmp.c != entry_zero.a || entry_branch.a != entry_cmp.a ||
        entry_branch.jump_target_insn != call_idx - 5) {
        return false;
    }
    const std::optional<std::int64_t> zero_value = integer_from_const(bytecode, entry_zero.b);
    if (!zero_value || *zero_value != 0) {
        return false;
    }
    const std::optional<std::int64_t> base_zero = integer_from_const(bytecode, base_const.b);
    if (!base_zero || *base_zero != 0 || base_const.a != base_ret.a) {
        return false;
    }
    if (ir.instructions[call_idx - 5].opcode != JitIrOpcode::LoadLocal ||
        ir.instructions[call_idx - 5].b != 0 || ir.instructions[call_idx - 5].a != n_reg_add) {
        return false;
    }
    const std::uint32_t zero_const_idx = entry_zero.b;
    const std::uint32_t one_const_idx = const_one.b;
    const std::uint32_t acc_local = ir.local_count;
    ir.local_count = std::max(ir.local_count, acc_local + 1);
    const std::uint32_t r_n = 0;
    const std::uint32_t r_tmp = 1;
    const std::uint32_t r_cond = 2;
    const std::uint32_t r_acc = 3;
    const std::uint32_t r_n2 = 4;
    const std::uint32_t r_one = 5;
    const std::uint32_t r_sum = 6;
    std::vector<JitIrInstruction> lowered;
    auto push = [&](JitIrOpcode op, std::uint32_t a = 0, std::uint32_t b = 0, std::uint32_t c = 0) {
        JitIrInstruction insn;
        insn.opcode = op;
        insn.a = a;
        insn.b = b;
        insn.c = c;
        insn.bytecode_ip = ir.instructions.front().bytecode_ip;
        lowered.push_back(insn);
        return static_cast<std::uint32_t>(lowered.size() - 1);
    };
    push(JitIrOpcode::LoadConst, r_tmp, zero_const_idx);
    push(JitIrOpcode::StoreLocal, acc_local, r_tmp);
    const std::uint32_t loop_head = push(JitIrOpcode::LoadLocal, r_n, 0);
    push(JitIrOpcode::LoadConst, r_tmp, zero_const_idx);
    push(JitIrOpcode::Greater, r_cond, r_n, r_tmp);
    const std::uint32_t branch_idx = push(JitIrOpcode::JumpIfFalse, r_cond);
    push(JitIrOpcode::LoadLocal, r_acc, acc_local);
    push(JitIrOpcode::LoadLocal, r_n2, 0);
    push(JitIrOpcode::Add, r_sum, r_acc, r_n2);
    push(JitIrOpcode::StoreLocal, acc_local, r_sum);
    push(JitIrOpcode::LoadLocal, r_n2, 0);
    push(JitIrOpcode::LoadConst, r_one, one_const_idx);
    push(JitIrOpcode::Sub, r_tmp, r_n2, r_one);
    push(JitIrOpcode::StoreLocal, 0, r_tmp);
    const std::uint32_t jump_back_idx = push(JitIrOpcode::Jump);
    const std::uint32_t exit_idx = push(JitIrOpcode::LoadLocal, r_acc, acc_local);
    push(JitIrOpcode::Return, r_acc);
    lowered[branch_idx].jump_target_insn = exit_idx;
    lowered[jump_back_idx].jump_target_insn = loop_head;
    ir.instructions = std::move(lowered);
    ir.blocks.clear();
    ir.cfg.clear();
    recompute_register_count(ir);
    return true;
}

bool run_optimizing_pipeline(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                             OptimizingPassStats& stats, std::string& error) {
    if (ir.instructions.empty()) {
        error = "empty IR for optimizing pipeline";
        return false;
    }
    if (const FunctionOptHints* hints = profile ? profile->function_hints(ir.function_index) : nullptr) {
        ir.profile_stable_integer_ops = hints->stable_integer_ops;
    } else if (std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& b) { return b.loop_header; })) {
        ir.profile_stable_integer_ops = true;
    }
    build_cfg_for_function(ir);
    const bool has_loop_headers =
        std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& b) { return b.loop_header; });
    if (!ir_has_call_sites(ir) && !ir_has_call_value_sites(ir) && !ir_allocates_containers(ir) &&
        !ir_loads_string_constants(bytecode, ir) && !ir_has_dynamic_value_compare_ops(ir) &&
        !ir_has_container_index_ops(ir) && has_loop_headers) {
        apply_stable_call_value_lowering(bytecode, ir);
        rewrite_integer_load_consts(bytecode, ir, stats);
        specialize_integer_ops(bytecode, ir, profile, stats);
        apply_baseline_loop_integer_ops(bytecode, ir);
        build_cfg_for_function(ir);
        buffer_integer_locals(bytecode, ir, stats);
        fuse_inplace_int_increment_before_store(bytecode, ir, stats);
        dead_code_elimination(ir, stats);
        branch_optimization(ir, profile);
        build_cfg_for_function(ir);
        ir.baseline_numeric_loop_fast_path = true;
        mark_cold_calls_outside_loop(ir);
        return true;
    }
    mark_side_effects(ir);
    if (try_lower_accumulating_self_recursion(bytecode, ir)) {
        build_cfg_for_function(ir);
        mark_side_effects(ir);
    }
    specialize_stable_call_value(bytecode, ir, stats);
    if (!ir_has_call_sites(ir) && ir_allocates_containers(ir)) {
        apply_baseline_loop_integer_ops(bytecode, ir);
        build_cfg_for_function(ir);
        mark_cold_calls_outside_loop(ir);
        return true;
    }
    run_escape_and_scalar_replacement(bytecode, ir, stats);
    if (ir_has_call_value_sites(ir)) {
        inline_stable_call_value_closures(bytecode, ir, profile, has_loop_headers ? 20 : 24, stats);
        dedupe_redundant_guards(ir, stats);
        eliminate_dead_closure_factories(bytecode, ir, stats);
        build_cfg_for_function(ir);
        if (ir_has_call_value_sites(ir)) {
            inline_stable_call_value_closures(bytecode, ir, profile, has_loop_headers ? 20 : 24, stats);
            dedupe_redundant_guards(ir, stats);
            build_cfg_for_function(ir);
        }
        mark_cold_calls_outside_loop(ir);
        if (!ir_has_call_value_sites(ir)) {
            rewrite_integer_load_consts(bytecode, ir, stats);
            specialize_integer_ops(bytecode, ir, profile, stats);
        }
        branch_optimization(ir, profile);
        if (!ir_has_call_value_sites(ir) && (!ir_has_call_sites(ir) || ir.cold_calls_outside_loop)) {
            apply_baseline_loop_integer_ops(bytecode, ir);
            build_cfg_for_function(ir);
            buffer_integer_locals(bytecode, ir, stats);
            dead_code_elimination(ir, stats);
        }
        build_cfg_for_function(ir);
        mark_cold_calls_outside_loop(ir);
        if (ir_has_call_sites(ir) || ir_has_call_value_sites(ir)) {
            return true;
        }
    }
    inline_hot_callees(bytecode, ir, profile, has_loop_headers ? 20 : 24, stats);
    constant_fold_and_propagate(bytecode, ir, stats);
    dead_code_elimination(ir, stats);
    if (!ir_has_call_sites(ir) && stats.inlined_calls > 0) {
        rewrite_integer_load_consts(bytecode, ir, stats);
        specialize_integer_ops(bytecode, ir, profile, stats);
        apply_baseline_loop_integer_ops(bytecode, ir);
        build_cfg_for_function(ir);
        buffer_integer_locals(bytecode, ir, stats);
        fuse_inplace_int_increment_before_store(bytecode, ir, stats);
        branch_optimization(ir, profile);
        build_cfg_for_function(ir);
        if (!ir_has_call_value_sites(ir) && !ir_loads_string_constants(bytecode, ir) &&
            !ir_has_dynamic_value_compare_ops(ir) && has_loop_headers) {
            ir.baseline_numeric_loop_fast_path = true;
        }
        mark_cold_calls_outside_loop(ir);
        return true;
    }
    common_subexpression_elimination(ir, stats);
    dead_code_elimination(ir, stats);
    const bool calls_remain = ir_has_call_sites(ir);
    const bool container_alloc_in_fn = ir_allocates_containers(ir);
    if (calls_remain || (has_loop_headers && ir_has_container_index_ops(ir))) {
        specialize_memory_access(ir, stats);
    }
    dedupe_redundant_guards(ir, stats);
            buffer_integer_locals(bytecode, ir, stats);
    specialize_list_integer_element_loads(ir, stats);
    specialize_string_ops(bytecode, ir, stats);
    if (has_loop_headers && !ir_has_call_sites(ir) && !ir_has_call_value_sites(ir) &&
        !ir_loads_string_constants(bytecode, ir)) {
        loop_invariant_motion(ir, stats);
    }
    fold_constant_string_comparisons(bytecode, ir, stats);
    fold_constant_condition_branches(ir, stats);
    if (!container_alloc_in_fn) {
        specialize_integer_ops(bytecode, ir, profile, stats);
        restore_shadow_integer_after_conditional_branch(ir, stats);
        fuse_inplace_int_increment_before_store(bytecode, ir, stats);
    } else {
        apply_baseline_loop_integer_ops(bytecode, ir);
    }
    branch_optimization(ir, profile);
    if (ir.instructions.empty()) {
        error = "optimizer removed all instructions";
        return false;
    }
    build_cfg_for_function(ir);
    mark_cold_calls_outside_loop(ir);
    return true;
}

bool finish_stable_direct_callvalue_loop_compile(const Bytecode& bytecode, JitFunctionIr& ir,
                                                 const JitOptProfile* profile, OptimizingPassStats& stats,
                                                 std::string& error) {
    if (ir_has_call_value_sites(ir)) {
        apply_stable_call_value_lowering(bytecode, ir);
        build_cfg_for_function(ir);
    }
    if (ir_has_call_sites(ir) && profile != nullptr) {
        OptimizingPassStats retry;
        if (!run_optimizing_pipeline(bytecode, ir, nullptr, retry, error)) {
            return false;
        }
        stats.inlined_calls += retry.inlined_calls;
        stats.specialized_ops += retry.specialized_ops;
        stats.constant_folds += retry.constant_folds;
        stats.dead_removed += retry.dead_removed;
    }
    if (!ir_has_call_sites(ir) && !ir_has_call_value_sites(ir)) {
        rewrite_integer_load_consts(bytecode, ir, stats);
        specialize_integer_ops(bytecode, ir, profile, stats);
        apply_baseline_loop_integer_ops(bytecode, ir);
        build_cfg_for_function(ir);
        buffer_integer_locals(bytecode, ir, stats);
        const bool has_loop_headers =
            std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& b) { return b.loop_header; });
        if (has_loop_headers && !ir_loads_string_constants(bytecode, ir) &&
            !ir_has_dynamic_value_compare_ops(ir)) {
            loop_invariant_motion(ir, stats);
        }
        restore_shadow_integer_after_conditional_branch(ir, stats);
        fuse_inplace_int_increment_before_store(bytecode, ir, stats);
        dead_code_elimination(ir, stats);
        branch_optimization(ir, profile);
        build_cfg_for_function(ir);
        if (has_loop_headers && !ir_loads_string_constants(bytecode, ir) &&
            !ir_has_dynamic_value_compare_ops(ir)) {
            ir.baseline_numeric_loop_fast_path = true;
        }
        mark_cold_calls_outside_loop(ir);
    }
    return true;
}

bool ir_may_use_baseline_numeric_loop_fast_path(const Bytecode& bytecode, const JitFunctionIr& ir) {
    const bool has_loop_headers =
        std::any_of(ir.cfg.begin(), ir.cfg.end(), [](const JitCfgBlock& b) { return b.loop_header; });
    if (!has_loop_headers) {
        return false;
    }
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::LoadConst && insn.b < bytecode.constants.size() &&
            bytecode.constants[insn.b].kind == ConstantKind::String) {
            return false;
        }
        if (insn.opcode == JitIrOpcode::Equal || insn.opcode == JitIrOpcode::NotEqual ||
            insn.opcode == JitIrOpcode::StringEqual || insn.opcode == JitIrOpcode::StringNotEqual) {
            return false;
        }
    }
    return true;
}

}  // namespace pscript::jit
