#pragma once

#include "runtime/runtime.h"

#include <exception>
#include <new>
#include <string>
#include <utility>

namespace pscript::native_boundary {

/** Test-only. When set, the next native call invokes it inside the VM exception boundary. */
inline void (*test_fault_hook)() = nullptr;

template <typename Fn>
auto guard(Fn&& fn) -> decltype(fn()) {
    try {
        if (test_fault_hook != nullptr) {
            test_fault_hook();
        }
        return std::forward<Fn>(fn)();
    } catch (const RuntimeError&) {
        throw;
    } catch (const ProcessExit&) {
        throw;
    } catch (const std::bad_alloc&) {
        throw RuntimeError("out of memory");
    } catch (const std::exception& ex) {
        throw RuntimeError(std::string(ex.what()));
    }
}

}  // namespace pscript::native_boundary
