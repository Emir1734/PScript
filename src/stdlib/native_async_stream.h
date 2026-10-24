#pragma once

#include "runtime/runtime.h"

#include <string>
#include <vector>

namespace pscript {

struct NativeAsyncStreamData {
    std::vector<std::string> chunks;
    std::size_t read_index = 0;
    bool closed = false;
};

bool is_native_async_stream(const Value& value);
Value make_native_async_stream(std::uint32_t class_index, std::vector<std::string> chunks);
Value call_native_async_stream_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                      Runtime& runtime);

}  // namespace pscript
