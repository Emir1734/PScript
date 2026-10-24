#pragma once

#include "compiler/bytecode.h"
#include "runtime/runtime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace pscript {

struct StackTraceFrame {
    std::string function_name;
    std::string file;
    int line = 0;
    int column = 0;
};

struct BuiltinExceptionClasses {
    std::uint32_t exception = 0xFFFFFFFFu;
    std::uint32_t runtime = 0xFFFFFFFFu;
    std::uint32_t type = 0xFFFFFFFFu;
    std::uint32_t name = 0xFFFFFFFFu;
    std::uint32_t index = 0xFFFFFFFFu;
    std::uint32_t value = 0xFFFFFFFFu;
    std::uint32_t cancellation = 0xFFFFFFFFu;
    std::uint32_t timeout = 0xFFFFFFFFu;
    std::uint32_t stop_iteration = 0xFFFFFFFFu;
    std::uint32_t network = 0xFFFFFFFFu;
};

struct VmCallFrameInfo {
    std::size_t return_ip = 0;
    std::uint32_t function_index = 0xFFFFFFFFu;
    const Bytecode* function_bytecode = nullptr;
    const Bytecode* caller_bytecode = nullptr;
};

BuiltinExceptionClasses resolve_builtin_exceptions(const Bytecode& bytecode);
std::uint32_t exception_class_for_category(RuntimeErrorCategory category, const BuiltinExceptionClasses& classes);
SourceLocation source_location_at(const Bytecode& bytecode, std::size_t ip);
std::string function_name_at(const Bytecode& bytecode, std::uint32_t function_index);
StackTraceFrame make_stack_frame(const Bytecode& bytecode, std::size_t ip, std::uint32_t function_index);
std::vector<StackTraceFrame> capture_stack_trace(const Bytecode& active_bytecode, std::size_t ip,
                                                 const std::vector<VmCallFrameInfo>& frames,
                                                 const Bytecode& main_bytecode);
Value make_stack_trace_value(const std::vector<StackTraceFrame>& trace);
Value make_exception_object(const Bytecode& bytecode, const BuiltinExceptionClasses& classes,
                            RuntimeErrorCategory category, const std::string& message, Value cause,
                            const SourceLocation& location, const std::vector<StackTraceFrame>& trace);
Value make_named_exception_object(const Bytecode& bytecode, std::uint32_t class_index, const std::string& message,
                                  Value cause, const SourceLocation& location,
                                  const std::vector<StackTraceFrame>& trace);
bool is_subclass_of_exception(const Value& value, std::uint32_t class_index, const Bytecode& bytecode);
Value enrich_exception_value(const Bytecode& bytecode, Value value, const std::vector<StackTraceFrame>& trace,
                             const SourceLocation& location);
std::string format_exception_summary(const Value& value);
std::string format_uncaught_exception(const Value& value, const Bytecode& bytecode);
std::string format_stack_trace_lines(const std::vector<StackTraceFrame>& trace);
bool is_exception_value(const Value& value, const Bytecode& bytecode);
bool is_stop_iteration(const Value& value, const BuiltinExceptionClasses& classes, const Bytecode& bytecode);
bool is_stop_iteration_runtime_error(const RuntimeError& err);
bool is_null_value(const Value& value);

}  // namespace pscript
