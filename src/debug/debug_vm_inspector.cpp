#include "debug/debug_vm_inspector.h"

#include "debug/debug_mapping.h"
#include "vm/vm.h"

namespace pscript::debug {

VmInspector::VmInspector(Vm& vm) : vm_(vm) {}

std::size_t VmInspector::instruction_pointer() const {
    return vm_.debug_instruction_pointer();
}

const Bytecode& VmInspector::active_bytecode() const {
    return vm_.debug_active_bytecode();
}

MappedLocation VmInspector::current_location() const {
    return map_ip_to_source(active_bytecode(), instruction_pointer());
}

std::size_t VmInspector::call_depth() const {
    return vm_.debug_call_depth();
}

std::vector<DebugStackFrame> VmInspector::stack_frames() const {
    return vm_.debug_stack_frames();
}

std::optional<Value> VmInspector::global_variable(const std::string& name) const {
    return vm_.debug_global_variable(name);
}

Value VmInspector::invoke_callable(const Value& callable, const std::vector<Value>& args) const {
    return vm_.debug_invoke_callable(callable, args);
}

std::optional<Value> VmInspector::function_value(const std::string& name) const {
    return vm_.debug_function_value(name);
}

}  // namespace pscript::debug
