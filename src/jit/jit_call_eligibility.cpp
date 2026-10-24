#include "jit/jit_call_eligibility.h"

#include <string>

namespace pscript::jit {
namespace {

std::uint32_t function_body_end(const Bytecode& bytecode, std::uint32_t function_index) {
    if (function_index + 1 < bytecode.functions.size()) {
        return bytecode.functions[function_index + 1].entry;
    }
    return static_cast<std::uint32_t>(bytecode.instructions.size());
}

}  // namespace

bool callee_body_is_arg_plus_const(const Bytecode& bytecode, const FunctionInfo& callee_fn, std::uint32_t callee_index,
                                   std::int64_t& constant_out) {
    if (callee_fn.arity != 1 || !callee_fn.captures.empty()) {
        return false;
    }
    const std::uint32_t start = callee_fn.entry;
    const std::uint32_t end = function_body_end(bytecode, callee_index);
    bool seen_local = false;
    bool seen_const = false;
    bool seen_add = false;
    bool seen_return = false;
    std::int64_t parsed_const = 0;
    for (std::uint32_t ip = start; ip < end; ++ip) {
        const Instruction& ins = bytecode.instructions[ip];
        switch (ins.opcode) {
            case Opcode::LoadLocal:
                if (ins.b != 0) {
                    return false;
                }
                seen_local = true;
                break;
            case Opcode::LoadConst:
                if (ins.b >= bytecode.constants.size() ||
                    bytecode.constants[ins.b].kind != ConstantKind::Integer) {
                    return false;
                }
                try {
                    parsed_const = std::stoll(bytecode.constants[ins.b].lexeme);
                } catch (...) {
                    return false;
                }
                seen_const = true;
                break;
            case Opcode::Add:
                seen_add = true;
                break;
            case Opcode::Return:
                seen_return = true;
                break;
            default:
                return false;
        }
    }
    if (!seen_local || !seen_const || !seen_add || !seen_return) {
        return false;
    }
    constant_out = parsed_const;
    return true;
}

bool callee_body_is_arg_plus_one(const Bytecode& bytecode, const FunctionInfo& callee_fn,
                                 std::uint32_t callee_index) {
    std::int64_t constant = 0;
    return callee_body_is_arg_plus_const(bytecode, callee_fn, callee_index, constant) && constant == 1;
}

bool callee_body_is_identity(const Bytecode& bytecode, const FunctionInfo& callee_fn, std::uint32_t callee_index) {
    if (callee_fn.arity != 1 || !callee_fn.captures.empty()) {
        return false;
    }
    const std::uint32_t start = callee_fn.entry;
    const std::uint32_t end = function_body_end(bytecode, callee_index);
    bool seen_local = false;
    bool seen_return = false;
    for (std::uint32_t ip = start; ip < end; ++ip) {
        const Instruction& ins = bytecode.instructions[ip];
        if (ins.opcode == Opcode::LoadLocal) {
            if (ins.b != 0) {
                return false;
            }
            seen_local = true;
        } else if (ins.opcode == Opcode::Return) {
            seen_return = true;
        } else {
            return false;
        }
    }
    return seen_local && seen_return;
}

}  // namespace pscript::jit
