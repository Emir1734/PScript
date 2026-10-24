#pragma once

#include "runtime/runtime.h"

#include <string>
#include <string_view>
#include <vector>

namespace pscript {

[[noreturn]] void native_type_error(std::string message);
[[noreturn]] void native_value_error(std::string message);
[[noreturn]] void native_runtime_error(std::string message);

const std::string& native_require_string(const Value& value, std::string_view function, std::string_view what);
const std::string& native_require_string_arg(const std::vector<Value>& args, std::size_t index,
                                             std::string_view function, std::string_view what);
void native_require_argc(std::string_view function, const std::vector<Value>& args, std::size_t count);
void native_require_min_argc(std::string_view function, const std::vector<Value>& args, std::size_t min_count);

std::string native_filesystem_message(std::string_view operation, const std::string& path,
                                      const std::exception& error);

}  // namespace pscript
