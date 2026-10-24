#include "vm/vm.h"

#include "debug/debug_mapping.h"
#include "debug/debug_vm_inspector.h"
#include "vm/exception_support.h"

namespace pscript {
namespace debug {

bool debug_local_in_scope(const DebugLocalBinding& binding, std::uint32_t ip) {
    if (binding.kind == DebugLocalKind::Parameter) {
        return true;
    }
    return ip >= binding.scope_start_ip && ip < binding.scope_end_ip;
}

void fill_variables(DebugStackFrame& frame, const Bytecode& bytecode, std::uint32_t function_index,
                    std::uint32_t current_ip, const std::vector<std::shared_ptr<Cell>>& locals,
                    const std::vector<std::shared_ptr<Cell>>& upvalues) {
    if (function_index >= bytecode.functions.size()) {
        return;
    }
    const FunctionDebugInfo* debug =
        function_index < bytecode.function_debug.size() ? &bytecode.function_debug[function_index] : nullptr;
    if (debug != nullptr) {
        for (const DebugLocalBinding& binding : debug->locals) {
            DebugVariable variable;
            variable.name = binding.name;
            variable.kind = binding.kind;
            variable.in_scope = debug_local_in_scope(binding, current_ip);
            if (!variable.in_scope && binding.kind != DebugLocalKind::Parameter) {
                continue;
            }
            if (binding.kind != DebugLocalKind::Upvalue && binding.slot < locals.size() && locals[binding.slot] &&
                locals[binding.slot]->value.has_value()) {
                variable.value = *locals[binding.slot]->value;
            }
            frame.locals.push_back(std::move(variable));
        }
        for (std::size_t i = 0; i < debug->upvalue_names.size(); ++i) {
            DebugVariable variable;
            variable.name = debug->upvalue_names[i];
            variable.kind = DebugLocalKind::Upvalue;
            if (i < upvalues.size() && upvalues[i] && upvalues[i]->value.has_value()) {
                variable.value = *upvalues[i]->value;
                variable.in_scope = true;
            }
            frame.upvalues.push_back(std::move(variable));
        }
    }
}

DebugStackFrame make_frame(const Bytecode& bytecode, std::uint32_t frame_index, std::uint32_t function_index,
                           std::size_t ip, const std::vector<std::shared_ptr<Cell>>& locals,
                           const std::vector<std::shared_ptr<Cell>>& upvalues) {
    DebugStackFrame frame;
    frame.index = frame_index;
    frame.function_index = function_index;
    frame.instruction_ip = ip;
    frame.function_name = function_name_at(bytecode, function_index);
    const MappedLocation mapped = map_ip_to_source(bytecode, ip);
    frame.source_file = mapped.source_file;
    frame.location = mapped.location;
    fill_variables(frame, bytecode, function_index, static_cast<std::uint32_t>(ip), locals, upvalues);
    return frame;
}

}  // namespace debug

void Vm::set_debug_hook(IDebugHook* hook) {
    debug_hook_ = hook;
}

bool Vm::debug_continue_execution() {
    debug_execution_paused_ = false;
    if (scheduler_active_) {
        run_async_scheduler_loop();
        if (!debug_execution_paused_ && !scheduler_has_pending_work()) {
            finish_async_scheduler_shutdown();
        }
    } else {
        run_sync_loop();
    }
    return !halted_ && !debug_execution_paused_;
}

std::size_t Vm::debug_instruction_pointer() const {
    return ip_;
}

const Bytecode& Vm::debug_active_bytecode() const {
    return code();
}

std::size_t Vm::debug_call_depth() const {
    return frames_.size();
}

std::vector<debug::DebugStackFrame> Vm::debug_stack_frames() const {
    using debug::DebugStackFrame;
    using debug::DebugVariable;
    std::vector<debug::DebugStackFrame> out;
    const Bytecode& active = code();
    for (std::size_t i = 0; i < frames_.size(); ++i) {
        const CallFrame& saved = frames_[i];
        const std::size_t ip = saved.return_ip > 0 ? saved.return_ip - 1 : 0;
        out.push_back(debug::make_frame(active, static_cast<std::uint32_t>(i), saved.function_index, ip, saved.locals,
                                        saved.upvalues));
    }
    const std::uint32_t current_fn = current_profile_function_index();
    out.push_back(
        debug::make_frame(active, static_cast<std::uint32_t>(frames_.size()), current_fn, ip_, locals_, upvalues_));
    return out;
}

Value Vm::debug_invoke_callable(const Value& callable, const std::vector<Value>& args) {
    return invoke_callable_for_runtime(callable, args);
}

std::optional<Value> Vm::debug_function_value(const std::string& name) const {
    for (std::uint32_t i = 0; i < bytecode_.functions.size(); ++i) {
        if (bytecode_.functions[i].name == name) {
            auto data = std::make_shared<ClosureData>();
            data->function_index = i;
            data->name = name;
            return Runtime::make_function(data);
        }
    }
    return std::nullopt;
}

std::optional<Value> Vm::debug_global_variable(const std::string& name) const {
    for (std::size_t i = 0; i < bytecode_.variables.size(); ++i) {
        if (bytecode_.variables[i].name == name) {
            if (i < variables_.size() && variables_[i].has_value()) {
                return *variables_[i];
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace pscript
