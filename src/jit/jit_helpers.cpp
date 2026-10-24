#include "jit/jit_helpers.h"

#include "compiler/bytecode.h"
#include "jit/jit_code.h"
#include "jit/jit_deopt.h"
#include "jit/jit_runtime.h"
#include "jit/jit_ic.h"
#include "jit/jit_exec_state.h"
#include "runtime/iterator.h"
#include "vm/vm.h"

#include <stdexcept>

namespace pscript::jit {

JitIcMetrics* ic_metrics_for_state(JitExecState* state) {
    if (state == nullptr) {
        return nullptr;
    }
    if (state->ic_metrics != nullptr) {
        return state->ic_metrics;
    }
    if (state->host_vm != nullptr) {
        return &state->host_vm->jit_engine().ic_metrics();
    }
    return nullptr;
}

namespace {

std::int64_t parse_integer(const std::string& lexeme) {
    std::size_t idx = 0;
    const long long value = std::stoll(lexeme, &idx, 10);
    if (idx != lexeme.size()) {
        throw std::runtime_error("invalid constant");
    }
    return static_cast<std::int64_t>(value);
}

double parse_float(const std::string& lexeme) {
    std::size_t idx = 0;
    const double value = std::stod(lexeme, &idx);
    if (idx != lexeme.size()) {
        throw std::runtime_error("invalid constant");
    }
    return value;
}

std::string decode_string_lexeme(const std::string& lexeme) {
    if (lexeme.size() < 2 || lexeme.front() != '"' || lexeme.back() != '"') {
        throw std::runtime_error("invalid string constant");
    }
    std::string out;
    out.reserve(lexeme.size() - 2);
    for (std::size_t i = 1; i + 1 < lexeme.size(); ++i) {
        const char c = lexeme[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= lexeme.size() - 1) {
            throw std::runtime_error("invalid string escape");
        }
        const char next = lexeme[++i];
        switch (next) {
            case 'n':
                out.push_back('\n');
                break;
            case 't':
                out.push_back('\t');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '"':
                out.push_back('"');
                break;
            default:
                out.push_back(next);
                break;
        }
    }
    return out;
}

Value load_constant(const Bytecode& bytecode, std::uint32_t index) {
    if (index >= bytecode.constants.size()) {
        throw std::runtime_error("invalid constant index");
    }
    const Constant& constant = bytecode.constants[index];
    switch (constant.kind) {
        case ConstantKind::Integer:
            return Runtime::make_integer(parse_integer(constant.lexeme));
        case ConstantKind::Float:
            return Runtime::make_float(parse_float(constant.lexeme));
        case ConstantKind::String:
            return Runtime::make_string(decode_string_lexeme(constant.lexeme));
        case ConstantKind::Bool:
            return Runtime::make_bool(constant.bool_value);
        case ConstantKind::Null:
            return Runtime::make_null();
        case ConstantKind::Bytes:
            return Runtime::make_bytes(constant.bytes_value);
    }
    return Runtime::make_null();
}

Value& reg(JitExecState* state, std::uint32_t index) {
    if (index >= state->registers.size()) {
        state->registers.resize(index + 1);
        state->register_data = state->registers.data();
    }
    return state->registers[index];
}

const Value& read_reg(JitExecState* state, std::uint32_t index) {
    return reg(state, index);
}

Value read_local(JitExecState* state, std::uint32_t index) {
    if (index >= state->locals.size() || !state->locals[index]) {
        return Runtime::make_null();
    }
    return state->locals[index]->value ? *state->locals[index]->value : Runtime::make_null();
}

void write_local(JitExecState* state, std::uint32_t index, Value value) {
    if (index >= state->locals.size()) {
        state->locals.resize(index + 1);
    }
    if (!state->locals[index]) {
        state->locals[index] = std::make_shared<Cell>();
    }
    state->locals[index]->value = std::move(value);
}

Value read_upvalue(JitExecState* state, std::uint32_t index) {
    if (index >= state->upvalues.size() || !state->upvalues[index]) {
        throw std::runtime_error("invalid closure access");
    }
    if (!state->upvalues[index]->value) {
        return Runtime::make_null();
    }
    return *state->upvalues[index]->value;
}

void write_upvalue(JitExecState* state, std::uint32_t index, Value value) {
    if (index >= state->upvalues.size() || !state->upvalues[index]) {
        throw std::runtime_error("invalid closure access");
    }
    state->upvalues[index]->value = std::move(value);
}

double as_number(const Value& value) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    if (value.kind == ValueKind::Float) {
        return value.floating;
    }
    throw std::runtime_error("type error");
}

void mark_fallback(JitExecState* state) {
    state->fallback = true;
}

}  // namespace

