#include "jit/jit_ir.h"

#include "jit/jit_ic.h"
#include "jit/jit_opt.h"

#include <algorithm>
#include <optional>
#include <unordered_map>



namespace pscript::jit {

namespace {



std::uint32_t function_end_ip(const Bytecode& bytecode, std::uint32_t function_index) {
    if (function_index >= bytecode.functions.size()) {
        return static_cast<std::uint32_t>(bytecode.instructions.size());
    }
    const std::uint32_t start = bytecode.functions[function_index].entry;
    std::uint32_t end = static_cast<std::uint32_t>(bytecode.instructions.size());
    for (const FunctionInfo& other : bytecode.functions) {
        if (other.entry > start && other.entry < end) {
            end = other.entry;
        }
    }
    return end;
}

bool function_body_has_direct_self_call(const Bytecode& bytecode, std::uint32_t function_index) {
    const FunctionInfo& fn = bytecode.functions[function_index];
    const std::uint32_t start = fn.entry;
    const std::uint32_t end = function_end_ip(bytecode, function_index);
    for (std::uint32_t ip = start; ip < end && ip < bytecode.instructions.size(); ++ip) {
        const Instruction& insn = bytecode.instructions[ip];
        if (insn.opcode == Opcode::Call && insn.b == function_index) {
            return true;
        }
    }
    return false;
}



bool opcode_supported(Opcode opcode) {

    switch (opcode) {

        case Opcode::LoadConst:

        case Opcode::LoadLocal:

        case Opcode::StoreLocal:

        case Opcode::LoadFunction:

        case Opcode::Add:

        case Opcode::Sub:

        case Opcode::Mul:

        case Opcode::Div:

        case Opcode::Mod:

        case Opcode::UnaryNot:

        case Opcode::Equal:

        case Opcode::NotEqual:

        case Opcode::Less:

        case Opcode::LessEqual:

        case Opcode::Greater:

        case Opcode::GreaterEqual:

        case Opcode::GetIndex:

        case Opcode::GetField:

        case Opcode::GetIter:

        case Opcode::IterNext:

        case Opcode::Jump:

        case Opcode::JumpIfFalse:

        case Opcode::Return:

        case Opcode::Call:

        case Opcode::CallValue:

        case Opcode::LoadUpvalue:

        case Opcode::StoreUpvalue:

        case Opcode::MakeClosure:

        case Opcode::MakeStruct:

        case Opcode::SetField:

        case Opcode::MakeList:

        case Opcode::MakeDict:

        case Opcode::MakeSet:

        case Opcode::SetIndex:

            return true;

        default:

            return false;

    }

}



JitIrOpcode map_binary(Opcode opcode) {

    switch (opcode) {

        case Opcode::Add:

            return JitIrOpcode::Add;

        case Opcode::Sub:

            return JitIrOpcode::Sub;

        case Opcode::Mul:

            return JitIrOpcode::Mul;

        case Opcode::Div:

            return JitIrOpcode::Div;

        case Opcode::Mod:

            return JitIrOpcode::Mod;

        default:

            return JitIrOpcode::Return;

    }

}



JitIrOpcode map_compare(Opcode opcode) {

    switch (opcode) {

        case Opcode::Equal:

            return JitIrOpcode::Equal;

        case Opcode::NotEqual:

            return JitIrOpcode::NotEqual;

        case Opcode::Less:

            return JitIrOpcode::Less;

        case Opcode::LessEqual:

            return JitIrOpcode::LessEqual;

        case Opcode::Greater:

            return JitIrOpcode::Greater;

        case Opcode::GreaterEqual:

            return JitIrOpcode::GreaterEqual;

        default:

            return JitIrOpcode::Return;

    }

}



}  // namespace

bool ir_callvalue_is_only_self_recursive(const Bytecode& bytecode, const JitFunctionIr& ir) {
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
        } else if (insn.opcode == JitIrOpcode::LoadUpvalue && insn.a < reg_closure_fn.size() && insn.b == 0) {
            // Recursive closure pattern: upvalue 0 is the function's own closure (e.g. sum in make_sum).
            reg_closure_fn[insn.a] = ir.function_index;
        }
    }
    bool saw_callvalue = false;
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::CallValue || insn.b >= reg_closure_fn.size() || !reg_closure_fn[insn.b]) {
            continue;
        }
        saw_callvalue = true;
        const std::uint32_t callee = *reg_closure_fn[insn.b];
        if (callee != ir.function_index) {
            return false;
        }
    }
    return saw_callvalue;
}

