#pragma once

#include "compiler/bytecode.h"

#include <array>
#include <cstdint>

namespace pscript::jit {

struct JitCompiledCode;
struct JitExecState;

struct JitIcMetrics {
    std::uint64_t call_mono_hits = 0;
    std::uint64_t call_poly_hits = 0;
    std::uint64_t call_megamorphic = 0;
    std::uint64_t call_misses = 0;
    std::uint64_t specialization_guard_failures = 0;
    std::uint64_t nested_jit_direct_entry = 0;
    std::uint64_t nested_jit_helper_invoke = 0;
    std::uint64_t call_inline_plus_one = 0;
    std::uint64_t nested_jit_emit_direct = 0;
    std::uint64_t prepare_nested_mono_full = 0;
    std::uint64_t prepare_nested_mono_fast = 0;
    std::uint64_t prepare_jit_callee_locals_calls = 0;
    std::uint64_t mirror_jit_callee_args_calls = 0;
    std::uint64_t nested_mono_complete_calls = 0;
    std::uint64_t call_value_emit_guard_calls = 0;
    std::uint64_t nested_begin_mono_call_value = 0;
    std::uint64_t emit_mono_invoke_fused = 0;
    std::uint64_t prepare_nested_mono_ns = 0;
};

JitIcMetrics* ic_metrics_for_state(JitExecState* state);

void reset_ic_metrics(JitIcMetrics& metrics);
std::string format_ic_metrics(const JitIcMetrics& metrics);

void set_ic_timing_enabled(bool enabled);
bool ic_timing_enabled();

struct JitDeoptContext;

struct JitCallIcEntry {
    std::uint32_t function_index = 0xFFFFFFFFu;
    void* entry = nullptr;
    const void* closure_token = nullptr;
    std::uint32_t code_generation = 0;
    std::uint8_t callee_arity = 0;
    std::uint32_t callee_register_count = 0;
    std::uint32_t callee_local_count = 0;
    std::uint32_t callee_ir_instruction_count = 0;
    const JitDeoptContext* callee_deopt_context = nullptr;
    const void* callee_call_ic_slots = nullptr;
    std::uint32_t callee_call_ic_slot_count = 0;
};

enum class JitCallIcMode : std::uint8_t {
    Empty = 0,
    Monomorphic = 1,
    Polymorphic = 2,
    Megamorphic = 3,
};

struct JitCallIcSlot {
    JitCallIcMode mode = JitCallIcMode::Empty;
    JitCallIcEntry mono;
    static constexpr std::size_t kPolyCapacity = 4;
    std::array<JitCallIcEntry, kPolyCapacity> poly{};
    std::size_t poly_count = 0;
};

void record_call_ic_hit(JitCallIcSlot* slot, JitIcMetrics* metrics, bool mono, bool poly);
void record_call_ic_miss(JitCallIcSlot* slot, JitIcMetrics* metrics);
void update_call_ic(JitCallIcSlot* slot, std::uint32_t callee_index, const JitCompiledCode* code,
                    const void* closure_token, const Bytecode* bytecode);
void hydrate_call_ic_entry(JitCallIcEntry& entry, std::uint32_t callee_index, const JitCompiledCode* code,
                           const void* closure_token, const Bytecode* bytecode);

}  // namespace pscript::jit
