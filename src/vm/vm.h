#pragma once

#include "compiler/bytecode.h"
#include "runtime/builtin_id.h"
#include "runtime/runtime_limits.h"
#include "runtime/runtime.h"
#include "runtime/task_group.h"
#include "vm/debug_hook.h"
#include "vm/exception_support.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iosfwd>
#include <memory>
#include <optional>

#include "jit/jit_config.h"
#include "jit/jit_exec_state.h"

namespace pscript::jit {
class JitEngine;
enum class JitExecuteResult : std::uint8_t;
struct JitCallIcSlot;
struct JitCallIcEntry;
struct JitCompiledCode;
}

namespace pscript::debug {
struct DebugStackFrame;
}
#include <stdexcept>
#include <string>
#include <vector>

namespace pscript {

class VmError : public std::runtime_error {
public:
    explicit VmError(std::string message);

    const std::string& message() const { return message_; }

private:
    std::string message_;
};

class Vm {
public:
    Vm(Bytecode bytecode, Runtime& runtime);
    Vm(Bytecode bytecode, std::ostream& output);
    ~Vm();

    void run();
    int run_project_tests(const std::optional<std::string>& filter = std::nullopt);
    const Bytecode& bytecode() const { return bytecode_; }
    void set_jit_enabled(bool enabled);
    void set_jit_config(jit::JitTieringConfig config);
    bool jit_enabled() const;
    jit::JitEngine& jit_engine();
    const jit::JitEngine& jit_engine() const;

    void set_debug_hook(IDebugHook* hook);
    IDebugHook* debug_hook() const { return debug_hook_; }
    bool debug_continue_execution();
    std::size_t debug_instruction_pointer() const;
    const Bytecode& debug_active_bytecode() const;
    std::size_t debug_call_depth() const;
    std::vector<debug::DebugStackFrame> debug_stack_frames() const;
    std::optional<Value> debug_global_variable(const std::string& name) const;
    Value debug_invoke_callable(const Value& callable, const std::vector<Value>& args);
    void prepare_runtime_invocation_state();
    Value invoke_callable_isolated(const Value& callable, const std::vector<Value>& args);
    std::optional<Value> debug_function_value(const std::string& name) const;
    bool uses_async_scheduler() const;
    bool process_exit_requested() const;
    int process_exit_code() const;

