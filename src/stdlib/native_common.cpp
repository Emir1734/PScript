#include "stdlib/native_common.h"

namespace pscript {

[[noreturn]] void native_type_error(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Type);
}

[[noreturn]] void native_value_error(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Value);
}

[[noreturn]] void native_runtime_error(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Runtime);
}

const std::string& native_require_string(const Value& value, std::string_view function, std::string_view what) {
    if (value.kind != ValueKind::String) {
        native_type_error(std::string("'") + std::string(function) + "' expects a string " + std::string(what));
    }
    return value.string;
}

const std::string& native_require_string_arg(const std::vector<Value>& args, std::size_t index,
                                             std::string_view function, std::string_view what) {
    if (index >= args.size()) {
        native_value_error(std::string("'") + std::string(function) + "' missing argument");
    }
    return native_require_string(args[index], function, what);
}

void native_require_argc(std::string_view function, const std::vector<Value>& args, std::size_t count) {
    if (args.size() == count) {
        return;
    }
    native_value_error(std::string("'") + std::string(function) + "' expects " + std::to_string(count) +
                       (count == 1 ? " argument" : " arguments"));
}

void native_require_min_argc(std::string_view function, const std::vector<Value>& args, std::size_t min_count) {
    if (args.size() >= min_count) {
        return;
    }
    native_value_error(std::string("'") + std::string(function) + "' expects at least " +
                       std::to_string(min_count) + " arguments");
}

std::string native_filesystem_message(std::string_view operation, const std::string& path,
                                      const std::exception& error) {
    return std::string(operation) + " failed for '" + path + "': " + error.what();
}

}  // namespace pscript
