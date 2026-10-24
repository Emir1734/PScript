#include "jit/jit_ic.h"

#include "jit/jit_code.h"
#include "jit/jit_deopt.h"

#include <algorithm>
#include <sstream>

namespace pscript::jit {

namespace {

JitCallIcEntry make_entry(std::uint32_t callee_index, const JitCompiledCode* code, const void* closure_token,
                          const Bytecode* bytecode) {
    JitCallIcEntry entry;
    entry.function_index = callee_index;
    entry.entry = code ? reinterpret_cast<void*>(code->entry) : nullptr;
    entry.closure_token = closure_token;
    if (code && code->metadata) {
        entry.code_generation = code->metadata->optimization_generation;
    }
    if (code && bytecode && callee_index < bytecode->functions.size()) {
        const FunctionInfo& fn = bytecode->functions[callee_index];
        entry.callee_arity = static_cast<std::uint8_t>(fn.arity);
        entry.callee_register_count =
            std::max(fn.register_count, code->register_count > 0 ? code->register_count : fn.register_count);
        entry.callee_local_count = fn.local_count;
        entry.callee_ir_instruction_count =
            code->ir_instruction_count > 0 ? code->ir_instruction_count : static_cast<std::uint32_t>(0);
        entry.callee_deopt_context = code->metadata ? &code->metadata->deopt : nullptr;
        if (!code->call_ic_slots.empty()) {
            entry.callee_call_ic_slots = static_cast<const void*>(code->call_ic_slots.data());
            entry.callee_call_ic_slot_count = static_cast<std::uint32_t>(code->call_ic_slots.size());
        }
    }
    return entry;
}

bool poly_contains(const JitCallIcSlot& slot, const JitCallIcEntry& entry) {
    for (std::size_t i = 0; i < slot.poly_count; ++i) {
        const JitCallIcEntry& existing = slot.poly[i];
        if (existing.function_index == entry.function_index && existing.entry == entry.entry &&
            existing.closure_token == entry.closure_token &&
            existing.code_generation == entry.code_generation) {
            return true;
        }
    }
    return false;
}

bool entries_equal(const JitCallIcEntry& a, const JitCallIcEntry& b) {
    return a.function_index == b.function_index && a.entry == b.entry && a.closure_token == b.closure_token &&
           a.code_generation == b.code_generation;
}

}  // namespace

bool g_ic_timing_enabled = false;

void set_ic_timing_enabled(bool enabled) {
    g_ic_timing_enabled = enabled;
}

bool ic_timing_enabled() {
    return g_ic_timing_enabled;
}

void reset_ic_metrics(JitIcMetrics& metrics) {
    metrics = JitIcMetrics{};
}

std::string format_ic_metrics(const JitIcMetrics& metrics) {
    std::ostringstream out;
    out << "call_mono_hits=" << metrics.call_mono_hits << " call_poly_hits=" << metrics.call_poly_hits
        << " call_megamorphic=" << metrics.call_megamorphic << " call_misses=" << metrics.call_misses
        << " nested_jit_direct_entry=" << metrics.nested_jit_direct_entry
        << " nested_jit_helper_invoke=" << metrics.nested_jit_helper_invoke
        << " nested_jit_emit_direct=" << metrics.nested_jit_emit_direct
        << " prepare_nested_mono_full=" << metrics.prepare_nested_mono_full
        << " prepare_nested_mono_fast=" << metrics.prepare_nested_mono_fast
        << " prepare_jit_callee_locals_calls=" << metrics.prepare_jit_callee_locals_calls
        << " mirror_jit_callee_args_calls=" << metrics.mirror_jit_callee_args_calls
        << " nested_mono_complete_calls=" << metrics.nested_mono_complete_calls
        << " call_value_emit_guard_calls=" << metrics.call_value_emit_guard_calls
        << " nested_begin_mono_call_value=" << metrics.nested_begin_mono_call_value
        << " emit_mono_invoke_fused=" << metrics.emit_mono_invoke_fused
        << " guard_failures=" << metrics.specialization_guard_failures;
    if (metrics.prepare_nested_mono_ns > 0) {
        out << " prepare_nested_mono_ns=" << metrics.prepare_nested_mono_ns;
    }
    return out.str();
}

void record_call_ic_hit(JitCallIcSlot* slot, JitIcMetrics* metrics, bool mono, bool poly) {
    if (metrics) {
        if (mono) {
            ++metrics->call_mono_hits;
        } else if (poly) {
            ++metrics->call_poly_hits;
        }
    }
    (void)slot;
}

void record_call_ic_miss(JitCallIcSlot* slot, JitIcMetrics* metrics) {
    if (metrics) {
        ++metrics->call_misses;
    }
    (void)slot;
}

void update_call_ic(JitCallIcSlot* slot, std::uint32_t callee_index, const JitCompiledCode* code,
                    const void* closure_token, const Bytecode* bytecode) {
    if (!slot || !code || !code->entry) {
        return;
    }
    const JitCallIcEntry entry = make_entry(callee_index, code, closure_token, bytecode);
    switch (slot->mode) {
        case JitCallIcMode::Empty:
            slot->mono = entry;
            slot->mode = JitCallIcMode::Monomorphic;
            break;
        case JitCallIcMode::Monomorphic:
            if (entries_equal(slot->mono, entry)) {
                slot->mono = entry;
                break;
            }
            slot->poly[0] = slot->mono;
            slot->poly[1] = entry;
            slot->poly_count = 2;
            slot->mode = JitCallIcMode::Polymorphic;
            break;
        case JitCallIcMode::Polymorphic: {
            if (poly_contains(*slot, entry)) {
                return;
            }
            if (slot->poly_count < JitCallIcSlot::kPolyCapacity) {
                slot->poly[slot->poly_count++] = entry;
                return;
            }
            slot->mode = JitCallIcMode::Megamorphic;
            slot->poly_count = 0;
            break;
        }
        case JitCallIcMode::Megamorphic:
            break;
    }
}

void hydrate_call_ic_entry(JitCallIcEntry& entry, std::uint32_t callee_index, const JitCompiledCode* code,
                           const void* closure_token, const Bytecode* bytecode) {
    if (!code || !code->entry || entry.entry != reinterpret_cast<void*>(code->entry)) {
        return;
    }
    const JitCallIcEntry fresh = make_entry(callee_index, code, closure_token, bytecode);
    entry.callee_arity = fresh.callee_arity;
    entry.callee_register_count = fresh.callee_register_count;
    entry.callee_local_count = fresh.callee_local_count;
    entry.callee_ir_instruction_count = fresh.callee_ir_instruction_count;
    entry.callee_deopt_context = fresh.callee_deopt_context;
    entry.callee_call_ic_slots = fresh.callee_call_ic_slots;
    entry.callee_call_ic_slot_count = fresh.callee_call_ic_slot_count;
    entry.code_generation = fresh.code_generation;
}

}  // namespace pscript::jit