    bool jit_execute_call_value(jit::JitExecState& state, std::uint32_t dest, std::uint32_t callee_reg,
                                std::uint32_t argc, jit::JitCallIcSlot* ic_slot);
    bool jit_invoke_direct_call(jit::JitExecState& state, std::uint32_t dest, std::uint32_t first_arg_reg,
                                std::uint32_t callee_index, jit::JitCallIcSlot* ic_slot,
                                const ClosureData* closure = nullptr);
    bool jit_invoke_nested_from_entry(jit::JitExecState& caller, std::uint32_t dest, std::uint32_t first_arg_reg,
                                      const jit::JitCallIcEntry& entry, const FunctionInfo& callee_fn,
                                      std::uint32_t callee_index,
                                      const std::vector<std::shared_ptr<Cell>>* closure_upvalues = nullptr);
    jit::JitExecState* prepare_nested_mono_callee(jit::JitExecState& caller, std::uint32_t first_arg_reg,
                                                  const jit::JitCallIcEntry& entry, std::uint32_t callee_index,
                                                  const FunctionInfo& callee_fn,
                                                  const std::vector<std::shared_ptr<Cell>>* closure_upvalues);
    void push_nested_jit_scratch_caller();
    void pop_nested_jit_scratch_caller();
    bool has_nested_jit_scratch_saved() const { return !nested_jit_scratch_stack_.empty(); }
    void publish_nested_mono_call_result(jit::JitExecState& caller, std::uint32_t dest, const Value& value);
    void commit_nested_mono_result_to_saved_caller(std::uint32_t dest, const Value& value);
    void notify_nested_mono_call_failed(jit::JitExecState* callee = nullptr);
    void populate_jit_exec_state(jit::JitExecState& state, const FunctionInfo& fn,
                                 std::uint32_t function_index) const;
    void init_locals_for_function(std::uint32_t local_count);

private:
    void run_sync_loop();
    void run_top_level_init();
    void run_async_scheduler();
    void run_async_scheduler_loop();
    void finish_async_scheduler_shutdown();
    void drain_background_tasks_after_sync_program();
    void finish_current_async_task(Value result, std::uint32_t module_index);
    void enqueue_task(std::shared_ptr<TaskData> task);
    void track_task(const std::shared_ptr<TaskData>& task);
    bool is_task_runnable(const TaskData& task) const;
    void check_scheduler_shutdown();
    void prune_completed_timers();
    void release_terminal_task_resources(TaskData& task);
    void prune_tracked_tasks();
    void run_task_until_yield(std::shared_ptr<TaskData> task);
    void save_current_execution_state(const std::shared_ptr<TaskData>& task, std::uint32_t await_dest);
    void restore_execution_state(const AsyncExecutionState& state);
    void complete_task(const std::shared_ptr<TaskData>& task, Value result);
    void fail_task(const std::shared_ptr<TaskData>& task, Value exception);
    void cancel_task(const std::shared_ptr<TaskData>& task, Value exception);
    void request_task_cancellation(const std::shared_ptr<TaskData>& task);
    void execute_task_cancellation(const std::shared_ptr<TaskData>& task);
    void finish_race_meta_task(const std::shared_ptr<TaskData>& task);
    void finish_wait_all_meta_task(const std::shared_ptr<TaskData>& task);
    void propagate_contestant_to_wait_all_metas(const std::shared_ptr<TaskData>& contestant, const Value& exception,
                                                bool cancelled);
    void finish_timeout_meta_task(const std::shared_ptr<TaskData>& task);
    void finish_timeout_on_timer(const std::shared_ptr<TaskData>& meta);
    void disarm_timeout_timer(const std::shared_ptr<TaskData>& meta);
    std::shared_ptr<TaskData> create_race_task(const std::vector<Value>& contestants);
    std::shared_ptr<TaskData> create_wait_all_task(const std::vector<Value>& contestants);
    std::shared_ptr<TaskData> create_timeout_task(const std::shared_ptr<TaskData>& target, std::int64_t milliseconds);
    void cancel_task_group_siblings(const std::shared_ptr<TaskData>& failed_task);
    void cancel_owned_task_groups(const std::shared_ptr<TaskData>& owner);
    void push_task_group(const std::shared_ptr<TaskGroupData>& group);
    void pop_task_group(const std::shared_ptr<TaskGroupData>& group);
    void track_task_group_child(const std::shared_ptr<TaskData>& child);
    std::shared_ptr<TaskData> create_completed_value_task(Value value);
    void remove_task_waiter(const std::shared_ptr<TaskData>& awaited, const std::shared_ptr<TaskData>& waiter);
    bool is_task_cancellable(const TaskData& task) const;
    bool is_task_terminal(const TaskData& task) const;
    bool is_timeout_timer_task(const TaskData& task) const;
    Value make_cancellation_exception(const std::string& message);
    Value make_timeout_exception(const std::string& message);
    Value make_io_task_exception(RuntimeErrorCategory category, const std::string& message);
    bool is_cancellation_exception(const Value& value) const;
    void wake_task_waiters(const std::shared_ptr<TaskData>& task);
    void execute_await(const Instruction& instruction);
    void ensure_task_io_started(const std::shared_ptr<TaskData>& task);
    void spawn_async_function(const FunctionInfo& fn, std::uint32_t function_index, std::vector<Value> args,
                              std::vector<std::shared_ptr<Cell>> captures, std::uint32_t dest,
                              std::uint32_t module_index, std::optional<Value> implicit_this = {});
    std::shared_ptr<TaskData> create_timer_task(std::int64_t milliseconds);
    void process_ready_timers();
    void process_io_completions();
    void process_callable_invokes();
    void bind_native_call_site();
    void clear_native_call_site_binding();
    bool scheduler_has_pending_work() const;
    void wait_for_scheduler_wake();
    const FunctionInfo& task_function(const TaskData& task) const;
    void start_task_execution(const std::shared_ptr<TaskData>& task);
    void resume_task_after_await(const std::shared_ptr<TaskData>& task);
    Value invoke_callable_for_runtime(const Value& callable, const std::vector<Value>& args);
    void run_scheduler_until_task_done(const std::shared_ptr<TaskData>& task);
    int run_discovered_tests(const std::optional<std::string>& filter);
    void ensure_scheduler_for_testing();
    void setup_run_session();
    void finish_run_session();

