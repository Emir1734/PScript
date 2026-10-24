#pragma once

#include "debug/debug_eval.h"
#include "debug/debug_types.h"
#include "debug/debug_vm_inspector.h"
#include "vm/debug_hook.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pscript {

class Vm;

namespace debug {

class IDebugEventListener {
public:
    virtual ~IDebugEventListener() = default;
    virtual void on_event(const DebugEvent& event) = 0;
};

class ExecutionController : public IDebugHook {
public:
    explicit ExecutionController(const Bytecode& bytecode);

    DebugState state() const { return state_; }
    const std::vector<DebugEvent>& events() const { return events_; }

    void set_listener(IDebugEventListener* listener) { listener_ = listener; }

    std::uint32_t add_line_breakpoint(const std::string& source_file, std::uint32_t line,
                                      const std::string& condition = "");
    bool remove_breakpoint(std::uint32_t id);
    void set_breakpoint_enabled(std::uint32_t id, bool enabled);
    void set_breakpoint_condition(std::uint32_t id, const std::string& condition);
    void set_breakpoint_hit_count_target(std::uint32_t id, std::uint32_t target);
    void set_breakpoint_one_shot(std::uint32_t id, bool one_shot);
    std::uint32_t add_temporary_line_breakpoint(const std::string& source_file, std::uint32_t line);
    void add_function_breakpoint(const std::string& function_name);
    void set_exception_breakpoint(ExceptionBreakpointMode mode) { exception_mode_ = mode; }

    std::optional<Value> evaluate_watch(const std::string& expression, EvalError& error) const;

    void pause();
    void continue_execution();
    void step_into();
    void step_over();
    void step_out();

    bool before_instruction(Vm& vm, std::size_t ip) override;
    void after_instruction(Vm& vm, std::size_t ip) override;
    void on_function_enter(Vm& vm, std::uint32_t function_index) override;
    void on_exception(Vm& vm, const Value& exception, bool will_catch) override;
    void on_program_exit(Vm& vm) override;

    bool attach(Vm& vm);
    void detach(Vm& vm);

private:
    const Bytecode& bytecode_;
    Vm* attached_vm_ = nullptr;
    DebugState state_ = DebugState::Idle;
    StepKind step_kind_ = StepKind::StepNone;
    std::uint32_t step_anchor_line_ = 0;
    std::uint32_t step_anchor_column_ = 0;
    std::size_t step_anchor_depth_ = 0;
    std::uint32_t next_breakpoint_id_ = 1;
    ExceptionBreakpointMode exception_mode_ = ExceptionBreakpointMode::Uncaught;
    IDebugEventListener* listener_ = nullptr;
    std::vector<DebugEvent> events_;
    std::unordered_map<std::uint32_t, SourceBreakpoint> line_breakpoints_;
    std::vector<FunctionBreakpoint> function_breakpoints_;
    bool jit_was_enabled_ = true;
    std::optional<std::size_t> skip_breakpoint_ip_once_;

    void capture_step_anchor();
    void emit(DebugEventKind kind, const std::string& message, const MappedLocation& loc);
    void resolve_breakpoint(SourceBreakpoint& bp);
    std::optional<bool> evaluate_breakpoint_condition(Vm& vm, const SourceBreakpoint& bp, EvalError& error) const;
    bool line_breakpoint_matches(Vm& vm, std::size_t ip, SourceBreakpoint& bp);
    bool breakpoint_hit_at(Vm& vm, std::size_t ip);
    bool stepping_complete(Vm& vm, std::size_t ip) const;
    bool function_name_matches(const std::string& breakpoint_name, const std::string& actual_name) const;
    bool can_accept_command() const;
};

}  // namespace debug
}  // namespace pscript