bool instruction_in_backedge_loop(const JitFunctionIr& ir, std::size_t insn_index) {
    if (insn_index >= ir.instructions.size()) {
        return false;
    }
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        const JitIrInstruction& insn = ir.instructions[i];
        if (insn.opcode != JitIrOpcode::JumpIfFalse || insn.jump_target_insn >= i) {
            continue;
        }
        if (insn_index >= insn.jump_target_insn && insn_index <= i) {
            return true;
        }
    }
    return false;
}

namespace {

void clear_stable_fn_reg(std::vector<std::optional<std::uint32_t>>& stable_fn, std::uint32_t reg) {
    if (reg < stable_fn.size()) {
        stable_fn[reg] = std::nullopt;
    }
}

void clear_stable_fn_dest(std::vector<std::optional<std::uint32_t>>& stable_fn, const JitIrInstruction& insn) {
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
        case JitIrOpcode::Call:
        case JitIrOpcode::CallValue:
            clear_stable_fn_reg(stable_fn, insn.a);
            break;
        default:
            break;
    }
}

}  // namespace

bool ir_callvalue_sites_are_stable_direct(const Bytecode& bytecode, const JitFunctionIr& ir) {
    if (!std::any_of(ir.instructions.begin(), ir.instructions.end(),
                     [](const JitIrInstruction& insn) { return insn.opcode == JitIrOpcode::CallValue; })) {
        return false;
    }
    const FunctionInfo& fn = bytecode.functions[ir.function_index];
    std::vector<std::optional<std::uint32_t>> stable_fn(ir.register_count);
    std::vector<std::optional<std::uint32_t>> stable_local(fn.local_count);
    bool all_stable = true;
    std::optional<std::uint32_t> sole_callvalue_target;
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::CallValue) {
            if (insn.b >= stable_fn.size() || !stable_fn[insn.b]) {
                all_stable = false;
                break;
            }
            const std::uint32_t fn_index = *stable_fn[insn.b];
            if (fn_index >= bytecode.functions.size() || !bytecode.functions[fn_index].captures.empty()) {
                all_stable = false;
                break;
            }
            if (!sole_callvalue_target.has_value()) {
                sole_callvalue_target = fn_index;
            } else if (*sole_callvalue_target != fn_index) {
                all_stable = false;
                break;
            }
        }
        if (insn.opcode == JitIrOpcode::Call && insn.a < stable_fn.size()) {
            std::uint32_t inner = 0;
            if (bytecode_function_returns_single_closure(bytecode, insn.c, inner)) {
                stable_fn[insn.a] = inner;
                continue;
            }
        }
        if (insn.opcode == JitIrOpcode::MakeClosure && insn.a < stable_fn.size()) {
            stable_fn[insn.a] = insn.b;
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadFunction && insn.a < stable_fn.size()) {
            stable_fn[insn.a] = insn.b;
            continue;
        }
        if (insn.opcode == JitIrOpcode::MoveReg && insn.a < stable_fn.size() && insn.b < stable_fn.size() &&
            stable_fn[insn.b]) {
            stable_fn[insn.a] = stable_fn[insn.b];
            continue;
        }
        if (insn.opcode == JitIrOpcode::StoreLocal && insn.a < stable_local.size() && insn.b < stable_fn.size() &&
            stable_fn[insn.b]) {
            stable_local[insn.a] = stable_fn[insn.b];
            continue;
        }
        if (insn.opcode == JitIrOpcode::LoadLocal && insn.b < stable_local.size() && stable_local[insn.b] &&
            insn.a < stable_fn.size()) {
            stable_fn[insn.a] = stable_local[insn.b];
            continue;
        }
        clear_stable_fn_dest(stable_fn, insn);
    }
    return all_stable;
}

bool jit_function_is_eligible(const Bytecode& bytecode, std::uint32_t function_index, std::string& reason) {

    if (function_index >= bytecode.functions.size()) {

        reason = "invalid function index";

        return false;

    }

    const FunctionInfo& fn = bytecode.functions[function_index];

    if (fn.is_native || fn.is_async || fn.is_constructor || fn.has_implicit_this) {

        reason = "unsupported function kind";

        return false;

    }

    if (function_body_has_direct_self_call(bytecode, function_index)) {

        reason = "direct self recursion";

        return false;

    }

    const std::uint32_t start = fn.entry;

    const std::uint32_t end = function_end_ip(bytecode, function_index);

    for (std::uint32_t ip = start; ip < end && ip < bytecode.instructions.size(); ++ip) {

        if (!opcode_supported(bytecode.instructions[ip].opcode)) {

            reason = "unsupported opcode in function body";

            return false;

        }

    }

    return true;

}