    const Value& read_register(std::uint32_t index) const;
    Value& write_register(std::uint32_t index);
    std::int64_t cached_integer_constant(std::uint32_t index);
    static void assign_register_integer(Value& dest, std::int64_t value);
    static bool try_fast_integer_arith(Opcode opcode, const Value& left, const Value& right, Value& dest);
    Value load_constant(std::uint32_t index);
    const Value& read_variable(std::uint32_t index) const;
    void write_variable(std::uint32_t index, Value value);
    const Value& read_local(std::uint32_t index) const;
    void write_local(std::uint32_t index, Value value);
    const Value& read_upvalue(std::uint32_t index) const;
    void write_upvalue(std::uint32_t index, Value value);
    void call_function(const Instruction& instruction);
    void execute_call_builtin(const Instruction& instruction);
    void prepare_locals_for_function(const FunctionInfo& fn, std::uint32_t function_index);
    void release_leaving_locals(std::uint32_t function_index);
    void reset_nested_jit_for_run();
    bool function_allows_leaf_local_cache(const FunctionInfo& fn) const;
    bool try_take_leaf_local_slots(std::uint32_t function_index, std::uint32_t local_count);
    bool stash_leaf_local_slots(std::uint32_t function_index);
    std::shared_ptr<Cell> acquire_local_cell();
    void recycle_local_cells(std::vector<std::shared_ptr<Cell>>& cells);
    void enter_function_prologue(const FunctionInfo& fn, std::uint32_t function_index, std::size_t return_ip,
                                 std::uint32_t dest);
    void enter_function_epilogue(const FunctionInfo& fn, std::uint32_t function_index);
    void call_value(const Instruction& instruction);
    void enter_function(const FunctionInfo& fn, std::uint32_t function_index, std::vector<Value> args,
                        std::vector<std::shared_ptr<Cell>> captures, std::uint32_t dest, std::size_t return_ip,
                        std::optional<Value> implicit_this = {});
    bool try_execute_jit_for_entered_function(const FunctionInfo& fn, std::uint32_t function_index);
    bool resume_after_jit_deopt(jit::JitExecState& state, const FunctionInfo& fn, std::uint32_t function_index);
    bool try_jit_osr_at_loop(std::uint32_t loop_bytecode_ip);
    bool function_may_use_jit(std::uint32_t function_index);
    void finish_jit_return(Value result, const FunctionInfo& fn, std::uint32_t function_index);
    void invoke_class(const Instruction& instruction);
    void call_class_method(const Instruction& instruction);
    void invoke_super(const Instruction& instruction);
    void make_closure(const Instruction& instruction);
    void return_from_function(const Instruction& instruction);
    void enter_try(std::uint32_t handler_index);
    void leave_try(std::uint32_t handler_index);
    void throw_value(Value value);
    void raise_runtime_error(const RuntimeError& err);
    void resolve_builtin_exception_classes();
    std::vector<StackTraceFrame> capture_stack_trace(std::size_t ip) const;
    [[noreturn]] void fail_uncaught(Value value);
    void end_finally();
    void store_catch_binding(const CatchEntry& entry, Value value);
    void unwind_to_frame_depth(std::size_t depth);
    bool catch_entry_matches(const CatchEntry& entry, const Value& value, const Bytecode& handler_bc) const;
    void run_finally_for_handler(std::uint32_t handler_index);
    void defer_return(std::uint32_t handler_index, std::uint32_t src);
    void defer_break(std::uint32_t handler_index, std::uint32_t jump_index);
    void defer_continue(std::uint32_t handler_index, std::uint32_t jump_index);
    void complete_pending_control();
    const Instruction& fetch();
    void execute(const Instruction& instruction);
    bool step_one_instruction();
    void jump_to(std::uint32_t target);
    const Bytecode& code() const;
    const Bytecode& module_code(std::uint32_t module_index) const;
    void ensure_module_states();
    void initialize_module(std::uint32_t module_index);
    Value import_module_value(std::uint32_t module_index);
    Value import_export_value(std::uint32_t module_index, const std::string& export_name);
    Value make_module_export_value(std::uint32_t module_index, const ModuleExport& export_info);
    Value resolve_module_index_value(const Value& object, const Value& index);
    void enter_module_bound_function(const FunctionInfo& fn, std::uint32_t module_index, std::uint32_t function_index,
                                     std::vector<Value> args, std::vector<std::shared_ptr<Cell>> captures,
                                     std::uint32_t dest, std::size_t return_ip,
                                     std::optional<Value> implicit_this = {});
    void invoke_module_class_ctor(std::uint32_t module_index, std::uint32_t class_index, const std::vector<Value>& args,
                                  std::uint32_t dest);
    void invoke_module_struct_ctor(std::uint32_t module_index, std::uint32_t struct_index, std::uint32_t dest);
    Value make_module_enum_export(std::uint32_t module_index, const ModuleExport& export_info);
    void set_module_export_value(const Value& module_value, const std::string& export_name, Value value);
    void     begin_module_initialization(std::uint32_t module_index);

