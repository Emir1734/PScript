#pragma once

#include "runtime/runtime.h"

#include <cassert>
#include <cstdint>

namespace pscript::runtime_debug {

inline bool value_kind_is_valid(ValueKind kind) {
    switch (kind) {
        case ValueKind::Integer:
        case ValueKind::Float:
        case ValueKind::String:
        case ValueKind::Bool:
        case ValueKind::Null:
        case ValueKind::Bytes:
        case ValueKind::List:
        case ValueKind::Dict:
        case ValueKind::Tuple:
        case ValueKind::Set:
        case ValueKind::Struct:
        case ValueKind::Class:
        case ValueKind::Enum:
        case ValueKind::Range:
        case ValueKind::Iterator:
        case ValueKind::Function:
        case ValueKind::Module:
        case ValueKind::Task:
            return true;
    }
    return false;
}

inline void assert_valid_value(const Value& value) {
#ifndef NDEBUG
    assert(value_kind_is_valid(value.kind));
    if (value.kind == ValueKind::List) {
        assert(value.list != nullptr);
    }
    if (value.kind == ValueKind::Function) {
        assert(value.function != nullptr);
    }
#endif
}

}  // namespace pscript::runtime_debug
