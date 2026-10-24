#pragma once

#include "runtime/runtime.h"

#include <cstddef>
#include <cstdint>

namespace pscript {

class Vm;

struct IDebugHook {
    virtual ~IDebugHook() = default;
    virtual bool before_instruction(Vm& vm, std::size_t ip) = 0;
    virtual void after_instruction(Vm& vm, std::size_t ip) {}
    virtual void on_function_enter(Vm& vm, std::uint32_t function_index) {}
    virtual void on_exception(Vm& vm, const Value& exception, bool will_catch) {}
    virtual void on_program_exit(Vm& vm) {}
};

}  // namespace pscript