    struct ResolvedInstanceMethod {
        bool found = false;
        std::uint32_t module_index = 0xFFFFFFFFu;
        std::uint32_t function_index = 0;
        const FunctionInfo* fn = nullptr;
    };

    enum class MethodInvokeStatus { Returned, StopIteration, Exception, Awaiting };

    struct ProtocolInvokeResume {
        bool active = false;
        std::uint32_t dest_reg = 0;
        std::uint32_t exit_ip = 0;
        bool is_iter_next = false;
        bool catch_stop_iteration = false;
        std::size_t saved_handlers = 0;
    };

    ResolvedInstanceMethod lookup_instance_method(const Value& receiver, std::string_view method) const;
    MethodInvokeStatus invoke_instance_method_sync(const Value& receiver, std::string_view method,
                                                   std::vector<Value> args, Value* out,
                                                   bool catch_stop_iteration);
    void complete_protocol_invoke_success(Value result);
    void complete_protocol_invoke_stop_iteration();
    void complete_protocol_invoke_exception();
    void execute_get_iter(const Instruction& instruction);
    void execute_iter_next(const Instruction& instruction);

    static Value constant_to_value(const Constant& constant);
    static std::string decode_string_lexeme(const std::string& lexeme);
    static bool is_number(const Value& value);
    static double as_number(const Value& value);
    static bool values_equal(const Value& left, const Value& right);
    static Value unary_plus(const Value& value);
    static Value unary_minus(const Value& value);
    static Value bitwise_not(const Value& value);
    static Value binary_arith(Opcode opcode, const Value& left, const Value& right);
    static Value binary_bitwise(Opcode opcode, const Value& left, const Value& right);
    static Value binary_compare(Opcode opcode, const Value& left, const Value& right);
    static Value coerce_to_float(const Value& value);
    static Value check_assign_type(const Value& value, const TypeCheckInfo& check,
                                   const std::vector<ClassInfo>& classes,
                                   const std::vector<TypeCheckInfo>& type_checks);
    static bool is_class_or_subclass(std::uint32_t class_index, std::uint32_t ancestor_index,
                                     const std::vector<ClassInfo>& classes);
    static const char* runtime_type_name(ValueKind kind);

    [[noreturn]] static void fail(const std::string& message);

    enum class ModuleInitState { Unloaded, Loading, Loaded, Failed };

