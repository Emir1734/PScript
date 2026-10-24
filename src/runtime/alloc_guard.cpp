#include "runtime/alloc_guard.h"

#include "runtime/profiler.h"
#include "runtime/runtime.h"

namespace pscript::alloc_guard {

FailHook& test_fail_hook() {
    static FailHook hook;
    return hook;
}

void throw_if_blocked(std::size_t bytes) {
    if (RuntimeProfiler* profiler = thread_local_profiler()) {
        profiler->on_allocation(bytes);
    }
    if (test_fail_hook() && test_fail_hook()(bytes)) {
        throw RuntimeError("out of memory", RuntimeErrorCategory::Runtime);
    }
}

}  // namespace pscript::alloc_guard