bool build_jit_ir_from_bytecode(const Bytecode& bytecode, std::uint32_t function_index, JitFunctionIr& out_ir,

                                std::string& error) {

    if (!jit_function_is_eligible(bytecode, function_index, error)) {

        return false;

    }

    const FunctionInfo& fn = bytecode.functions[function_index];

    const std::uint32_t start = fn.entry;

    const std::uint32_t end = function_end_ip(bytecode, function_index);



    out_ir = {};

    out_ir.function_index = function_index;

    out_ir.register_count = fn.register_count;

    out_ir.local_count = fn.local_count;



    std::unordered_map<std::uint32_t, std::uint32_t> ip_to_insn;

    JitBasicBlock block;

    block.id = 0;

    block.start_bytecode_ip = start;



    for (std::uint32_t ip = start; ip < end; ++ip) {

        ip_to_insn[ip] = static_cast<std::uint32_t>(out_ir.instructions.size());

        const Instruction& insn = bytecode.instructions[ip];

        JitIrInstruction ir;

        ir.bytecode_ip = ip;

        switch (insn.opcode) {

            case Opcode::LoadConst:

                ir.opcode = JitIrOpcode::LoadConst;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::LoadLocal:

                ir.opcode = JitIrOpcode::LoadLocal;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::StoreLocal:

                ir.opcode = JitIrOpcode::StoreLocal;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::LoadFunction:

                ir.opcode = JitIrOpcode::LoadFunction;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::UnaryNot:

                ir.opcode = JitIrOpcode::UnaryNot;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::Add:

            case Opcode::Sub:

            case Opcode::Mul:

            case Opcode::Div:

            case Opcode::Mod:

                ir.opcode = map_binary(insn.opcode);

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::Equal:

            case Opcode::NotEqual:

            case Opcode::Less:

            case Opcode::LessEqual:

            case Opcode::Greater:

            case Opcode::GreaterEqual:

                ir.opcode = map_compare(insn.opcode);

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::GetIndex:

                ir.opcode = JitIrOpcode::GetIndex;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::GetField:

                ir.opcode = JitIrOpcode::GetField;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::GetIter:

                ir.opcode = JitIrOpcode::GetIter;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::IterNext:

                ir.opcode = JitIrOpcode::IterNext;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::Jump:

                ir.opcode = JitIrOpcode::Jump;

                ir.jump_target_insn = 0;

                break;

            case Opcode::JumpIfFalse:

                ir.opcode = JitIrOpcode::JumpIfFalse;

                ir.a = insn.a;

                ir.jump_target_insn = 0;

                break;

            case Opcode::Return:

                ir.opcode = JitIrOpcode::Return;

                ir.a = insn.a;

                break;

            case Opcode::Call:

                ir.opcode = JitIrOpcode::Call;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::CallValue:

                ir.opcode = JitIrOpcode::CallValue;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::LoadUpvalue:

                ir.opcode = JitIrOpcode::LoadUpvalue;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::StoreUpvalue:

                ir.opcode = JitIrOpcode::StoreUpvalue;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::MakeClosure:

                ir.opcode = JitIrOpcode::MakeClosure;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::MakeStruct:

                ir.opcode = JitIrOpcode::MakeStruct;

                ir.a = insn.a;

                ir.b = insn.b;

                break;

            case Opcode::SetField:

                ir.opcode = JitIrOpcode::SetField;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::MakeList:

                ir.opcode = JitIrOpcode::MakeList;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::MakeDict:

                ir.opcode = JitIrOpcode::MakeDict;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::MakeTuple:

                ir.opcode = JitIrOpcode::MakeTuple;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::MakeSet:

                ir.opcode = JitIrOpcode::MakeSet;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            case Opcode::SetIndex:

                ir.opcode = JitIrOpcode::SetIndex;

                ir.a = insn.a;

                ir.b = insn.b;

                ir.c = insn.c;

                break;

            default:

                error = "unsupported opcode during IR build";

                return false;

        }

        out_ir.instructions.push_back(ir);

        block.instructions.push_back(ir);

    }



    for (std::uint32_t ip = start; ip < end; ++ip) {

        const Instruction& insn = bytecode.instructions[ip];

        const auto it = ip_to_insn.find(ip);

        if (it == ip_to_insn.end()) {

            continue;

        }

        JitIrInstruction& ir = out_ir.instructions[it->second];

        if (insn.opcode == Opcode::Jump) {

            const auto target = ip_to_insn.find(insn.a);

            if (target == ip_to_insn.end()) {

                error = "jump target outside function";

                return false;

            }

            ir.jump_target_insn = target->second;

        } else if (insn.opcode == Opcode::JumpIfFalse) {

            const auto target = ip_to_insn.find(insn.b);

            if (target == ip_to_insn.end()) {

                error = "jump target outside function";

                return false;

            }

            ir.jump_target_insn = target->second;

        } else if (insn.opcode == Opcode::IterNext) {

            const auto target = ip_to_insn.find(insn.c);

            if (target == ip_to_insn.end()) {

                error = "iter next exit outside function";

                return false;

            }

            ir.jump_target_insn = target->second;

        }

    }



    out_ir.blocks.push_back(std::move(block));

    build_cfg_for_function(out_ir);

    return true;

}



