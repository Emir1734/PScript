#pragma once

#include <cstdint>
#include <string_view>

namespace pscript {

struct Value;
class Runtime;

enum class BuiltinId : std::uint8_t {
    Unknown = 0,
    Print,
    Range,
    Input,
    AsyncInput,
    Int,
    Float,
    Str,
    Bool,
    Type,
    TypeOf,
    Annotations,
    Members,
    Delay,
    Race,
    Timeout,
    WaitAll,
    WaitAny,
    Abs,
    Min,
    Max,
    Round,
    Clamp,
};

BuiltinId builtin_id_from_name(std::string_view name);

// Dispatches runtime builtins handled entirely in Runtime (not VM task/async specials).
// Returns true if dispatched; false if id requires VM-specific handling or is Unknown.
bool dispatch_runtime_builtin(Runtime& runtime, BuiltinId id, const Value* args, std::uint32_t argc, Value& out);

}  // namespace pscript