    struct ModuleRuntimeState {
        ModuleInitState state = ModuleInitState::Unloaded;
        std::vector<std::optional<Value>> globals;
        std::string failure_message;
    };

    struct CallFrame {
        std::size_t return_ip = 0;
        std::uint32_t dest = 0;
        std::uint32_t function_index = 0;
        bool runtime_invoke_root = false;
        std::uint32_t module_index = 0xFFFFFFFFu;
        const Bytecode* saved_code = nullptr;
        std::vector<std::optional<Value>> saved_variables;
        std::size_t register_base = 0;
        std::vector<std::optional<Value>> registers;
        std::vector<std::shared_ptr<Cell>> locals;
        std::vector<std::shared_ptr<Cell>> upvalues;
        std::vector<Value> defer_stack;
    };

    void run_function_defers(std::vector<Value>& defers);
    void invoke_sync_function_value(const Value& callee, std::vector<Value> args);

    void restore_module_context(CallFrame& frame);
    void restore_call_frame_state(CallFrame& frame);

    struct HandlerFrame {
        std::uint32_t handler_index = 0;
        std::size_t frame_depth = 0;
        std::uint32_t next_catch = 0;
        const Bytecode* owner_code = nullptr;
    };

    const Bytecode& handler_bytecode(const HandlerFrame& frame) const;

    enum class PendingControlKind { None, Return, Break, Continue };

    struct PendingControl {
        PendingControlKind kind = PendingControlKind::None;
        Value return_value;
        std::uint32_t jump_index = 0;
    };

    static constexpr std::uint32_t kInvalidHandlerIndex = 0xFFFFFFFFu;
    static constexpr std::size_t kMaxCallStackDepth = limits::kMaxCallStackDepth;

    void ensure_call_stack_capacity();
    void ensure_jit_engine_bound();
    void prepare_jit_callee_locals(jit::JitExecState& callee, const FunctionInfo& callee_fn,
                                   const jit::JitExecState& caller, std::uint32_t first_arg_reg);
    void mirror_jit_callee_args_to_registers(jit::JitExecState& callee, const FunctionInfo& callee_fn,
                                             const jit::JitExecState& caller, std::uint32_t first_arg_reg);
    void update_nested_mono_callee_args(jit::JitExecState& callee, const FunctionInfo& callee_fn,
                                        const jit::JitExecState& caller, std::uint32_t first_arg_reg);
    bool try_nested_mono_fast_integer_bind(jit::JitExecState& callee, const FunctionInfo& callee_fn,
                                           const jit::JitExecState& caller, std::uint32_t first_arg_reg);
    void invalidate_nested_mono_scratch_cache();
    void release_jit_callee_locals(jit::JitExecState& callee);
    jit::JitExecState& nested_jit_call_scratch();
    bool run_jit_nested_call(jit::JitExecState& caller, const jit::JitCompiledCode& code, const FunctionInfo& callee_fn,
                             std::uint32_t callee_index, std::uint32_t first_arg_reg, std::uint32_t instruction_count,
                             Value& out_result, const std::vector<std::shared_ptr<Cell>>* closure_upvalues = nullptr);
    bool jit_invoke_call_from_interpreter(jit::JitExecState& state, std::uint32_t dest, std::uint32_t first_arg_reg,
                                          std::uint32_t callee_index);
    bool jit_invoke_call_value_from_interpreter(jit::JitExecState& state, std::uint32_t dest, std::uint32_t callee_reg,
                                                std::uint32_t argc);
    bool dispatch_jit_execution_result(jit::JitExecState& state, const FunctionInfo& fn, std::uint32_t function_index,
                                       jit::JitExecuteResult result);
    RuntimeProfiler* profiler_if_enabled();
    RuntimeProfiler* session_profiler_ = nullptr;
    std::uint32_t current_profile_function_index() const;
    void record_call_site_profile();
    std::uint32_t current_register_count() const;
    void clear_register_window(std::uint32_t register_count);

