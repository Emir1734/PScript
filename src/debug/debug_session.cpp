#include "debug/debug_session.h"

#include "debug/debug_mapping.h"
#include "vm/exception_support.h"
#include "vm/vm.h"

namespace pscript::debug {
namespace {

bool value_is_truthy(const Value& value) {
    switch (value.kind) {
        case ValueKind::Null:
            return false;
        case ValueKind::Bool:
            return value.boolean;
        case ValueKind::Integer:
            return value.integer != 0;
        case ValueKind::Float:
            return value.floating != 0.0;
        case ValueKind::String:
            return !value.string.empty();
        default:
            return true;
    }
}

}  // namespace

ExecutionController::ExecutionController(const Bytecode& bytecode) : bytecode_(bytecode) {}

void ExecutionController::emit(DebugEventKind kind, const std::string& message, const MappedLocation& loc) {
    DebugEvent event;
    event.kind = kind;
    event.message = message;
    event.location = loc.location;
    event.source_file = loc.source_file;
    event.function_index = loc.function_index;
    events_.push_back(event);
    if (listener_ != nullptr) {
        listener_->on_event(event);
    }
}

void ExecutionController::resolve_breakpoint(SourceBreakpoint& bp) {
    bp.resolved_ips = map_line_to_ips(bytecode_, bp.source_file, bp.line);
    bp.state = bp.resolved_ips.empty() ? BreakpointState::Requested : BreakpointState::Resolved;
}

std::uint32_t ExecutionController::add_line_breakpoint(const std::string& source_file, std::uint32_t line,
                                                       const std::string& condition) {
    SourceBreakpoint bp;
    bp.source_file = source_file;
    bp.line = line;
    bp.condition = condition;
    resolve_breakpoint(bp);
    bp.state = BreakpointState::Enabled;
    const std::uint32_t id = next_breakpoint_id_++;
    line_breakpoints_.emplace(id, std::move(bp));
    return id;
}

std::uint32_t ExecutionController::add_temporary_line_breakpoint(const std::string& source_file, std::uint32_t line) {
    const std::uint32_t id = add_line_breakpoint(source_file, line);
    set_breakpoint_one_shot(id, true);
    return id;
}

bool ExecutionController::remove_breakpoint(std::uint32_t id) {
    return line_breakpoints_.erase(id) > 0;
}

void ExecutionController::set_breakpoint_enabled(std::uint32_t id, bool enabled) {
    const auto it = line_breakpoints_.find(id);
    if (it == line_breakpoints_.end()) {
        return;
    }
    it->second.state = enabled ? BreakpointState::Enabled : BreakpointState::Disabled;
}

void ExecutionController::set_breakpoint_condition(std::uint32_t id, const std::string& condition) {
    const auto it = line_breakpoints_.find(id);
    if (it == line_breakpoints_.end()) {
        return;
    }
    it->second.condition = condition;
}

void ExecutionController::set_breakpoint_hit_count_target(std::uint32_t id, std::uint32_t target) {
    const auto it = line_breakpoints_.find(id);
    if (it == line_breakpoints_.end()) {
        return;
    }
    it->second.hit_count_target = target;
    it->second.hit_count = 0;
}

void ExecutionController::set_breakpoint_one_shot(std::uint32_t id, bool one_shot) {
    const auto it = line_breakpoints_.find(id);
    if (it == line_breakpoints_.end()) {
        return;
    }
    it->second.one_shot = one_shot;
}

void ExecutionController::add_function_breakpoint(const std::string& function_name) {
    FunctionBreakpoint bp;
    bp.function_name = function_name;
    bp.state = BreakpointState::Enabled;
    function_breakpoints_.push_back(std::move(bp));
}

bool ExecutionController::can_accept_command() const {
    return state_ == DebugState::Paused || state_ == DebugState::Exception || state_ == DebugState::Stepping;
}

void ExecutionController::pause() {
    if (state_ == DebugState::Exited || state_ == DebugState::Stopped) {
        return;
    }
    state_ = DebugState::Paused;
    step_kind_ = StepKind::StepNone;
}

void ExecutionController::continue_execution() {
    if (!can_accept_command()) {
        return;
    }
    if (attached_vm_ != nullptr) {
        skip_breakpoint_ip_once_ = attached_vm_->debug_instruction_pointer();
    }
    state_ = DebugState::Running;
    step_kind_ = StepKind::StepNone;
    emit(DebugEventKind::Continued, "continued", {});
}

void ExecutionController::capture_step_anchor() {
    if (attached_vm_ == nullptr) {
        return;
    }
    VmInspector inspector(*attached_vm_);
    const MappedLocation loc = inspector.current_location();
    step_anchor_line_ = loc.location.line;
    step_anchor_column_ = loc.location.column;
    step_anchor_depth_ = inspector.call_depth();
    skip_breakpoint_ip_once_.reset();
}

void ExecutionController::step_into() {
    if (!can_accept_command()) {
        return;
    }
    capture_step_anchor();
    state_ = DebugState::Stepping;
    step_kind_ = StepKind::StepInto;
}

void ExecutionController::step_over() {
    if (!can_accept_command()) {
        return;
    }
    capture_step_anchor();
    state_ = DebugState::Stepping;
    step_kind_ = StepKind::StepOver;
}

void ExecutionController::step_out() {
    if (!can_accept_command()) {
        return;
    }
    capture_step_anchor();
    state_ = DebugState::Stepping;
    step_kind_ = StepKind::StepLeave;
}

bool ExecutionController::attach(Vm& vm) {
    attached_vm_ = &vm;
    jit_was_enabled_ = vm.jit_enabled();
    vm.set_jit_enabled(false);
    vm.set_debug_hook(this);
    state_ = DebugState::Running;
    emit(DebugEventKind::Started, "debug attached", {});
    return true;
}

void ExecutionController::detach(Vm& vm) {
    attached_vm_ = nullptr;
    vm.set_debug_hook(nullptr);
    vm.set_jit_enabled(jit_was_enabled_);
    state_ = DebugState::Stopped;
    emit(DebugEventKind::Terminated, "debug detached", {});
}

std::optional<Value> ExecutionController::evaluate_watch(const std::string& expression, EvalError& error) const {
    if (attached_vm_ == nullptr) {
        error.message = "debugger not attached";
        return std::nullopt;
    }
    VmInspector inspector(*attached_vm_);
    ExpressionEvaluator evaluator(inspector);
    return evaluator.evaluate(expression, error);
}

std::optional<bool> ExecutionController::evaluate_breakpoint_condition(Vm& vm, const SourceBreakpoint& bp,
                                                                     EvalError& error) const {
    if (bp.condition.empty()) {
        return true;
    }
    VmInspector inspector(vm);
    ExpressionEvaluator evaluator(inspector);
    const std::optional<Value> value = evaluator.evaluate(bp.condition, error);
    if (!value.has_value()) {
        return std::nullopt;
    }
    return value_is_truthy(*value);
}

bool ExecutionController::line_breakpoint_matches(Vm& vm, std::size_t ip, SourceBreakpoint& bp) {
    bool ip_matches = false;
    for (const std::size_t candidate : bp.resolved_ips) {
        if (candidate == ip) {
            ip_matches = true;
            break;
        }
    }
    if (!ip_matches) {
        return false;
    }
    ++bp.hit_count;
    if (bp.hit_count_target.has_value() && bp.hit_count < *bp.hit_count_target) {
        return false;
    }
    EvalError error;
    const std::optional<bool> condition = evaluate_breakpoint_condition(vm, bp, error);
    if (!condition.has_value()) {
        if (!error.message.empty()) {
            emit(DebugEventKind::Stopped, "breakpoint condition error: " + error.message,
                 map_ip_to_source(vm.debug_active_bytecode(), ip));
        }
        return false;
    }
    if (!*condition) {
        return false;
    }
    if (bp.one_shot) {
        bp.state = BreakpointState::Disabled;
    }
    return true;
}

bool ExecutionController::breakpoint_hit_at(Vm& vm, std::size_t ip) {
    if (skip_breakpoint_ip_once_.has_value() && *skip_breakpoint_ip_once_ == ip) {
        skip_breakpoint_ip_once_.reset();
        return false;
    }
    for (auto& entry : line_breakpoints_) {
        SourceBreakpoint& bp = entry.second;
        if (bp.state != BreakpointState::Enabled) {
            continue;
        }
        if (line_breakpoint_matches(vm, ip, bp)) {
            return true;
        }
    }
    return false;
}

bool ExecutionController::stepping_complete(Vm& vm, std::size_t ip) const {
    VmInspector inspector(vm);
    const MappedLocation loc = map_ip_to_source(inspector.active_bytecode(), ip);
    const std::size_t stack_depth = inspector.call_depth();
    if (step_kind_ == StepKind::StepInto) {
        if (stack_depth > step_anchor_depth_) {
            return true;
        }
        return loc.location.line != step_anchor_line_ || loc.location.column != step_anchor_column_;
    }
    if (step_kind_ == StepKind::StepOver) {
        if (stack_depth > step_anchor_depth_) {
            return false;
        }
        if (stack_depth < step_anchor_depth_) {
            return true;
        }
        return loc.location.line != step_anchor_line_ || loc.location.column != step_anchor_column_;
    }
    if (step_kind_ == StepKind::StepLeave) {
        return stack_depth < step_anchor_depth_;
    }
    return false;
}

bool ExecutionController::function_name_matches(const std::string& breakpoint_name,
                                                const std::string& actual_name) const {
    if (breakpoint_name == actual_name) {
        return true;
    }
    if (actual_name.size() > breakpoint_name.size() + 1 &&
        actual_name.compare(actual_name.size() - breakpoint_name.size(), breakpoint_name.size(), breakpoint_name) == 0 &&
        actual_name[actual_name.size() - breakpoint_name.size() - 1] == '.') {
        return true;
    }
    return false;
}

bool ExecutionController::before_instruction(Vm& vm, std::size_t ip) {
    if (state_ == DebugState::Paused || state_ == DebugState::Exception) {
        return true;
    }
    if (state_ == DebugState::Exited || state_ == DebugState::Stopped) {
        return true;
    }
    if (breakpoint_hit_at(vm, ip)) {
        state_ = DebugState::Paused;
        step_kind_ = StepKind::StepNone;
        emit(DebugEventKind::BreakpointHit, "breakpoint", map_ip_to_source(vm.debug_active_bytecode(), ip));
        return true;
    }
    if (state_ == DebugState::Stepping && stepping_complete(vm, ip)) {
        state_ = DebugState::Paused;
        step_kind_ = StepKind::StepNone;
        emit(DebugEventKind::StepComplete, "step", map_ip_to_source(vm.debug_active_bytecode(), ip));
        return true;
    }
    return false;
}

void ExecutionController::after_instruction(Vm& vm, std::size_t ip) {
    (void)vm;
    (void)ip;
}

void ExecutionController::on_function_enter(Vm& vm, std::uint32_t function_index) {
    if (function_index >= bytecode_.functions.size()) {
        return;
    }
    const std::string& name = bytecode_.functions[function_index].name;
    for (const FunctionBreakpoint& bp : function_breakpoints_) {
        if (bp.state == BreakpointState::Enabled && function_name_matches(bp.function_name, name)) {
            state_ = DebugState::Paused;
            step_kind_ = StepKind::StepNone;
            emit(DebugEventKind::BreakpointHit, "function breakpoint",
                 map_ip_to_source(vm.debug_active_bytecode(), vm.debug_instruction_pointer()));
            return;
        }
    }
}

void ExecutionController::on_program_exit(Vm& vm) {
    (void)vm;
    if (state_ == DebugState::Stopped || state_ == DebugState::Exited) {
        return;
    }
    state_ = DebugState::Exited;
    emit(DebugEventKind::Exited, "program exited", {});
}

void ExecutionController::on_exception(Vm& vm, const Value& exception, bool will_catch) {
    if (exception_mode_ == ExceptionBreakpointMode::None) {
        return;
    }
    if (exception_mode_ == ExceptionBreakpointMode::Uncaught && will_catch) {
        return;
    }
    state_ = DebugState::Exception;
    emit(DebugEventKind::Exception, format_exception_summary(exception),
         map_ip_to_source(vm.debug_active_bytecode(), vm.debug_instruction_pointer()));
    state_ = DebugState::Paused;
}

}  // namespace pscript::debug