void build_cfg_for_function(JitFunctionIr& ir) {

    ir.cfg.clear();

    if (ir.instructions.empty()) {

        return;

    }

    std::vector<std::uint32_t> leaders;

    leaders.push_back(0);

    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {

        const JitIrInstruction& insn = ir.instructions[i];

        if (insn.opcode == JitIrOpcode::Jump || insn.opcode == JitIrOpcode::JumpIfFalse) {

            if (insn.jump_target_insn < ir.instructions.size()) {

                leaders.push_back(insn.jump_target_insn);

            }

            if (i + 1 < ir.instructions.size()) {

                leaders.push_back(static_cast<std::uint32_t>(i + 1));

            }

        } else if (insn.opcode == JitIrOpcode::IterNext) {

            if (insn.jump_target_insn < ir.instructions.size()) {

                leaders.push_back(insn.jump_target_insn);

            }

            if (i + 1 < ir.instructions.size()) {

                leaders.push_back(static_cast<std::uint32_t>(i + 1));

            }

        }

    }

    std::sort(leaders.begin(), leaders.end());

    leaders.erase(std::unique(leaders.begin(), leaders.end()), leaders.end());



    for (std::size_t b = 0; b < leaders.size(); ++b) {

        JitCfgBlock block;

        block.id = static_cast<std::uint32_t>(b);

        block.first_insn = leaders[b];

        const std::uint32_t next_leader =

            b + 1 < leaders.size() ? leaders[b + 1] : static_cast<std::uint32_t>(ir.instructions.size());

        block.insn_count = next_leader - block.first_insn;

        ir.cfg.push_back(block);

    }

    for (std::size_t b = 0; b < ir.cfg.size(); ++b) {

        const JitCfgBlock& block = ir.cfg[b];

        for (std::uint32_t idx = block.first_insn; idx < block.first_insn + block.insn_count; ++idx) {

            if (idx < ir.instructions.size()) {

                ir.instructions[idx].block_id = static_cast<std::uint32_t>(b);

            }

        }

        const std::uint32_t last = block.first_insn + block.insn_count - 1;

        if (last >= ir.instructions.size()) {

            continue;

        }

        const JitIrInstruction& term = ir.instructions[last];

        if (term.opcode == JitIrOpcode::Jump) {

            for (const JitCfgBlock& target : ir.cfg) {

                if (target.first_insn == term.jump_target_insn) {

                    ir.cfg[b].successors.push_back(target.id);

                    ir.cfg[target.id].predecessors.push_back(static_cast<std::uint32_t>(b));

                }

            }

        } else if (term.opcode == JitIrOpcode::JumpIfFalse || term.opcode == JitIrOpcode::IterNext) {

            for (const JitCfgBlock& target : ir.cfg) {

                if (target.first_insn == term.jump_target_insn) {

                    ir.cfg[b].successors.push_back(target.id);

                    ir.cfg[target.id].predecessors.push_back(static_cast<std::uint32_t>(b));

                }

            }

            if (b + 1 < ir.cfg.size()) {

                ir.cfg[b].successors.push_back(static_cast<std::uint32_t>(b + 1));

                ir.cfg[b + 1].predecessors.push_back(static_cast<std::uint32_t>(b));

            }

        } else if (term.opcode != JitIrOpcode::Return && b + 1 < ir.cfg.size()) {

            ir.cfg[b].successors.push_back(static_cast<std::uint32_t>(b + 1));

            ir.cfg[b + 1].predecessors.push_back(static_cast<std::uint32_t>(b));

        }

        for (const JitCfgBlock& pred : ir.cfg) {

            for (std::uint32_t succ : pred.successors) {

                if (succ == block.id && pred.id < block.first_insn) {

                    ir.cfg[b].loop_header = true;

                }

            }

        }

    }

}

void assign_call_ic_slots(JitFunctionIr& ir, std::vector<JitCallIcSlot>& slots) {
    std::uint32_t next = 0;
    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::Call || insn.opcode == JitIrOpcode::CallValue) {
            insn.call_ic_index = next++;
        }
    }
    slots.resize(next);
}

}  // namespace pscript::jit


