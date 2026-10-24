#include "vm/exception_support.h"

#include <sstream>
#include <unordered_map>

namespace pscript {
namespace {

std::uint32_t find_class_by_name(const Bytecode& bytecode, const std::string& name) {
    for (std::size_t i = 0; i < bytecode.classes.size(); ++i) {
        if (bytecode.classes[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return 0xFFFFFFFFu;
}

std::uint32_t find_field_index(const ClassInfo& info, const std::string& name) {
    for (std::size_t i = 0; i < info.layout.size(); ++i) {
        if (info.layout[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return 0xFFFFFFFFu;
}

void set_field_by_name(Value& instance, const ClassInfo& info, const std::string& name, Value value) {
    const std::uint32_t index = find_field_index(info, name);
    if (index != 0xFFFFFFFFu) {
        Runtime::set_field(instance, index, std::move(value));
    }
}

bool is_subclass_of(std::uint32_t class_index, std::uint32_t ancestor_index, const std::vector<ClassInfo>& classes) {
    for (std::uint32_t current = class_index; current != 0xFFFFFFFFu; current = classes[current].parent_index) {
        if (current == ancestor_index) {
            return true;
        }
    }
    return false;
}

std::string value_to_text(const Value& value) {
    if (value.kind == ValueKind::Null) {
        return "null";
    }
    if (value.kind == ValueKind::String) {
        return value.string;
    }
    return Runtime::format(value);
}

}  // namespace

BuiltinExceptionClasses resolve_builtin_exceptions(const Bytecode& bytecode) {
    BuiltinExceptionClasses classes;
    classes.exception = find_class_by_name(bytecode, "Exception");
    classes.runtime = find_class_by_name(bytecode, "RuntimeException");
    classes.type = find_class_by_name(bytecode, "TypeException");
    classes.name = find_class_by_name(bytecode, "NameException");
    classes.index = find_class_by_name(bytecode, "IndexException");
    classes.value = find_class_by_name(bytecode, "ValueException");
    classes.cancellation = find_class_by_name(bytecode, "CancellationException");
    classes.timeout = find_class_by_name(bytecode, "TimeoutException");
    classes.stop_iteration = find_class_by_name(bytecode, "StopIteration");
    classes.network = find_class_by_name(bytecode, "NetworkException");
    return classes;
}

bool is_stop_iteration(const Value& value, const BuiltinExceptionClasses& classes, const Bytecode& bytecode) {
    if (classes.stop_iteration == 0xFFFFFFFFu) {
        return false;
    }
    return is_subclass_of_exception(value, classes.stop_iteration, bytecode);
}

bool is_stop_iteration_runtime_error(const RuntimeError& err) {
    return err.message() == "StopIteration";
}

std::uint32_t exception_class_for_category(RuntimeErrorCategory category, const BuiltinExceptionClasses& classes) {
    switch (category) {
        case RuntimeErrorCategory::Type:
            return classes.type != 0xFFFFFFFFu ? classes.type : classes.runtime;
        case RuntimeErrorCategory::Name:
            return classes.name != 0xFFFFFFFFu ? classes.name : classes.runtime;
        case RuntimeErrorCategory::Index:
            return classes.index != 0xFFFFFFFFu ? classes.index : classes.runtime;
        case RuntimeErrorCategory::Value:
            return classes.value != 0xFFFFFFFFu ? classes.value : classes.runtime;
        case RuntimeErrorCategory::Network:
            return classes.network != 0xFFFFFFFFu ? classes.network : classes.runtime;
        case RuntimeErrorCategory::Runtime:
        default:
            return classes.runtime != 0xFFFFFFFFu ? classes.runtime : classes.exception;
    }
}

SourceLocation source_location_at(const Bytecode& bytecode, std::size_t ip) {
    if (ip < bytecode.instruction_locations.size()) {
        return bytecode.instruction_locations[ip];
    }
    return {};
}

std::string function_name_at(const Bytecode& bytecode, std::uint32_t function_index) {
    if (function_index == 0xFFFFFFFFu || function_index >= bytecode.functions.size()) {
        return "main";
    }
    return bytecode.functions[function_index].name;
}

StackTraceFrame make_stack_frame(const Bytecode& location_bytecode, std::size_t ip, const Bytecode& name_bytecode,
                                 std::uint32_t function_index) {
    StackTraceFrame frame;
    frame.function_name = function_name_at(name_bytecode, function_index);
    frame.file = location_bytecode.source_file.empty() ? "<stdin>" : location_bytecode.source_file;
    const SourceLocation location = source_location_at(location_bytecode, ip);
    frame.line = static_cast<int>(location.line);
    frame.column = static_cast<int>(location.column);
    return frame;
}

std::vector<StackTraceFrame> capture_stack_trace(const Bytecode& active_bytecode, std::size_t ip,
                                                 const std::vector<VmCallFrameInfo>& frames,
                                                 const Bytecode& main_bytecode) {
    std::vector<StackTraceFrame> trace;
    if (frames.empty()) {
        trace.push_back(make_stack_frame(active_bytecode, ip, active_bytecode, 0xFFFFFFFFu));
    } else {
        const VmCallFrameInfo& current = frames.back();
        const Bytecode& fn_bc = current.function_bytecode != nullptr ? *current.function_bytecode : main_bytecode;
        trace.push_back(make_stack_frame(active_bytecode, ip, fn_bc, current.function_index));
    }
    for (std::size_t i = frames.size(); i > 0; --i) {
        const VmCallFrameInfo& frame = frames[i - 1];
        const std::uint32_t caller_index = i >= 2 ? frames[i - 2].function_index : 0xFFFFFFFFu;
        const Bytecode& caller_fn_bc =
            i >= 2 && frames[i - 2].function_bytecode != nullptr ? *frames[i - 2].function_bytecode : main_bytecode;
        const Bytecode& call_site_bc = frame.caller_bytecode != nullptr ? *frame.caller_bytecode : main_bytecode;
        const std::size_t call_ip = frame.return_ip > 0 ? frame.return_ip - 1 : 0;
        trace.push_back(make_stack_frame(call_site_bc, call_ip, caller_fn_bc, caller_index));
    }
    return trace;
}

Value make_stack_trace_value(const std::vector<StackTraceFrame>& trace) {
    std::vector<Value> entries;
    entries.reserve(trace.size());
    for (const StackTraceFrame& frame : trace) {
        std::vector<std::pair<std::string, Value>> fields;
        fields.emplace_back("function", Runtime::make_string(frame.function_name));
        fields.emplace_back("file", Runtime::make_string(frame.file));
        fields.emplace_back("line", Runtime::make_integer(frame.line));
        fields.emplace_back("column", Runtime::make_integer(frame.column));
        entries.push_back(Runtime::make_dict(std::move(fields)));
    }
    return Runtime::make_list(std::move(entries));
}

Value make_exception_object(const Bytecode& bytecode, const BuiltinExceptionClasses& classes,
                            RuntimeErrorCategory category, const std::string& message, Value cause,
                            const SourceLocation& location, const std::vector<StackTraceFrame>& trace) {
    const std::uint32_t class_index = exception_class_for_category(category, classes);
    if (class_index == 0xFFFFFFFFu || class_index >= bytecode.classes.size()) {
        return Runtime::make_string(message);
    }
    const ClassInfo& info = bytecode.classes[class_index];
    std::vector<std::string> field_names;
    field_names.reserve(info.layout.size());
    for (const ClassFieldLayout& field : info.layout) {
        field_names.push_back(field.name);
    }
    Value instance = Runtime::make_class(class_index, info.name, std::move(field_names));
    const std::string file = bytecode.source_file.empty() ? "<stdin>" : bytecode.source_file;
    set_field_by_name(instance, info, "message", Runtime::make_string(message));
    set_field_by_name(instance, info, "cause", std::move(cause));
    set_field_by_name(instance, info, "file", Runtime::make_string(file));
    set_field_by_name(instance, info, "line", Runtime::make_integer(static_cast<int>(location.line)));
    set_field_by_name(instance, info, "column", Runtime::make_integer(static_cast<int>(location.column)));
    set_field_by_name(instance, info, "stack_trace", make_stack_trace_value(trace));
    return instance;
}

Value make_named_exception_object(const Bytecode& bytecode, std::uint32_t class_index, const std::string& message,
                                  Value cause, const SourceLocation& location,
                                  const std::vector<StackTraceFrame>& trace) {
    if (class_index == 0xFFFFFFFFu || class_index >= bytecode.classes.size()) {
        return Runtime::make_string(message);
    }
    const ClassInfo& info = bytecode.classes[class_index];
    std::vector<std::string> field_names;
    field_names.reserve(info.layout.size());
    for (const ClassFieldLayout& field : info.layout) {
        field_names.push_back(field.name);
    }
    Value instance = Runtime::make_class(class_index, info.name, std::move(field_names));
    const std::string file = bytecode.source_file.empty() ? "<stdin>" : bytecode.source_file;
    set_field_by_name(instance, info, "message", Runtime::make_string(message));
    set_field_by_name(instance, info, "cause", std::move(cause));
    set_field_by_name(instance, info, "file", Runtime::make_string(file));
    set_field_by_name(instance, info, "line", Runtime::make_integer(static_cast<int>(location.line)));
    set_field_by_name(instance, info, "column", Runtime::make_integer(static_cast<int>(location.column)));
    set_field_by_name(instance, info, "stack_trace", make_stack_trace_value(trace));
    return instance;
}

bool is_subclass_of_exception(const Value& value, std::uint32_t class_index, const Bytecode& bytecode) {
    if (value.kind != ValueKind::Class || !value.class_value || class_index == 0xFFFFFFFFu) {
        return false;
    }
    return is_subclass_of(value.class_value->type_index, class_index, bytecode.classes);
}

bool is_null_value(const Value& value) {
    return value.kind == ValueKind::Null;
}

bool is_exception_value(const Value& value, const Bytecode& bytecode) {
    if (value.kind != ValueKind::Class || !value.class_value) {
        return false;
    }
    const std::uint32_t exception_index = find_class_by_name(bytecode, "Exception");
    if (exception_index == 0xFFFFFFFFu) {
        return false;
    }
    return is_subclass_of(value.class_value->type_index, exception_index, bytecode.classes);
}

Value enrich_exception_value(const Bytecode& bytecode, Value value, const std::vector<StackTraceFrame>& trace,
                             const SourceLocation& location) {
    if (!is_exception_value(value, bytecode)) {
        return value;
    }
    const ClassInfo& info = bytecode.classes[value.class_value->type_index];
    const Value existing_trace = Runtime::get_field(value, find_field_index(info, "stack_trace"));
    if (existing_trace.kind == ValueKind::List && existing_trace.list && !existing_trace.list->items.empty()) {
        return value;
    }
    set_field_by_name(value, info, "file",
                      Runtime::make_string(bytecode.source_file.empty() ? "<stdin>" : bytecode.source_file));
    set_field_by_name(value, info, "line", Runtime::make_integer(static_cast<int>(location.line)));
    set_field_by_name(value, info, "column", Runtime::make_integer(static_cast<int>(location.column)));
    set_field_by_name(value, info, "stack_trace", make_stack_trace_value(trace));
    return value;
}

std::string format_exception_summary(const Value& value) {
    if (value.kind != ValueKind::Class || !value.class_value) {
        return Runtime::format(value);
    }
    std::uint32_t message_index = 0;
    for (std::size_t i = 0; i < value.class_value->field_names.size(); ++i) {
        if (value.class_value->field_names[i] == "message") {
            message_index = static_cast<std::uint32_t>(i);
            break;
        }
    }
    const Value message = Runtime::get_field(value, message_index);
    if (message.kind == ValueKind::String && !message.string.empty()) {
        return value.class_value->type_name + ": " + message.string;
    }
    return value.class_value->type_name;
}

std::vector<StackTraceFrame> extract_trace_from_exception(const Value& value, const Bytecode& bytecode) {
    std::vector<StackTraceFrame> frames;
    if (!is_exception_value(value, bytecode)) {
        return frames;
    }
    const ClassInfo& info = bytecode.classes[value.class_value->type_index];
    const Value trace_value = Runtime::get_field(value, find_field_index(info, "stack_trace"));
    if (trace_value.kind != ValueKind::List || !trace_value.list) {
        return frames;
    }
    for (const Value& entry : trace_value.list->items) {
        if (entry.kind != ValueKind::Dict || !entry.dict) {
            continue;
        }
        StackTraceFrame frame;
        for (const auto& field : entry.dict->entries) {
            if (field.first == "function" && field.second.kind == ValueKind::String) {
                frame.function_name = field.second.string;
            } else if (field.first == "file" && field.second.kind == ValueKind::String) {
                frame.file = field.second.string;
            } else if (field.first == "line" && field.second.kind == ValueKind::Integer) {
                frame.line = static_cast<int>(field.second.integer);
            } else if (field.first == "column" && field.second.kind == ValueKind::Integer) {
                frame.column = static_cast<int>(field.second.integer);
            }
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

std::string format_stack_trace_lines(const std::vector<StackTraceFrame>& trace) {
    std::ostringstream out;
    for (const StackTraceFrame& frame : trace) {
        out << "  at " << frame.function_name << " (" << frame.file << ':' << frame.line << ':' << frame.column << ")\n";
    }
    return out.str();
}

std::string format_cause_chain(const Value& value, const Bytecode& bytecode, int depth,
                               std::unordered_set<const void*>& seen) {
    if (!is_exception_value(value, bytecode) || depth > 32) {
        return {};
    }
    const void* identity = value.class_value.get();
    if (seen.count(identity) != 0) {
        return "  caused by: <cycle>\n";
    }
    seen.insert(identity);

    const ClassInfo& info = bytecode.classes[value.class_value->type_index];
    const Value cause = Runtime::get_field(value, find_field_index(info, "cause"));
    if (is_null_value(cause)) {
        return {};
    }

    std::ostringstream out;
    out << "caused by:\n";
    if (is_exception_value(cause, bytecode)) {
        out << format_exception_summary(cause) << '\n';
        const auto trace = extract_trace_from_exception(cause, bytecode);
        out << format_stack_trace_lines(trace);
        out << format_cause_chain(cause, bytecode, depth + 1, seen);
    } else {
        out << "  " << Runtime::format(cause) << '\n';
    }
    return out.str();
}

std::string format_uncaught_exception(const Value& value, const Bytecode& bytecode) {
    std::ostringstream out;
    out << "Unhandled exception: ";
    if (is_exception_value(value, bytecode)) {
        out << format_exception_summary(value) << '\n';
        const auto trace = extract_trace_from_exception(value, bytecode);
        out << format_stack_trace_lines(trace);
        std::unordered_set<const void*> seen;
        out << format_cause_chain(value, bytecode, 0, seen);
    } else {
        out << Runtime::format(value) << '\n';
    }
    return out.str();
}

}  // namespace pscript