extern "C" void jit_helper_load_const(JitExecState* state, std::uint32_t dest, std::uint32_t const_index) {
    try {
        reg(state, dest) = load_constant(*state->bytecode, const_index);
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_move_local_to_reg(JitExecState* state, std::uint32_t dest, std::uint32_t local_index) {
    reg(state, dest) = read_local(state, local_index);
}

extern "C" void jit_helper_move_reg_to_local(JitExecState* state, std::uint32_t local_index, std::uint32_t src) {
    write_local(state, local_index, read_reg(state, src));
}

extern "C" void jit_helper_load_local_integer_to_reg(JitExecState* state, std::uint32_t dest,
                                                     std::uint32_t local_index) {
    const Value value = read_local(state, local_index);
    if (value.kind == ValueKind::Null) {
        // Integer-only locals are null until their first store. The shadow is overwritten
        // by that store; treating the unread null as 0 keeps the native entry alive.
        reg(state, dest) = Runtime::make_integer(0);
        return;
    }
    if (value.kind != ValueKind::Integer) {
        if (value.kind == ValueKind::Function) {
            reg(state, dest) = value;
            return;
        }
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_integer(value.integer);
}

extern "C" void jit_helper_store_local_integer_from_reg(JitExecState* state, std::uint32_t local_index,
                                                        std::uint32_t src) {
    const Value& value = read_reg(state, src);
    if (value.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    write_local(state, local_index, Runtime::make_integer(value.integer));
}

void flush_shadow_integer_locals(JitExecState* state, const std::int32_t* local_to_shadow,
                                 std::uint32_t local_count) {
    if (!state || !local_to_shadow) {
        return;
    }
    for (std::uint32_t local = 0; local < local_count; ++local) {
        const std::int32_t shadow = local_to_shadow[local];
        if (shadow < 0) {
            continue;
        }
        jit_helper_store_local_integer_fast(state, local, static_cast<std::uint32_t>(shadow));
    }
}

void hydrate_shadow_integer_locals(JitExecState* state, const std::int32_t* local_to_shadow,
                                   std::uint32_t local_count);

extern "C" void jit_helper_flush_integer_shadow(JitExecState* state) {
    if (state == nullptr || state->integer_shadow_map == nullptr || state->integer_shadow_count == 0) {
        return;
    }
    flush_shadow_integer_locals(state, state->integer_shadow_map, state->integer_shadow_count);
}

extern "C" void jit_helper_hydrate_integer_shadow(JitExecState* state) {
    if (state == nullptr || state->integer_shadow_map == nullptr || state->integer_shadow_count == 0) {
        return;
    }
    hydrate_shadow_integer_locals(state, state->integer_shadow_map, state->integer_shadow_count);
}

void hydrate_shadow_integer_locals(JitExecState* state, const std::int32_t* local_to_shadow,
                                   std::uint32_t local_count) {
    if (!state || !local_to_shadow) {
        return;
    }
    for (std::uint32_t local = 0; local < local_count; ++local) {
        const std::int32_t shadow = local_to_shadow[local];
        if (shadow < 0) {
            continue;
        }
        jit_helper_load_local_integer_to_reg(state, static_cast<std::uint32_t>(shadow), local);
    }
}

void hydrate_shadow_integer_locals_for_entry(JitExecState* state, const std::int32_t* local_to_shadow,
                                             std::uint32_t local_count, std::uint32_t arity) {
    if (!state || !local_to_shadow) {
        return;
    }
    if (arity > 0 && arity <= 8) {
        for (std::uint32_t local = 0; local < local_count; ++local) {
            const std::int32_t shadow = local_to_shadow[local];
            if (shadow < 0) {
                continue;
            }
            if (local < arity) {
                const Value param = read_local(state, local);
                if (param.kind != ValueKind::Integer) {
                    hydrate_shadow_integer_locals(state, local_to_shadow, local_count);
                    return;
                }
                const std::uint32_t shadow_reg = static_cast<std::uint32_t>(shadow);
                if (shadow_reg >= state->registers.size()) {
                    state->registers.resize(shadow_reg + 1);
                    state->register_data = state->registers.data();
                }
                Value& shadow_value = state->registers[shadow_reg];
                shadow_value.kind = ValueKind::Integer;
                shadow_value.integer = param.integer;
                if (local < state->registers.size()) {
                    state->registers[local].kind = ValueKind::Integer;
                    state->registers[local].integer = param.integer;
                }
                continue;
            }
            const std::uint32_t shadow_reg = static_cast<std::uint32_t>(shadow);
            if (shadow_reg >= state->registers.size()) {
                continue;
            }
            Value& shadow_value = state->registers[shadow_reg];
            shadow_value.kind = ValueKind::Integer;
            shadow_value.integer = 0;
        }
        return;
    }
    hydrate_shadow_integer_locals(state, local_to_shadow, local_count);
}

extern "C" void jit_helper_store_local_integer_fast(JitExecState* state, std::uint32_t local_index,
                                                    std::uint32_t src_reg) {
    if (local_index >= state->locals.size() || !state->locals[local_index]) {
        mark_fallback(state);
        return;
    }
    if (src_reg >= state->registers.size()) {
        mark_fallback(state);
        return;
    }
    const Value& reg_value = read_reg(state, src_reg);
    if (reg_value.kind != ValueKind::Integer) {
        if (reg_value.kind == ValueKind::Function) {
            write_local(state, local_index, reg_value);
            return;
        }
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    const std::int64_t value = reg_value.integer;
    std::shared_ptr<Cell>& cell = state->locals[local_index];
    if (!cell->value) {
        cell->value = Runtime::make_integer(value);
        return;
    }
    cell->value->kind = ValueKind::Integer;
    cell->value->integer = value;
}

void sync_local_capture_cell(JitExecState* state, std::uint32_t local_index) {
    if (state == nullptr) {
        return;
    }
    if (local_index >= state->locals.size()) {
        state->locals.resize(static_cast<std::size_t>(local_index) + 1);
    }
    if (!state->locals[local_index]) {
        state->locals[local_index] = std::make_shared<Cell>();
    }
    if (state->locals[local_index]->value.has_value()) {
        return;
    }
    if (state->integer_shadow_map != nullptr && local_index < state->integer_shadow_count) {
        const std::int32_t shadow = state->integer_shadow_map[local_index];
        if (shadow >= 0) {
            jit_helper_store_local_integer_fast(state, local_index, static_cast<std::uint32_t>(shadow));
            if (state->locals[local_index]->value.has_value()) {
                return;
            }
        }
    }
    if (local_index < state->registers.size()) {
        const Value& reg_value = read_reg(state, local_index);
        if (reg_value.kind != ValueKind::Null) {
            state->locals[local_index]->value = reg_value;
        }
    }
}

extern "C" void jit_helper_binary(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right,
                                  std::uint32_t op) {
    try {
        const Value l = read_reg(state, left);
        const Value r = read_reg(state, right);
        switch (op) {
            case static_cast<std::uint32_t>(Opcode::Add):
                if (l.kind == ValueKind::String && r.kind == ValueKind::String) {
                    reg(state, dest) = Runtime::concat_strings(l.string, r.string);
                    return;
                }
                if (l.kind == ValueKind::Integer && r.kind == ValueKind::Integer) {
                    reg(state, dest) = Runtime::make_integer(l.integer + r.integer);
                    return;
                }
                if (l.kind != ValueKind::Integer && l.kind != ValueKind::Float) {
                    mark_fallback(state);
                    return;
                }
                if (r.kind != ValueKind::Integer && r.kind != ValueKind::Float) {
                    mark_fallback(state);
                    return;
                }
                reg(state, dest) = Runtime::make_float(as_number(l) + as_number(r));
                return;
            case static_cast<std::uint32_t>(Opcode::Sub):
                if (l.kind == ValueKind::Integer && r.kind == ValueKind::Integer) {
                    reg(state, dest) = Runtime::make_integer(l.integer - r.integer);
                    return;
                }
                reg(state, dest) = Runtime::make_float(as_number(l) - as_number(r));
                return;
            case static_cast<std::uint32_t>(Opcode::Mul):
                if (l.kind == ValueKind::Integer && r.kind == ValueKind::Integer) {
                    reg(state, dest) = Runtime::make_integer(l.integer * r.integer);
                    return;
                }
                reg(state, dest) = Runtime::make_float(as_number(l) * as_number(r));
                return;
            case static_cast<std::uint32_t>(Opcode::Div):
                reg(state, dest) = Runtime::make_float(as_number(l) / as_number(r));
                return;
            case static_cast<std::uint32_t>(Opcode::Mod):
                if (l.kind == ValueKind::Integer && r.kind == ValueKind::Integer) {
                    if (r.integer == 0) {
                        mark_fallback(state);
                        return;
                    }
                    reg(state, dest) = Runtime::make_integer(l.integer % r.integer);
                    return;
                }
                mark_fallback(state);
                return;
            default:
                mark_fallback(state);
        }
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_compare(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right,
                                   std::uint32_t op) {
    try {
        const Value l = read_reg(state, left);
        const Value r = read_reg(state, right);
        bool result = false;
        switch (op) {
            case static_cast<std::uint32_t>(Opcode::Equal):
                result = Runtime::equals(l, r);
                break;
            case static_cast<std::uint32_t>(Opcode::NotEqual):
                result = !Runtime::equals(l, r);
                break;
            case static_cast<std::uint32_t>(Opcode::Less):
                result = as_number(l) < as_number(r);
                break;
            case static_cast<std::uint32_t>(Opcode::LessEqual):
                result = as_number(l) <= as_number(r);
                break;
            case static_cast<std::uint32_t>(Opcode::Greater):
                result = as_number(l) > as_number(r);
                break;
            case static_cast<std::uint32_t>(Opcode::GreaterEqual):
                result = as_number(l) >= as_number(r);
                break;
            default:
                mark_fallback(state);
                return;
        }
        reg(state, dest) = Runtime::make_bool(result);
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_return(JitExecState* state, std::uint32_t src) {
    state->return_value = read_reg(state, src);
    state->completed = 1;
}

extern "C" void jit_helper_jump_if_false(JitExecState* state, std::uint32_t cond_reg,
                                         std::uint32_t target_insn_index) {
    if (!Runtime::is_truthy(reg(state, cond_reg))) {
        state->current_insn = target_insn_index;
    } else {
        ++state->current_insn;
    }
}

extern "C" void jit_helper_jump(JitExecState* state, std::uint32_t target_insn_index) {
    state->current_insn = target_insn_index;
}

extern "C" void jit_helper_move_reg(JitExecState* state, std::uint32_t dest, std::uint32_t src) {
    const Value value = read_reg(state, src);
    reg(state, dest) = value;
}

extern "C" void jit_helper_load_const_int(JitExecState* state, std::uint32_t dest, std::uint32_t low,
                                          std::uint32_t high) {
    const std::int64_t value =
        static_cast<std::int64_t>(static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32));
    reg(state, dest) = Runtime::make_integer(value);
}

extern "C" void jit_helper_guard_integer(JitExecState* state, std::uint32_t reg_index, std::uint32_t site_id) {
    const Value value = read_reg(state, reg_index);
    if (value.kind != ValueKind::Integer) {
        jit_request_deopt(state, site_id);
    }
}

static void store_integer_reg(Value& dest, std::int64_t value) {
    dest.kind = ValueKind::Integer;
    dest.integer = value;
}

extern "C" void jit_helper_int_add(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    const Value l = read_reg(state, left);
    const Value r = read_reg(state, right);
    if (l.kind != ValueKind::Integer || r.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    store_integer_reg(reg(state, dest), l.integer + r.integer);
}

extern "C" void jit_helper_int_sub(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    const Value l = read_reg(state, left);
    const Value r = read_reg(state, right);
    if (l.kind != ValueKind::Integer || r.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    store_integer_reg(reg(state, dest), l.integer - r.integer);
}

extern "C" void jit_helper_int_mul(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    const Value l = read_reg(state, left);
    const Value r = read_reg(state, right);
    if (l.kind != ValueKind::Integer || r.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    store_integer_reg(reg(state, dest), l.integer * r.integer);
}

extern "C" void jit_helper_int_mod(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    const Value l = read_reg(state, left);
    const Value r = read_reg(state, right);
    if (l.kind != ValueKind::Integer || r.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    if (r.integer == 0) {
        state->fallback = true;
        return;
    }
    store_integer_reg(reg(state, dest), l.integer % r.integer);
}

extern "C" void jit_helper_int_div(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    const Value l = read_reg(state, left);
    const Value r = read_reg(state, right);
    if (l.kind != ValueKind::Integer || r.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    if (r.integer == 0) {
        state->fallback = true;
        return;
    }
    store_integer_reg(reg(state, dest), l.integer / r.integer);
}

extern "C" void jit_helper_copy_integer_reg(JitExecState* state, std::uint32_t dest, std::uint32_t src) {
    const Value& value = read_reg(state, src);
    if (value.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    store_integer_reg(reg(state, dest), value.integer);
}

extern "C" void jit_helper_int_less(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    const Value l = read_reg(state, left);
    const Value r = read_reg(state, right);
    if (l.kind != ValueKind::Integer || r.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_bool(l.integer < r.integer);
}

extern "C" void jit_helper_unary_not(JitExecState* state, std::uint32_t dest, std::uint32_t src) {
    const Value value = read_reg(state, src);
    reg(state, dest) = Runtime::make_bool(!Runtime::is_truthy(value));
}

extern "C" void jit_helper_get_index(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                   std::uint32_t index) {
    try {
        const Value object_value = read_reg(state, object);
        const Value index_value = read_reg(state, index);
        reg(state, dest) = Runtime::get_index(object_value, index_value);
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_get_field(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                   std::uint32_t field_index) {
    try {
        const Value object_value = read_reg(state, object);
        reg(state, dest) = Runtime::get_field(object_value, field_index);
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_get_iter(JitExecState* state, std::uint32_t dest, std::uint32_t iterable) {
    try {
        const Value iterable_value = read_reg(state, iterable);
        reg(state, dest) = runtime_get_iter(iterable_value);
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_iter_next(JitExecState* state, std::uint32_t dest, std::uint32_t iterator,
                                     std::uint32_t exit_insn) {
    const Value it = read_reg(state, iterator);
    if (!is_native_iterator(it)) {
        mark_fallback(state);
        return;
    }
    Value next;
    try {
        if (!native_iterator_try_next(it, next)) {
            state->current_insn = exit_insn;
            return;
        }
        reg(state, dest) = std::move(next);
    } catch (...) {
        mark_fallback(state);
        return;
    }
    ++state->current_insn;
}

extern "C" void jit_helper_load_function(JitExecState* state, std::uint32_t dest, std::uint32_t function_index) {
    if (!state->bytecode || function_index >= state->bytecode->functions.size()) {
        mark_fallback(state);
        return;
    }
    auto data = std::make_shared<ClosureData>();
    data->function_index = function_index;
    data->name = state->bytecode->functions[function_index].name;
    reg(state, dest) = Runtime::make_function(std::move(data));
}

extern "C" void jit_helper_load_upvalue(JitExecState* state, std::uint32_t dest, std::uint32_t upvalue_index) {
    if (state == nullptr || upvalue_index >= state->upvalues.size() || !state->upvalues[upvalue_index]) {
        mark_fallback(state);
        return;
    }
    const std::shared_ptr<Cell>& cell = state->upvalues[upvalue_index];
    reg(state, dest) = cell->value ? *cell->value : Runtime::make_null();
}

extern "C" void jit_helper_load_closure_capture(JitExecState* state, std::uint32_t dest, std::uint32_t closure_reg,
                                              std::uint32_t capture_index) {
    if (state == nullptr || closure_reg >= state->registers.size()) {
        mark_fallback(state);
        return;
    }
    const Value& closure = state->registers[closure_reg];
    if (closure.kind != ValueKind::Function || !closure.function ||
        capture_index >= closure.function->captures.size() || !closure.function->captures[capture_index]) {
        mark_fallback(state);
        return;
    }
    const std::shared_ptr<Cell>& cell = closure.function->captures[capture_index];
    reg(state, dest) = cell->value ? *cell->value : Runtime::make_null();
}

extern "C" void jit_helper_store_upvalue(JitExecState* state, std::uint32_t upvalue_index, std::uint32_t src_reg) {
    try {
        write_upvalue(state, upvalue_index, read_reg(state, src_reg));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_materialize_closure_two_integer_captures(JitExecState* state, std::uint32_t dest,
                                                                    std::uint32_t inner_function_index,
                                                                    std::uint32_t cap0, std::uint32_t cap1) {
    if (!state->bytecode || inner_function_index >= state->bytecode->functions.size()) {
        mark_fallback(state);
        return;
    }
    try {
        const FunctionInfo& fn = state->bytecode->functions[inner_function_index];
        auto data = std::make_shared<ClosureData>();
        data->function_index = inner_function_index;
        data->name = fn.name;
        for (const std::int64_t capture :
             {static_cast<std::int64_t>(static_cast<std::int32_t>(cap0)),
              static_cast<std::int64_t>(static_cast<std::int32_t>(cap1))}) {
            auto cell = std::make_shared<Cell>();
            cell->value = Runtime::make_integer(capture);
            data->captures.push_back(cell);
        }
        reg(state, dest) = Runtime::make_function(std::move(data));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_materialize_closure_integer_capture(JitExecState* state, std::uint32_t dest,
                                                               std::uint32_t inner_function_index, std::uint32_t low,
                                                               std::uint32_t high) {
    if (!state->bytecode || inner_function_index >= state->bytecode->functions.size()) {
        mark_fallback(state);
        return;
    }
    try {
        const FunctionInfo& fn = state->bytecode->functions[inner_function_index];
        const std::int64_t capture =
            static_cast<std::int64_t>(static_cast<std::uint64_t>(high) << 32 | static_cast<std::uint64_t>(low));
        auto data = std::make_shared<ClosureData>();
        data->function_index = inner_function_index;
        data->name = fn.name;
        auto cell = std::make_shared<Cell>();
        cell->value = Runtime::make_integer(capture);
        data->captures.push_back(cell);
        reg(state, dest) = Runtime::make_function(std::move(data));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_make_closure(JitExecState* state, std::uint32_t dest, std::uint32_t function_index) {
    if (!state->bytecode || function_index >= state->bytecode->functions.size()) {
        mark_fallback(state);
        return;
    }
    try {
        const FunctionInfo& fn = state->bytecode->functions[function_index];
        auto data = std::make_shared<ClosureData>();
        data->function_index = function_index;
        data->name = fn.name;
        data->captures.reserve(fn.captures.size());
        for (const Capture& capture : fn.captures) {
            if (capture.from_local) {
                sync_local_capture_cell(state, capture.index);
                if (!state->locals[capture.index] || !state->locals[capture.index]->value.has_value()) {
                    throw std::runtime_error("invalid closure access");
                }
                // Nested JIT calls reuse scratch locals; escaping closures must not alias cells that
                // recycle_local_cells may clear when the callee scratch frame is torn down.
                if (state->jit_call_depth > 0) {
                    auto copied = std::make_shared<Cell>();
                    copied->value = *state->locals[capture.index]->value;
                    data->captures.push_back(copied);
                } else {
                    data->captures.push_back(state->locals[capture.index]);
                }
            } else {
                if (capture.index >= state->upvalues.size() || !state->upvalues[capture.index]) {
                    throw std::runtime_error("invalid closure access");
                }
                data->captures.push_back(state->upvalues[capture.index]);
            }
        }
        reg(state, dest) = Runtime::make_function(std::move(data));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" JitExecState* jit_nested_begin_mono_call(JitExecState* caller, std::uint32_t first_arg,
                                                    std::uint32_t callee_index, std::uint32_t ic_index) {
    if (caller == nullptr || caller->host_vm == nullptr || caller->bytecode == nullptr ||
        callee_index >= caller->bytecode->functions.size()) {
        return nullptr;
    }
    if (caller->call_ic_slots == nullptr || ic_index >= caller->call_ic_slot_count) {
        return nullptr;
    }
    JitCallIcSlot* slot = &caller->call_ic_slots[ic_index];
    if (slot->mode != JitCallIcMode::Monomorphic || slot->mono.function_index != callee_index ||
        slot->mono.entry == nullptr) {
        return nullptr;
    }
    if (slot->mono.callee_register_count == 0) {
        const JitCompiledCode* code = caller->host_vm->jit_engine().lookup(callee_index);
        hydrate_call_ic_entry(slot->mono, callee_index, code, slot->mono.closure_token, caller->bytecode);
    }
    if (slot->mono.callee_register_count == 0) {
        return nullptr;
    }
    const FunctionInfo& fn = caller->bytecode->functions[callee_index];
    if (!fn.captures.empty()) {
        return nullptr;
    }
    return caller->host_vm->prepare_nested_mono_callee(*caller, first_arg, slot->mono, callee_index, fn, nullptr);
}

extern "C" JitExecState* jit_nested_begin_mono_call_value(JitExecState* caller, std::uint32_t first_arg,
                                                          std::uint32_t callee_reg, std::uint32_t ic_index) {
    if (caller == nullptr || caller->host_vm == nullptr || caller->bytecode == nullptr ||
        caller->call_ic_slots == nullptr || ic_index >= caller->call_ic_slot_count ||
        callee_reg >= caller->registers.size()) {
        return nullptr;
    }
    const Value& callee = caller->registers[callee_reg];
    if (callee.kind != ValueKind::Function || !callee.function ||
        callee.function->module_index != 0xFFFFFFFFu) {
        return nullptr;
    }
    const std::uint32_t callee_index = callee.function->function_index;
    JitCallIcSlot* slot = &caller->call_ic_slots[ic_index];
    if (slot->mode != JitCallIcMode::Monomorphic || slot->mono.function_index != callee_index ||
        slot->mono.entry == nullptr ||
        slot->mono.closure_token != static_cast<const void*>(callee.function.get())) {
        return nullptr;
    }
    if (callee_index >= caller->bytecode->functions.size()) {
        return nullptr;
    }
    const FunctionInfo& fn = caller->bytecode->functions[callee_index];
    if (!fn.captures.empty() && callee.function->captures.size() != fn.captures.size()) {
        return nullptr;
    }
    if (fn.captures.size() > 1) {
        return nullptr;
    }
    if (slot->mono.callee_register_count == 0) {
        const JitCompiledCode* code = caller->host_vm->jit_engine().lookup(callee_index);
        hydrate_call_ic_entry(slot->mono, callee_index, code, slot->mono.closure_token, caller->bytecode);
    }
    if (slot->mono.callee_register_count == 0) {
        return nullptr;
    }
    const std::vector<std::shared_ptr<Cell>>* captures =
        callee.function->captures.empty() ? nullptr : &callee.function->captures;
    if (JitIcMetrics* metrics = ic_metrics_for_state(caller)) {
        ++metrics->nested_begin_mono_call_value;
    }
    return caller->host_vm->prepare_nested_mono_callee(*caller, first_arg, slot->mono, callee_index, fn, captures);
}

extern "C" JitExecState* jit_nested_begin_mono_call_value_after_ic_guard(JitExecState* caller,
                                                                         std::uint32_t first_arg,
                                                                         std::uint32_t callee_reg,
                                                                         std::uint32_t ic_index) {
    if (caller == nullptr || caller->host_vm == nullptr || caller->bytecode == nullptr ||
        caller->call_ic_slots == nullptr || ic_index >= caller->call_ic_slot_count ||
        callee_reg >= caller->registers.size()) {
        return nullptr;
    }
    JitCallIcSlot& slot = caller->call_ic_slots[ic_index];
    const std::uint32_t callee_index = slot.mono.function_index;
    if (callee_index >= caller->bytecode->functions.size()) {
        return nullptr;
    }
    const FunctionInfo& fn = caller->bytecode->functions[callee_index];
    if (!fn.captures.empty() && fn.captures.size() > 1) {
        return nullptr;
    }
    const Value& callee_val = caller->registers[callee_reg];
    const std::vector<std::shared_ptr<Cell>>* captures = nullptr;
    if (!fn.captures.empty() && callee_val.function && !callee_val.function->captures.empty()) {
        captures = &callee_val.function->captures;
    }
    if (JitIcMetrics* metrics = ic_metrics_for_state(caller)) {
        ++metrics->nested_begin_mono_call_value;
    }
    return caller->host_vm->prepare_nested_mono_callee(*caller, first_arg, slot.mono, callee_index, fn, captures);
}

extern "C" bool jit_nested_invoke_mono_call_value_emit(JitExecState* caller, std::uint32_t dest,
                                                       std::uint32_t first_arg, std::uint32_t callee_reg,
                                                       std::uint32_t ic_index) {
    if (caller == nullptr) {
        return false;
    }
    JitIcMetrics* metrics = ic_metrics_for_state(caller);
    if (metrics != nullptr) {
        ++metrics->emit_mono_invoke_fused;
    }
    JitExecState* callee = jit_nested_begin_mono_call_value(caller, first_arg, callee_reg, ic_index);
    if (callee == nullptr) {
        return false;
    }
    if (caller->call_ic_slots != nullptr && ic_index < caller->call_ic_slot_count) {
        record_call_ic_hit(&caller->call_ic_slots[ic_index], metrics, true, false);
    }
    if (caller->call_ic_slots == nullptr || ic_index >= caller->call_ic_slot_count) {
        return false;
    }
    void* entry = caller->call_ic_slots[ic_index].mono.entry;
    if (entry == nullptr) {
        return false;
    }
    reinterpret_cast<JitNativeEntry>(entry)(callee);
    return jit_nested_complete_mono_call_emit(caller, callee, dest);
}

extern "C" void* jit_mono_entry_ptr_for_ic(JitExecState* caller, std::uint32_t ic_index, std::uint32_t callee_index) {
    if (caller == nullptr || caller->call_ic_slots == nullptr || ic_index >= caller->call_ic_slot_count) {
        return nullptr;
    }
    const JitCallIcSlot& slot = caller->call_ic_slots[ic_index];
    if (slot.mode != JitCallIcMode::Monomorphic || slot.mono.function_index != callee_index) {
        return nullptr;
    }
    return slot.mono.entry;
}

extern "C" bool jit_nested_complete_mono_call(JitExecState* caller, JitExecState* callee, std::uint32_t dest) {
    if (JitIcMetrics* metrics = ic_metrics_for_state(caller)) {
        ++metrics->nested_mono_complete_calls;
    }
    auto rollback_scratch = [&]() {
        if (caller != nullptr && caller->host_vm != nullptr && caller->host_vm->has_nested_jit_scratch_saved()) {
            caller->host_vm->pop_nested_jit_scratch_caller();
        }
    };
    auto fail_nested = [&](bool mark_caller_fallback) {
        if (caller != nullptr && caller->host_vm != nullptr) {
            caller->host_vm->notify_nested_mono_call_failed(callee);
        }
        if (mark_caller_fallback && caller != nullptr) {
            mark_fallback(caller);
        }
        rollback_scratch();
        return false;
    };
    if (caller == nullptr || callee == nullptr) {
        if (caller != nullptr) {
            mark_fallback(caller);
        }
        if (caller != nullptr && caller->host_vm != nullptr) {
            caller->host_vm->notify_nested_mono_call_failed(callee);
        }
        rollback_scratch();
        return false;
    }
    if (callee->deopt_requested) {
        caller->deopt_requested = true;
        return fail_nested(false);
    }
    if (callee->fallback) {
        caller->fallback = true;
        return fail_nested(false);
    }
    if (!callee->completed) {
        return fail_nested(true);
    }
    Value result_slot{};
    if (callee->return_value.kind == ValueKind::Integer) {
        result_slot.kind = ValueKind::Integer;
        result_slot.integer = callee->return_value.integer;
    } else if (callee->return_value.kind == ValueKind::Float) {
        result_slot.kind = ValueKind::Float;
        result_slot.floating = callee->return_value.floating;
    } else {
        result_slot = callee->return_value;
    }
    if (caller->host_vm != nullptr) {
        if (caller->host_vm->has_nested_jit_scratch_saved()) {
            caller->host_vm->commit_nested_mono_result_to_saved_caller(dest, result_slot);
        } else {
            caller->host_vm->publish_nested_mono_call_result(*caller, dest, result_slot);
        }
    } else {
        if (dest >= caller->registers.size()) {
            caller->registers.resize(dest + 1);
            refresh_register_data(*caller);
        }
        caller->registers[dest] = std::move(result_slot);
        refresh_register_data(*caller);
    }
    if (JitIcMetrics* metrics = ic_metrics_for_state(caller)) {
        ++metrics->nested_jit_direct_entry;
    }
    return true;
}

extern "C" bool jit_nested_complete_mono_call_emit(JitExecState* caller, JitExecState* callee, std::uint32_t dest) {
    if (!jit_nested_complete_mono_call(caller, callee, dest)) {
        return false;
    }
    if (JitIcMetrics* metrics = ic_metrics_for_state(caller)) {
        ++metrics->nested_jit_emit_direct;
        ++metrics->emit_mono_invoke_fused;
    }
    return true;
}

static bool try_mono_invoke_from_ic(JitExecState* state, std::uint32_t dest, std::uint32_t first_arg,
                                    std::uint32_t callee_index, std::uint32_t ic_index,
                                    const std::vector<std::shared_ptr<Cell>>* upvalues);

extern "C" void jit_helper_call_slow(JitExecState* state, std::uint32_t dest, std::uint32_t first_arg,
                                     std::uint32_t callee_index, std::uint32_t ic_index) {
    if (!state->host_vm) {
        mark_fallback(state);
        return;
    }
    state->fallback = false;
    state->deopt_requested = false;
    jit::JitCallIcSlot* ic = nullptr;
    if (state->call_ic_slots && ic_index < state->call_ic_slot_count) {
        ic = &state->call_ic_slots[ic_index];
    }
    if (!state->host_vm->jit_invoke_direct_call(*state, dest, first_arg, callee_index, ic)) {
        if (state->ic_metrics != nullptr) {
            ++state->ic_metrics->nested_jit_helper_invoke;
        }
        mark_fallback(state);
    }
}

extern "C" void jit_helper_call(JitExecState* state, std::uint32_t dest, std::uint32_t first_arg,
                                std::uint32_t callee_index, std::uint32_t ic_index) {
    if (state != nullptr && state->host_vm != nullptr) {
        state->fallback = false;
        state->deopt_requested = false;
        if (try_mono_invoke_from_ic(state, dest, first_arg, callee_index, ic_index, nullptr)) {
            return;
        }
    }
    jit_helper_call_slow(state, dest, first_arg, callee_index, ic_index);
}

static bool try_mono_invoke_call_value_from_ic(JitExecState* state, std::uint32_t dest, std::uint32_t callee_reg,
                                               std::uint32_t ic_index) {
    if (state == nullptr || state->call_ic_slots == nullptr || ic_index >= state->call_ic_slot_count ||
        callee_reg >= state->registers.size()) {
        return false;
    }
    JitExecState* callee = jit_nested_begin_mono_call_value(state, callee_reg + 1, callee_reg, ic_index);
    if (callee == nullptr) {
        return false;
    }
    void* entry = state->call_ic_slots[ic_index].mono.entry;
    if (entry == nullptr) {
        return false;
    }
    reinterpret_cast<jit::JitNativeEntry>(entry)(callee);
    if (jit_nested_complete_mono_call(state, callee, dest)) {
        return true;
    }
    state->fallback = false;
    state->deopt_requested = false;
    return false;
}

static bool try_mono_invoke_from_ic(JitExecState* state, std::uint32_t dest, std::uint32_t first_arg,
                                    std::uint32_t callee_index, std::uint32_t ic_index,
                                    const std::vector<std::shared_ptr<Cell>>* upvalues) {
    if (upvalues != nullptr && !upvalues->empty()) {
        return false;
    }
    if (JitExecState* callee = jit_nested_begin_mono_call(state, first_arg, callee_index, ic_index)) {
        void* entry = jit_mono_entry_ptr_for_ic(state, ic_index, callee_index);
        if (entry != nullptr) {
            reinterpret_cast<jit::JitNativeEntry>(entry)(callee);
            if (jit_nested_complete_mono_call(state, callee, dest)) {
                return true;
            }
            state->fallback = false;
            state->deopt_requested = false;
        }
    }
    return false;
}

extern "C" void jit_helper_call_value_slow(JitExecState* state, std::uint32_t dest, std::uint32_t callee_reg,
                                           std::uint32_t argc, std::uint32_t ic_index) {
    if (!state->host_vm) {
        mark_fallback(state);
        return;
    }
    jit::JitCallIcSlot* ic = nullptr;
    if (state->call_ic_slots && ic_index < state->call_ic_slot_count) {
        ic = &state->call_ic_slots[ic_index];
    }
    if (!state->host_vm->jit_execute_call_value(*state, dest, callee_reg, argc, ic)) {
        mark_fallback(state);
    }
}

static bool call_value_mono_eligible(JitExecState* state, std::uint32_t callee_reg, std::uint32_t argc) {
    if (state == nullptr || state->bytecode == nullptr || callee_reg >= state->registers.size()) {
        return false;
    }
    const Value& callee_val = state->registers[callee_reg];
    if (callee_val.kind != ValueKind::Function || !callee_val.function ||
        callee_val.function->module_index != 0xFFFFFFFFu ||
        callee_val.function->function_index >= state->bytecode->functions.size()) {
        return false;
    }
    const std::uint32_t callee_index = callee_val.function->function_index;
    const FunctionInfo& callee_fn = state->bytecode->functions[callee_index];
    if (!callee_fn.captures.empty() && callee_fn.captures.size() > 1) {
        return false;
    }
    if (!callee_val.function->captures.empty() && callee_val.function->captures.size() > 1) {
        return false;
    }
    return argc == callee_fn.arity && !callee_fn.is_native && !callee_fn.is_async && !callee_fn.is_constructor;
}

extern "C" bool jit_call_value_mono_emit_guard(JitExecState* state, std::uint32_t callee_reg, std::uint32_t argc) {
    if (state != nullptr && state->ic_metrics != nullptr) {
        ++state->ic_metrics->call_value_emit_guard_calls;
    }
    return call_value_mono_eligible(state, callee_reg, argc);
}

extern "C" void jit_helper_call_value(JitExecState* state, std::uint32_t dest, std::uint32_t callee_reg,
                                      std::uint32_t argc, std::uint32_t ic_index) {
    if (!state->host_vm || callee_reg >= state->registers.size()) {
        mark_fallback(state);
        return;
    }
    state->fallback = false;
    state->deopt_requested = false;
    jit_helper_call_value_slow(state, dest, callee_reg, argc, ic_index);
}

extern "C" void jit_helper_guard_field_object(JitExecState* state, std::uint32_t reg_index, std::uint32_t site_id) {
    if (reg_index >= state->registers.size()) {
        mark_fallback(state);
        return;
    }
    const Value value = read_reg(state, reg_index);
    if (value.kind != ValueKind::Struct && value.kind != ValueKind::Class) {
        if (state->ic_metrics) {
            ++state->ic_metrics->specialization_guard_failures;
        }
        jit_request_deopt(state, site_id);
    }
}

extern "C" void jit_helper_guard_value_kind(JitExecState* state, std::uint32_t reg_index, std::uint32_t kind,
                                            std::uint32_t site_id) {
    if (reg_index >= state->registers.size()) {
        mark_fallback(state);
        return;
    }
    const Value value = read_reg(state, reg_index);
    if (static_cast<std::uint32_t>(value.kind) != kind) {
        if (state->ic_metrics) {
            ++state->ic_metrics->specialization_guard_failures;
        }
        jit_request_deopt(state, site_id);
        return;
    }
}

extern "C" void jit_helper_list_get_index(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                          std::uint32_t index_reg) {
    try {
        const Value& object_value = read_reg(state, object);
        const Value& index_value = read_reg(state, index_reg);
        if (object_value.kind != ValueKind::List || !object_value.list) {
            mark_fallback(state);
            return;
        }
        if (index_value.kind != ValueKind::Integer) {
            mark_fallback(state);
            return;
        }
        if (index_value.integer < 0 ||
            static_cast<std::uint64_t>(index_value.integer) >= object_value.list->items.size()) {
            mark_fallback(state);
            return;
        }
        reg(state, dest) = object_value.list->items[static_cast<std::size_t>(index_value.integer)];
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_tuple_get_index(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                           std::uint32_t index_reg) {
    try {
        const Value& object_value = read_reg(state, object);
        const Value& index_value = read_reg(state, index_reg);
        if (object_value.kind != ValueKind::Tuple || !object_value.tuple) {
            mark_fallback(state);
            return;
        }
        if (index_value.kind != ValueKind::Integer) {
            mark_fallback(state);
            return;
        }
        if (index_value.integer < 0 ||
            static_cast<std::uint64_t>(index_value.integer) >= object_value.tuple->items.size()) {
            mark_fallback(state);
            return;
        }
        reg(state, dest) = object_value.tuple->items[static_cast<std::size_t>(index_value.integer)];
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_bytes_get_index(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                           std::uint32_t index_reg) {
    try {
        const Value& object_value = read_reg(state, object);
        const Value& index_value = read_reg(state, index_reg);
        if (object_value.kind != ValueKind::Bytes || !object_value.bytes) {
            mark_fallback(state);
            return;
        }
        if (index_value.kind != ValueKind::Integer) {
            mark_fallback(state);
            return;
        }
        if (index_value.integer < 0 ||
            static_cast<std::uint64_t>(index_value.integer) >= object_value.bytes->data.size()) {
            mark_fallback(state);
            return;
        }
        reg(state, dest) = Runtime::make_integer(
            static_cast<std::int64_t>(object_value.bytes->data[static_cast<std::size_t>(index_value.integer)]));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_dict_get_string_key(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                               std::uint32_t key_reg) {
    try {
        const Value& object_value = read_reg(state, object);
        const Value& key_value = read_reg(state, key_reg);
        if (object_value.kind != ValueKind::Dict || !object_value.dict) {
            mark_fallback(state);
            return;
        }
        if (key_value.kind != ValueKind::String) {
            mark_fallback(state);
            return;
        }
        const Value* found = Runtime::dict_lookup(*object_value.dict, key_value.string);
        if (!found) {
            mark_fallback(state);
            return;
        }
        reg(state, dest) = *found;
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_list_get_int_element(JitExecState* state, std::uint32_t dest, std::uint32_t object,
                                                std::uint32_t index_reg) {
    const Value& object_value = read_reg(state, object);
    const Value& index_value = read_reg(state, index_reg);
    if (object_value.kind != ValueKind::List || !object_value.list) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    if (index_value.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    if (index_value.integer < 0 ||
        static_cast<std::uint64_t>(index_value.integer) >= object_value.list->items.size()) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    const Value& element =
        object_value.list->items[static_cast<std::size_t>(index_value.integer)];
    if (element.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_integer(element.integer);
}

extern "C" void jit_helper_set_index(JitExecState* state, std::uint32_t object, std::uint32_t index,
                                     std::uint32_t value) {
    try {
        Runtime::set_index(read_reg(state, object), read_reg(state, index), read_reg(state, value));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_list_set_index(JitExecState* state, std::uint32_t object, std::uint32_t index,
                                          std::uint32_t value) {
    try {
        const Value& object_value = read_reg(state, object);
        const Value& index_value = read_reg(state, index);
        const Value& new_value = read_reg(state, value);
        if (object_value.kind != ValueKind::List || !object_value.list) {
            mark_fallback(state);
            return;
        }
        if (index_value.kind != ValueKind::Integer) {
            mark_fallback(state);
            return;
        }
        if (index_value.integer < 0 ||
            static_cast<std::uint64_t>(index_value.integer) >= object_value.list->items.size()) {
            mark_fallback(state);
            return;
        }
        object_value.list->items[static_cast<std::size_t>(index_value.integer)] = new_value;
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_make_list(JitExecState* state, std::uint32_t dest, std::uint32_t first_reg,
                                     std::uint32_t count) {
    try {
        std::vector<Value> items;
        items.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            items.push_back(read_reg(state, first_reg + i));
        }
        reg(state, dest) = Runtime::make_list(std::move(items));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_make_dict(JitExecState* state, std::uint32_t dest, std::uint32_t first_pair_reg,
                                     std::uint32_t count) {
    try {
        std::vector<std::pair<std::string, Value>> entries;
        entries.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const Value& key = read_reg(state, first_pair_reg + i * 2);
            if (key.kind != ValueKind::String) {
                mark_fallback(state);
                return;
            }
            entries.emplace_back(key.string, read_reg(state, first_pair_reg + i * 2 + 1));
        }
        reg(state, dest) = Runtime::make_dict(std::move(entries));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_make_tuple(JitExecState* state, std::uint32_t dest, std::uint32_t first_reg,
                                      std::uint32_t count) {
    try {
        std::vector<Value> items;
        items.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            items.push_back(read_reg(state, first_reg + i));
        }
        reg(state, dest) = Runtime::make_tuple(std::move(items));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_make_set(JitExecState* state, std::uint32_t dest, std::uint32_t first_reg,
                                    std::uint32_t count) {
    try {
        std::vector<Value> items;
        items.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            items.push_back(read_reg(state, first_reg + i));
        }
        reg(state, dest) = Runtime::make_set(std::move(items));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_string_equal(JitExecState* state, std::uint32_t dest, std::uint32_t left,
                                      std::uint32_t right) {
    if (left == right) {
        reg(state, dest) = Runtime::make_bool(true);
        return;
    }
    const Value& l = read_reg(state, left);
    const Value& r = read_reg(state, right);
    if (l.kind != ValueKind::String || r.kind != ValueKind::String) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_bool(Runtime::strings_equal(l.string, r.string));
}

extern "C" void jit_helper_string_not_equal(JitExecState* state, std::uint32_t dest, std::uint32_t left,
                                            std::uint32_t right) {
    if (left == right) {
        reg(state, dest) = Runtime::make_bool(false);
        return;
    }
    const Value& l = read_reg(state, left);
    const Value& r = read_reg(state, right);
    if (l.kind != ValueKind::String || r.kind != ValueKind::String) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_bool(!Runtime::strings_equal(l.string, r.string));
}

extern "C" void jit_helper_string_concat(JitExecState* state, std::uint32_t dest, std::uint32_t left,
                                         std::uint32_t right) {
    const Value& l = read_reg(state, left);
    const Value& r = read_reg(state, right);
    if (l.kind != ValueKind::String || r.kind != ValueKind::String) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::concat_strings(l.string, r.string);
}

extern "C" void jit_helper_int_equal(JitExecState* state, std::uint32_t dest, std::uint32_t left,
                                     std::uint32_t right) {
    const Value l = read_reg(state, left);
    const Value r = read_reg(state, right);
    if (l.kind != ValueKind::Integer || r.kind != ValueKind::Integer) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_bool(l.integer == r.integer);
}

namespace {

bool read_float_reg(JitExecState* state, std::uint32_t reg_index, double& out) {
    const Value& value = read_reg(state, reg_index);
    if (value.kind == ValueKind::Float) {
        out = value.floating;
        return true;
    }
    if (value.kind == ValueKind::Integer) {
        out = static_cast<double>(value.integer);
        return true;
    }
    return false;
}

}  // namespace

extern "C" void jit_helper_float_add(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    double l = 0.0;
    double r = 0.0;
    if (!read_float_reg(state, left, l) || !read_float_reg(state, right, r)) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_float(l + r);
}

extern "C" void jit_helper_float_sub(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    double l = 0.0;
    double r = 0.0;
    if (!read_float_reg(state, left, l) || !read_float_reg(state, right, r)) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_float(l - r);
}

extern "C" void jit_helper_float_mul(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    double l = 0.0;
    double r = 0.0;
    if (!read_float_reg(state, left, l) || !read_float_reg(state, right, r)) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_float(l * r);
}

extern "C" void jit_helper_float_div(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    double l = 0.0;
    double r = 0.0;
    if (!read_float_reg(state, left, l) || !read_float_reg(state, right, r)) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    if (r == 0.0) {
        state->fallback = true;
        return;
    }
    reg(state, dest) = Runtime::make_float(l / r);
}

extern "C" void jit_helper_float_less(JitExecState* state, std::uint32_t dest, std::uint32_t left, std::uint32_t right) {
    double l = 0.0;
    double r = 0.0;
    if (!read_float_reg(state, left, l) || !read_float_reg(state, right, r)) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_bool(l < r);
}

extern "C" void jit_helper_float_equal(JitExecState* state, std::uint32_t dest, std::uint32_t left,
                                       std::uint32_t right) {
    double l = 0.0;
    double r = 0.0;
    if (!read_float_reg(state, left, l) || !read_float_reg(state, right, r)) {
        jit_request_deopt_at_ir_index(state, state->current_insn);
        return;
    }
    reg(state, dest) = Runtime::make_bool(l == r);
}

extern "C" void jit_helper_make_struct(JitExecState* state, std::uint32_t dest, std::uint32_t struct_index) {
    if (!state->bytecode || struct_index >= state->bytecode->structs.size()) {
        mark_fallback(state);
        return;
    }
    try {
        const StructInfo& info = state->bytecode->structs[struct_index];
        std::vector<std::string> field_names;
        field_names.reserve(info.fields.size());
        for (const StructFieldInfo& field : info.fields) {
            field_names.push_back(field.name);
        }
        reg(state, dest) = Runtime::make_struct(struct_index, info.name, std::move(field_names));
    } catch (...) {
        mark_fallback(state);
    }
}

extern "C" void jit_helper_set_field(JitExecState* state, std::uint32_t object, std::uint32_t field_index,
                                     std::uint32_t value_reg) {
    try {
        const Value field_value = read_reg(state, value_reg);
        Value& object_value = reg(state, object);
        Runtime::set_field(object_value, field_index, field_value);
    } catch (...) {
        mark_fallback(state);
    }
}

}  // namespace pscript::jit
