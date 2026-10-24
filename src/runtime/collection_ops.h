#pragma once

#include "runtime/runtime.h"

#include <vector>

namespace pscript {

std::vector<Value> materialize_iterable(Runtime& runtime, const Value& iterable);
Value builtin_filter(Runtime& runtime, const std::vector<Value>& args);
Value builtin_map(Runtime& runtime, const std::vector<Value>& args);
Value builtin_sort_by(Runtime& runtime, const std::vector<Value>& args);
Value builtin_group_by(Runtime& runtime, const std::vector<Value>& args);
Value builtin_take(Runtime& runtime, const std::vector<Value>& args);
Value builtin_parallel_map(Runtime& runtime, const std::vector<Value>& args);

bool parallel_callable_is_worker_safe(const Value& callable);
std::size_t parallel_map_last_peak_concurrency();

}  // namespace pscript
