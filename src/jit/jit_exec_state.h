#pragma once

#include "compiler/bytecode.h"
#include "runtime/runtime.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pscript {

class Vm;

namespace jit {

struct JitCallIcSlot;
struct JitIcMetrics;

// POD header fields must stay at the front: the x64 backend uses offsetof on these members.
struct JitDeoptContext;

struct JitExecState {
    // Byte-sized flags: native JIT stores via fixed offsets; volatile avoids MSVC /Od
    // caching stale values across the opaque JitNativeEntry call in Debug builds.
    volatile std::uint8_t completed = 0;
    volatile std::uint8_t fallback = 0;
    volatile std::uint8_t deopt_requested = 0;
    std::uint32_t current_insn = 0;
    std::uint32_t instruction_count = 0;
    std::uint32_t function_index = 0;
    std::uint32_t current_block = 0;
    std::uint32_t deopt_resume_bytecode_ip = 0;
    std::uint32_t deopt_site_id = 0;
    std::uint32_t deopt_ir_insn_index = 0;
    Vm* host_vm = nullptr;
    Runtime* runtime = nullptr;
    const Bytecode* bytecode = nullptr;
    const JitDeoptContext* deopt_context = nullptr;
    // Must point at registers.data() for the duration of native JIT execution (updated on register resize).
    Value* register_data = nullptr;
    std::vector<Value> registers;
    std::vector<std::shared_ptr<Cell>> locals;
    std::vector<std::shared_ptr<Cell>> upvalues;
    Value return_value;
    std::uint32_t jit_call_depth = 0;
    JitCallIcSlot* call_ic_slots = nullptr;
    std::uint32_t call_ic_slot_count = 0;
    JitIcMetrics* ic_metrics = nullptr;
    const std::int32_t* integer_shadow_map = nullptr;
    std::uint32_t integer_shadow_count = 0;
    // When true, prepare_exec_state_for_compiled_entry keeps register file values (VM populate path).
    bool preserve_registers_on_entry = false;
    // When false, native_jit_completions is not incremented (nested function JIT entries).
    bool count_native_completion = false;
    // On-stack replacement: preserve current_insn and hydrate shadows from live locals.
    bool osr_entry = false;
};

static_assert(offsetof(JitExecState, completed) == 0);
static_assert(offsetof(JitExecState, fallback) == 1);
static_assert(offsetof(JitExecState, deopt_requested) == 2);
static_assert(offsetof(JitExecState, current_insn) == 4);

inline void refresh_register_data(JitExecState& state) {
    state.register_data = state.registers.empty() ? nullptr : state.registers.data();
}

}  // namespace jit
}  // namespace pscript
