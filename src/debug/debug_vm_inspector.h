#pragma once

#include "compiler/bytecode.h"
#include "debug/debug_mapping.h"
#include "debug/debug_types.h"
#include "runtime/runtime.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pscript {

class Vm;

namespace debug {

struct DebugVariable {
    std::string name;
    DebugLocalKind kind = DebugLocalKind::Local;
    Value value;
    bool in_scope = true;
};

struct DebugStackFrame {
    std::uint32_t index = 0;
    std::string function_name;
    std::string source_file;
    SourceLocation location;
    std::uint32_t function_index = 0xFFFFFFFFu;
    std::size_t instruction_ip = 0;
    std::vector<DebugVariable> locals;
    std::vector<DebugVariable> upvalues;
};

class VmInspector {
public:
    explicit VmInspector(Vm& vm);

    std::size_t instruction_pointer() const;
    const Bytecode& active_bytecode() const;
    MappedLocation current_location() const;
    std::vector<DebugStackFrame> stack_frames() const;
    std::size_t call_depth() const;
    std::optional<Value> global_variable(const std::string& name) const;
    Value invoke_callable(const Value& callable, const std::vector<Value>& args) const;
    std::optional<Value> function_value(const std::string& name) const;

    Vm& vm() const { return vm_; }

private:
    Vm& vm_;
};

}  // namespace debug
}  // namespace pscript
