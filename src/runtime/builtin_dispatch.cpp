#include "runtime/builtin_id.h"
#include "runtime/runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace pscript {
namespace {

double numeric_to_double(const Value& value) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    if (value.kind == ValueKind::Float) {
        return value.floating;
    }
    throw RuntimeError("numeric function expects a numeric argument", RuntimeErrorCategory::Type);
}

bool all_integer(const Value* args, std::uint32_t argc) {
    for (std::uint32_t i = 0; i < argc; ++i) {
        if (args[i].kind != ValueKind::Integer) {
            return false;
        }
    }
    return true;
}

Value numeric_from_double(double value, bool prefer_int) {
    if (prefer_int && std::floor(value) == value &&
        value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        value <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return Runtime::make_integer(static_cast<std::int64_t>(value));
    }
    return Runtime::make_float(value);
}

void expect_argc(std::string_view name, std::uint32_t argc, std::uint32_t expected) {
    if (argc != expected) {
        throw RuntimeError(std::string(name) + " expects " + std::to_string(expected) + " arguments");
    }
}

std::vector<Value> args_to_vector(const Value* args, std::uint32_t argc) {
    return std::vector<Value>(args, args + argc);
}

}  // namespace

BuiltinId builtin_id_from_name(std::string_view name) {
    if (name == "print") {
        return BuiltinId::Print;
    }
    if (name == "range") {
        return BuiltinId::Range;
    }
    if (name == "input") {
        return BuiltinId::Input;
    }
    if (name == "async_input") {
        return BuiltinId::AsyncInput;
    }
    if (name == "int") {
        return BuiltinId::Int;
    }
    if (name == "float") {
        return BuiltinId::Float;
    }
    if (name == "str") {
        return BuiltinId::Str;
    }
    if (name == "bool") {
        return BuiltinId::Bool;
    }
    if (name == "type") {
        return BuiltinId::Type;
    }
    if (name == "type_of") {
        return BuiltinId::TypeOf;
    }
    if (name == "annotations") {
        return BuiltinId::Annotations;
    }
    if (name == "members") {
        return BuiltinId::Members;
    }
    if (name == "delay") {
        return BuiltinId::Delay;
    }
    if (name == "race") {
        return BuiltinId::Race;
    }
    if (name == "timeout") {
        return BuiltinId::Timeout;
    }
    if (name == "wait_all" || name == "all") {
        return BuiltinId::WaitAll;
    }
    if (name == "wait_any") {
        return BuiltinId::WaitAny;
    }
    if (name == "abs") {
        return BuiltinId::Abs;
    }
    if (name == "min") {
        return BuiltinId::Min;
    }
    if (name == "max") {
        return BuiltinId::Max;
    }
    if (name == "round") {
        return BuiltinId::Round;
    }
    if (name == "clamp") {
        return BuiltinId::Clamp;
    }
    return BuiltinId::Unknown;
}

bool dispatch_runtime_builtin(Runtime& runtime, BuiltinId id, const Value* args, std::uint32_t argc, Value& out) {
    switch (id) {
        case BuiltinId::Print:
            out = runtime.print(args_to_vector(args, argc));
            return true;
        case BuiltinId::Range:
            out = Runtime::builtin_range(runtime, args_to_vector(args, argc));
            return true;
        case BuiltinId::Input:
            out = Runtime::builtin_input(runtime, args_to_vector(args, argc));
            return true;
        case BuiltinId::Int:
            expect_argc("int", argc, 1);
            out = Runtime::convert_int(args[0]);
            return true;
        case BuiltinId::Float:
            expect_argc("float", argc, 1);
            out = Runtime::convert_float(args[0]);
            return true;
        case BuiltinId::Str:
            expect_argc("str", argc, 1);
            out = Runtime::make_string(Runtime::format(args[0]));
            return true;
        case BuiltinId::Bool:
            expect_argc("bool", argc, 1);
            out = Runtime::convert_bool(args[0]);
            return true;
        case BuiltinId::Type:
            expect_argc("type", argc, 1);
            out = Runtime::make_string(Runtime::runtime_type_name(args[0]));
            return true;
        case BuiltinId::Abs:
            expect_argc("abs", argc, 1);
            if (args[0].kind == ValueKind::Integer) {
                const std::int64_t value = args[0].integer;
                out = Runtime::make_integer(value < 0 ? -value : value);
                return true;
            }
            if (args[0].kind == ValueKind::Float) {
                out = Runtime::make_float(std::fabs(args[0].floating));
                return true;
            }
            throw RuntimeError("'abs' expects a numeric argument", RuntimeErrorCategory::Type);
        case BuiltinId::Min:
            if (argc < 2) {
                throw RuntimeError("'min' expects at least 2 arguments");
            }
            {
                double best = numeric_to_double(args[0]);
                for (std::uint32_t i = 1; i < argc; ++i) {
                    best = std::min(best, numeric_to_double(args[i]));
                }
                out = numeric_from_double(best, all_integer(args, argc));
            }
            return true;
        case BuiltinId::Max:
            if (argc < 2) {
                throw RuntimeError("'max' expects at least 2 arguments");
            }
            {
                double best = numeric_to_double(args[0]);
                for (std::uint32_t i = 1; i < argc; ++i) {
                    best = std::max(best, numeric_to_double(args[i]));
                }
                out = numeric_from_double(best, all_integer(args, argc));
            }
            return true;
        case BuiltinId::Round:
            expect_argc("round", argc, 1);
            if (args[0].kind == ValueKind::Integer) {
                out = Runtime::make_integer(args[0].integer);
                return true;
            }
            if (args[0].kind == ValueKind::Float) {
                const double value = args[0].floating;
                const double rounded = value >= 0.0 ? std::floor(value + 0.5) : std::ceil(value - 0.5);
                out = numeric_from_double(rounded, false);
                return true;
            }
            throw RuntimeError("'round' expects a numeric argument", RuntimeErrorCategory::Type);
        case BuiltinId::Clamp:
            expect_argc("clamp", argc, 3);
            {
                const double value = numeric_to_double(args[0]);
                const double lower = numeric_to_double(args[1]);
                const double upper = numeric_to_double(args[2]);
                if (lower > upper) {
                    throw RuntimeError("clamp bounds are invalid", RuntimeErrorCategory::Value);
                }
                const double clamped = std::min(upper, std::max(lower, value));
                out = numeric_from_double(clamped, all_integer(args, argc));
            }
            return true;
        case BuiltinId::AsyncInput:
        case BuiltinId::TypeOf:
        case BuiltinId::Annotations:
        case BuiltinId::Members:
        case BuiltinId::Delay:
        case BuiltinId::Race:
        case BuiltinId::Timeout:
        case BuiltinId::WaitAll:
        case BuiltinId::WaitAny:
        case BuiltinId::Unknown:
            return false;
    }
    return false;
}

}  // namespace pscript
