#include "stdlib/native_common.h"

#include <filesystem>
#include <sstream>

namespace pscript {
namespace {

std::string path_to_string(const std::filesystem::path& path) {
    return path.generic_string();
}

std::filesystem::path path_from_value(const Value& value, std::string_view function, std::string_view what) {
    return std::filesystem::path(native_require_string(value, function, what));
}

[[noreturn]] void path_fail(const std::string& message) {
    native_value_error(message);
}

}  // namespace

Value native_path_join(const std::vector<Value>& args) {
    native_require_argc("join", args, 2);
    std::filesystem::path result = path_from_value(args[0], "join", "segment");
    result /= path_from_value(args[1], "join", "segment");
    return Runtime::make_string(path_to_string(result));
}

Value native_path_basename(const std::vector<Value>& args) {
    native_require_argc("basename", args, 1);
    const std::filesystem::path path = path_from_value(args[0], "basename", "path");
    return Runtime::make_string(path.filename().generic_string());
}

Value native_path_dirname(const std::vector<Value>& args) {
    native_require_argc("dirname", args, 1);
    const std::filesystem::path path = path_from_value(args[0], "dirname", "path");
    const std::filesystem::path parent = path.parent_path();
    if (parent.empty()) {
        return Runtime::make_string("");
    }
    return Runtime::make_string(path_to_string(parent));
}

Value native_path_extension(const std::vector<Value>& args) {
    native_require_argc("extension", args, 1);
    const std::filesystem::path path = path_from_value(args[0], "extension", "path");
    return Runtime::make_string(path.extension().generic_string());
}

Value native_path_stem(const std::vector<Value>& args) {
    native_require_argc("stem", args, 1);
    const std::filesystem::path path = path_from_value(args[0], "stem", "path");
    return Runtime::make_string(path.stem().generic_string());
}

Value native_path_normalize(const std::vector<Value>& args) {
    native_require_argc("normalize", args, 1);
    const std::filesystem::path path = path_from_value(args[0], "normalize", "path");
    return Runtime::make_string(path_to_string(path.lexically_normal()));
}

Value native_path_absolute(const std::vector<Value>& args) {
    native_require_argc("absolute", args, 1);
    const std::filesystem::path path = path_from_value(args[0], "absolute", "path");
    try {
        return Runtime::make_string(path_to_string(std::filesystem::absolute(path)));
    } catch (const std::exception& ex) {
        native_runtime_error(native_filesystem_message("absolute", path.generic_string(), ex));
    }
}

Value native_path_resolve(const std::vector<Value>& args) {
    native_require_min_argc("resolve", args, 1);
    std::filesystem::path result = path_from_value(args[0], "resolve", "path");
    for (std::size_t i = 1; i < args.size(); ++i) {
        result /= path_from_value(args[i], "resolve", "path");
    }
    try {
        result = std::filesystem::absolute(result).lexically_normal();
    } catch (const std::exception& ex) {
        native_runtime_error(native_filesystem_message("resolve", result.generic_string(), ex));
    }
    return Runtime::make_string(path_to_string(result));
}

Value native_path_is_absolute(const std::vector<Value>& args) {
    native_require_argc("is_absolute", args, 1);
    const std::filesystem::path path = path_from_value(args[0], "is_absolute", "path");
    return Runtime::make_bool(path.is_absolute());
}

Value native_path_is_relative(const std::vector<Value>& args) {
    native_require_argc("is_relative", args, 1);
    const std::filesystem::path path = path_from_value(args[0], "is_relative", "path");
    return Runtime::make_bool(!path.is_absolute());
}

Value native_path_relative(const std::vector<Value>& args) {
    native_require_argc("relative", args, 2);
    const std::filesystem::path target = path_from_value(args[0], "relative", "target");
    const std::filesystem::path base = path_from_value(args[1], "relative", "base");
    try {
        const std::filesystem::path result = std::filesystem::relative(target, base);
        if (result.empty()) {
            return Runtime::make_string(".");
        }
        return Runtime::make_string(path_to_string(result));
    } catch (const std::exception& ex) {
        path_fail(native_filesystem_message("relative", target.generic_string(), ex));
    }
}

}  // namespace pscript
