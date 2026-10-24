#pragma once

#include "jit/jit_exec_state.h"
#include "jit/jit_ic.h"

#include <cstddef>
#include <cstdint>

namespace pscript::jit {

struct JitCallEmitLayout {
    static constexpr std::uint32_t kOffCallIcSlotsVec = static_cast<std::uint32_t>(offsetof(JitExecState, call_ic_slots));
    static constexpr std::uint32_t kOffCallIcSlotCount =
        static_cast<std::uint32_t>(offsetof(JitExecState, call_ic_slot_count));
    static constexpr std::uint32_t kOffRegistersVec = static_cast<std::uint32_t>(offsetof(JitExecState, registers));
    static constexpr std::uint32_t kOffReturnValue = static_cast<std::uint32_t>(offsetof(JitExecState, return_value));
    static constexpr std::uint32_t kSlotOffMode = static_cast<std::uint32_t>(offsetof(JitCallIcSlot, mode));
    static constexpr std::uint32_t kSlotOffMono = static_cast<std::uint32_t>(offsetof(JitCallIcSlot, mono));
    static constexpr std::uint32_t kMonoOffFunctionIndex =
        static_cast<std::uint32_t>(offsetof(JitCallIcEntry, function_index));
    static constexpr std::uint32_t kMonoOffClosureToken =
        static_cast<std::uint32_t>(offsetof(JitCallIcEntry, closure_token));
    static constexpr std::uint32_t kMonoOffEntry = static_cast<std::uint32_t>(offsetof(JitCallIcEntry, entry));
    static constexpr std::uint32_t kMonoOffCalleeRegCount =
        static_cast<std::uint32_t>(offsetof(JitCallIcEntry, callee_register_count));
    static constexpr std::uint32_t kMonoOffCalleeArity =
        static_cast<std::uint32_t>(offsetof(JitCallIcEntry, callee_arity));
    static constexpr std::uint32_t kIcModeMonomorphic = static_cast<std::uint32_t>(JitCallIcMode::Monomorphic);
    static constexpr std::uint32_t kValueSize = static_cast<std::uint32_t>(sizeof(Value));
    static constexpr std::uint32_t kOffValueKind = static_cast<std::uint32_t>(offsetof(Value, kind));
    static constexpr std::uint32_t kOffValueInteger = static_cast<std::uint32_t>(offsetof(Value, integer));
    static constexpr std::uint32_t kOffValueFloat = static_cast<std::uint32_t>(offsetof(Value, floating));
    static constexpr std::uint32_t kKindInteger = static_cast<std::uint32_t>(ValueKind::Integer);
    static constexpr std::uint32_t kKindFloat = static_cast<std::uint32_t>(ValueKind::Float);
    static constexpr std::uint32_t kSlotStride = static_cast<std::uint32_t>(sizeof(JitCallIcSlot));
};

}  // namespace pscript::jit