    Bytecode bytecode_;
    std::unique_ptr<Runtime> owned_runtime_;
    Runtime* runtime_ = nullptr;
    const Bytecode* integer_constant_cache_owner_ = nullptr;
    std::vector<std::int64_t> integer_constant_cache_;
    std::vector<std::uint8_t> integer_constant_ready_;
    std::vector<std::optional<Value>> registers_;
    std::size_t register_base_ = 0;
    std::vector<std::optional<Value>> variables_;
    std::vector<std::shared_ptr<Cell>> locals_;
    std::vector<std::shared_ptr<Cell>> local_cell_pool_;
    std::vector<std::vector<std::shared_ptr<Cell>>> leaf_local_slots_;
    static constexpr std::size_t kMaxLocalCellPoolSize = 8192;
    std::vector<std::shared_ptr<Cell>> upvalues_;
    std::vector<CallFrame> frames_;
    std::vector<HandlerFrame> handler_stack_;
    std::optional<Value> pending_exception_;
    PendingControl pending_control_;
    std::optional<std::size_t> protocol_invoke_boundary_;
    bool protocol_catch_stop_iteration_ = false;
    bool protocol_stop_iteration_ = false;
    ProtocolInvokeResume protocol_invoke_resume_;
    BuiltinExceptionClasses builtin_exceptions_;
    const Bytecode* execution_bytecode_ = nullptr;
    std::vector<ModuleRuntimeState> module_states_;
    std::size_t ip_ = 0;
    std::uint64_t instruction_steps_ = 0;
    bool halted_ = false;
    bool scheduler_active_ = false;
    bool task_yield_ = false;
    std::shared_ptr<TaskData> current_task_;
    std::shared_ptr<TaskData> root_task_;
    std::deque<std::shared_ptr<TaskData>> run_queue_;
    std::vector<std::shared_ptr<TaskData>> timer_tasks_;
    std::vector<std::shared_ptr<TaskData>> tracked_tasks_;
    std::vector<std::shared_ptr<TaskData>> deferred_background_tasks_;
    std::vector<std::optional<Value>> scheduler_globals_;
    std::vector<std::shared_ptr<TaskGroupData>> task_group_stack_;
    std::uint32_t runtime_callable_sandbox_depth_ = 0;
    bool force_runtime_invoke_sandbox_ = false;
    bool jit_enabled_ = true;
    std::optional<jit::JitTieringConfig> jit_config_;
    std::unique_ptr<jit::JitEngine> jit_engine_;
    std::vector<std::uint8_t> jit_eligibility_hints_;
    std::vector<std::uint32_t> jit_ir_instruction_counts_;
    std::string jit_program_fingerprint_;
    bool jit_program_fingerprint_ready_ = false;
    jit::JitExecState nested_jit_call_scratch_;
    std::vector<jit::JitExecState> nested_jit_scratch_stack_;
    std::uint32_t nested_jit_scratch_local_count_ = 0xFFFFFFFFu;
    bool nested_mono_scratch_warm_ = false;
    std::uint32_t nested_mono_cache_fn_index_ = 0xFFFFFFFFu;
    const void* nested_mono_cache_entry_ = nullptr;
    const void* nested_mono_cache_closure_token_ = nullptr;
    std::uint32_t nested_mono_cache_reg_count_ = 0;
    std::uint32_t nested_mono_cache_local_count_ = 0xFFFFFFFFu;
    struct CallValueMonoCacheEntry {
        const ClosureData* closure_token = nullptr;
        std::uint32_t function_index = 0xFFFFFFFFu;
    };
    std::vector<CallValueMonoCacheEntry> callvalue_mono_cache_;
    std::uint32_t jit_vm_fallback_depth_ = 0;
    std::uint32_t jit_native_completion_gate_function_ = 0xFFFFFFFFu;
    std::uint32_t jit_native_completion_gate_frame_depth_ = 0xFFFFFFFFu;
    bool debug_execution_paused_ = false;
    IDebugHook* debug_hook_ = nullptr;
};

}  // namespace pscript
