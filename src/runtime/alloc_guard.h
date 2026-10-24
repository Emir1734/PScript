#pragma once

#include <cstddef>
#include <functional>

namespace pscript::alloc_guard {

using FailHook = std::function<bool(std::size_t bytes)>;

/** Test-only hook; when it returns true, allocation sites throw RuntimeError instead of allocating. */
FailHook& test_fail_hook();

void throw_if_blocked(std::size_t bytes);

}  // namespace pscript::alloc_guard
