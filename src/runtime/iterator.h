#pragma once

#include "runtime/runtime.h"

namespace pscript {

enum class NativeIteratorKind : std::uint32_t {
    List = 1,
    Tuple,
    Dict,
    Set,
    Range,
    Bytes,
    SqlRows,
    Archive,
};

struct NativeIteratorData {
    NativeIteratorKind kind = NativeIteratorKind::List;
    std::int64_t index = 0;
    Value source;
    bool exhausted = false;
};

bool is_native_iterator(const Value& value);
Value make_native_iterator(NativeIteratorKind kind, Value source);
Value native_iterator_iter(const Value& iterator);
Value native_iterator_next(const Value& iterator);
// Returns true and writes the next element to out; false means exhausted (StopIteration).
bool native_iterator_try_next(const Value& iterator, Value& out);
Value runtime_get_iter(const Value& value);
bool is_native_sql_rows(const Value& value);

}  // namespace pscript
