#include "runtime/builtin_id.h"
#include "vm/vm.h"

#include "compiler/pgo.h"
#include "jit/jit_runtime.h"
#include "runtime/runtime_debug.h"

#include <new>

#include "runtime/task_group.h"
#include "stdlib/native_async_stream.h"
#include "stdlib/stdlib.h"
#include "vm/exception_support.h"
#include "runtime/alloc_guard.h"
#include "runtime/async_io.h"
#include "runtime/profiler.h"
#include "runtime/runtime_limits.h"
#include "runtime/http_ops.h"
#include "runtime/iterator.h"
#include "stdlib/stdlib.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <optional>
#include <cstdint>
#include <memory>
#include <ostream>
#include <thread>
#include <utility>
#include <vector>

namespace pscript {
namespace {

std::int64_t parse_integer(const std::string& lexeme) {
    try {
        std::size_t idx = 0;
        const long long value = std::stoll(lexeme, &idx, 10);
        if (idx != lexeme.size()) {
            throw VmError("invalid bytecode");
        }
        return static_cast<std::int64_t>(value);
    } catch (const VmError&) {
        throw;
    } catch (...) {
        throw VmError("invalid bytecode");
    }
}

double parse_float(const std::string& lexeme) {
    try {
        std::size_t idx = 0;
        const double value = std::stod(lexeme, &idx);
        if (idx != lexeme.size()) {
            throw VmError("invalid bytecode");
        }
        return value;
    } catch (const VmError&) {
        throw;
    } catch (...) {
        throw VmError("invalid bytecode");
    }
}

[[noreturn]] void user_runtime_error(const std::string& message) {
    throw RuntimeError(message);
}

[[noreturn]] void user_runtime_error(const std::string& message, RuntimeErrorCategory category) {
    throw RuntimeError(message, category);
}

template <typename Raise>
void absorb_foreign_exception(Raise&& raise) {
    try {
        throw;
    } catch (const RuntimeError& err) {
        raise(err);
    } catch (const VmError&) {
        throw;
    } catch (const ProcessExit&) {
        throw;
    } catch (const std::bad_alloc&) {
        raise(RuntimeError("out of memory"));
    } catch (const std::exception& ex) {
        raise(RuntimeError(ex.what()));
    }
}

std::string reflection_type_name(const Bytecode& bytecode, const Value& value) {
    if (value.kind == ValueKind::Struct && value.struct_value) {
        return value.struct_value->type_name;
    }
    if (value.kind == ValueKind::Class && value.class_value) {
        return value.class_value->type_name;
    }
    if (value.kind == ValueKind::Function && value.function) {
        if (value.function->function_index < bytecode.functions.size()) {
            return bytecode.functions[value.function->function_index].name;
        }
    }
    switch (value.kind) {
        case ValueKind::Integer:
            return "int";
        case ValueKind::Float:
            return "float";
        case ValueKind::Bool:
            return "bool";
        case ValueKind::String:
            return "string";
        case ValueKind::Null:
            return "null";
        case ValueKind::List:
            return "list";
        case ValueKind::Dict:
            return "dict";
        case ValueKind::Tuple:
            return "tuple";
        case ValueKind::Set:
            return "set";
        case ValueKind::Struct:
            return "struct";
        case ValueKind::Class:
            return "class";
        case ValueKind::Enum:
            return "enum";
        case ValueKind::Iterator:
            return "iterator";
        case ValueKind::Function:
            return "function";
        case ValueKind::Task:
            return "task";
        default:
            return "unknown";
    }
}

Value reflection_annotations(const Bytecode& bytecode, const Value& value) {
    std::vector<Value> items;
    const auto push_annotation = [&](const AnnotationInfo& annotation) {
        std::vector<Value> entry;
        entry.push_back(Runtime::make_string(annotation.name));
        std::vector<Value> args;
        for (const auto& arg : annotation.args) {
            std::vector<Value> pair;
            if (arg.first) {
                pair.push_back(Runtime::make_string(*arg.first));
            } else {
                pair.push_back(Runtime::make_null());
            }
            pair.push_back(Runtime::make_string(arg.second));
            args.push_back(Runtime::make_tuple(std::move(pair)));
        }
        entry.push_back(Runtime::make_list(std::move(args)));
        items.push_back(Runtime::make_list(std::move(entry)));
    };
    if (value.kind == ValueKind::Function && value.function &&
        value.function->function_index < bytecode.functions.size()) {
        for (const AnnotationInfo& annotation :
             bytecode.functions[value.function->function_index].annotations) {
            push_annotation(annotation);
        }
    } else if (value.kind == ValueKind::Class && value.class_value &&
               value.class_value->type_index < bytecode.classes.size()) {
        for (const AnnotationInfo& annotation : bytecode.classes[value.class_value->type_index].annotations) {
            push_annotation(annotation);
        }
    } else if (value.kind == ValueKind::Struct && value.struct_value &&
               value.struct_value->type_index < bytecode.structs.size()) {
        for (const AnnotationInfo& annotation : bytecode.structs[value.struct_value->type_index].annotations) {
            push_annotation(annotation);
        }
    }
    return Runtime::make_list(std::move(items));
}

Value reflection_members(const Bytecode& bytecode, const Value& value) {
    std::vector<Value> items;
    if (value.kind == ValueKind::Class && value.class_value &&
        value.class_value->type_index < bytecode.classes.size()) {
        const ClassInfo& info = bytecode.classes[value.class_value->type_index];
        for (const StructFieldInfo& field : info.fields) {
            items.push_back(Runtime::make_string(field.name));
        }
        for (const ClassMethodInfo& method : info.methods) {
            items.push_back(Runtime::make_string(method.name));
        }
        for (const ClassPropertyInfo& property : info.properties) {
            items.push_back(Runtime::make_string(property.name));
        }
    } else if (value.kind == ValueKind::Struct && value.struct_value &&
               value.struct_value->type_index < bytecode.structs.size()) {
        for (const StructFieldInfo& field : bytecode.structs[value.struct_value->type_index].fields) {
            items.push_back(Runtime::make_string(field.name));
        }
    }
    return Runtime::make_list(std::move(items));
}

}  // namespace

VmError::VmError(std::string message)
    : std::runtime_error(message), message_(std::move(message)) {}

Vm::~Vm() = default;

Vm::Vm(Bytecode bytecode, Runtime& runtime)
    : bytecode_(std::move(bytecode)), runtime_(&runtime) {}

Vm::Vm(Bytecode bytecode, std::ostream& output)
    : bytecode_(std::move(bytecode)),
      owned_runtime_(std::make_unique<Runtime>(output)),
      runtime_(owned_runtime_.get()) {}

RuntimeProfiler* Vm::profiler_if_enabled() {
    if (runtime_ == nullptr) {
        return nullptr;
    }
    RuntimeProfiler& profiler = runtime_->profiler();
    return profiler.enabled() ? &profiler : nullptr;
}

std::uint32_t Vm::current_profile_function_index() const {
    if (frames_.empty()) {
        return 0xFFFFFFFFu;
    }
    return frames_.back().function_index;
}

void Vm::record_call_site_profile() {
    if (session_profiler_ == nullptr || ip_ == 0) {
        return;
    }
    session_profiler_->on_call_site(current_profile_function_index(), static_cast<std::uint32_t>(ip_ - 1));
}

std::shared_ptr<Cell> Vm::acquire_local_cell() {
    alloc_guard::throw_if_blocked(1);
    if (!local_cell_pool_.empty()) {
        std::shared_ptr<Cell> cell = std::move(local_cell_pool_.back());
        local_cell_pool_.pop_back();
        cell->value = std::nullopt;
        return cell;
    }
    return std::make_shared<Cell>();
}

void Vm::recycle_local_cells(std::vector<std::shared_ptr<Cell>>& cells) {
    for (std::shared_ptr<Cell>& cell : cells) {
        if (!cell) {
            continue;
        }
        // Closures may still hold captures pointing at these cells.
        if (cell.use_count() != 1) {
            continue;
        }
        cell->value = std::nullopt;
        if (local_cell_pool_.size() < kMaxLocalCellPoolSize) {
            local_cell_pool_.push_back(std::move(cell));
        }
    }
    cells.clear();
}

void Vm::ensure_jit_engine_bound() {
    if (!jit_program_fingerprint_ready_) {
        jit_program_fingerprint_ = compute_program_fingerprint(bytecode_);
        jit_program_fingerprint_ready_ = true;
    }
    jit_engine().bind_bytecode(&bytecode_, jit_program_fingerprint_);
}

void Vm::invalidate_nested_mono_scratch_cache() {
    nested_mono_scratch_warm_ = false;
    nested_mono_cache_fn_index_ = 0xFFFFFFFFu;
    nested_mono_cache_entry_ = nullptr;
    nested_mono_cache_closure_token_ = nullptr;
    nested_mono_cache_reg_count_ = 0;
    nested_mono_cache_local_count_ = 0xFFFFFFFFu;
}

void Vm::reset_nested_jit_for_run() {
    jit_native_completion_gate_function_ = 0xFFFFFFFFu;
    jit_native_completion_gate_frame_depth_ = 0xFFFFFFFFu;
    if (jit_engine_) {
        jit_engine_->reset_native_jit_completions();
    }
    invalidate_nested_mono_scratch_cache();
    nested_jit_scratch_stack_.clear();
    nested_jit_call_scratch_.completed = false;
    nested_jit_call_scratch_.fallback = false;
    nested_jit_call_scratch_.deopt_requested = false;
    nested_jit_call_scratch_.jit_call_depth = 0;
    nested_jit_call_scratch_.return_value = Value{};
    nested_jit_call_scratch_.current_insn = 0;
    nested_jit_call_scratch_.host_vm = this;
    nested_jit_call_scratch_.runtime = runtime_;
    nested_jit_call_scratch_.bytecode = execution_bytecode_;
}

void Vm::prepare_jit_callee_locals(jit::JitExecState& callee, const FunctionInfo& callee_fn,
                                   const jit::JitExecState& caller, std::uint32_t first_arg_reg) {
    if (callee.ic_metrics != nullptr) {
        ++callee.ic_metrics->prepare_jit_callee_locals_calls;
    }
    callee.locals.resize(callee_fn.local_count);
    for (std::uint32_t i = callee_fn.arity; i < callee_fn.local_count; ++i) {
        if (!callee.locals[i]) {
            callee.locals[i] = acquire_local_cell();
        } else {
            callee.locals[i]->value = std::nullopt;
        }
    }
    for (std::uint32_t i = 0; i < callee_fn.arity; ++i) {
        if (!callee.locals[i]) {
            callee.locals[i] = acquire_local_cell();
        }
        const std::uint32_t reg = first_arg_reg + i;
        if (reg >= caller.registers.size()) {
            callee.locals[i]->value = Runtime::make_null();
            continue;
        }
        const Value& arg = caller.registers[reg];
        std::optional<Value>& cell = callee.locals[i]->value;
        if (arg.kind == ValueKind::Integer && cell && cell->kind == ValueKind::Integer) {
            cell->integer = arg.integer;
        } else {
            cell = arg;
        }
    }
}

void Vm::release_jit_callee_locals(jit::JitExecState& callee) {
    recycle_local_cells(callee.locals);
}

jit::JitExecState& Vm::nested_jit_call_scratch() {
    return nested_jit_call_scratch_;
}

void Vm::push_nested_jit_scratch_caller() {
    nested_jit_scratch_stack_.push_back(std::move(nested_jit_call_scratch_));
}

void Vm::pop_nested_jit_scratch_caller() {
    if (nested_jit_scratch_stack_.empty()) {
        return;
    }
    nested_jit_call_scratch_ = std::move(nested_jit_scratch_stack_.back());
    nested_jit_scratch_stack_.pop_back();
}

void Vm::publish_nested_mono_call_result(jit::JitExecState& caller, std::uint32_t dest, const Value& value) {
    if (dest >= caller.registers.size()) {
        caller.registers.resize(static_cast<std::size_t>(dest) + 1);
    }
    Value published = value;
    if (caller.jit_call_depth == 0 && published.kind == ValueKind::Function && published.function &&
        !published.function->captures.empty()) {
        published = Runtime::clone_closure_value(published);
    }
    Value& slot = caller.registers[dest];
    if (published.kind == ValueKind::Integer) {
        slot.kind = ValueKind::Integer;
        slot.integer = published.integer;
    } else if (published.kind == ValueKind::Float) {
        slot.kind = ValueKind::Float;
        slot.floating = published.floating;
    } else {
        slot = published;
    }
    jit::refresh_register_data(caller);
}

void Vm::commit_nested_mono_result_to_saved_caller(std::uint32_t dest, const Value& value) {
    if (nested_jit_scratch_stack_.empty()) {
        return;
    }
    jit::JitExecState& saved = nested_jit_scratch_stack_.back();
    publish_nested_mono_call_result(saved, dest, value);
    pop_nested_jit_scratch_caller();
}

void Vm::update_nested_mono_callee_args(jit::JitExecState& callee, const FunctionInfo& callee_fn,
                                        const jit::JitExecState& caller, std::uint32_t first_arg_reg) {
    if (callee_fn.local_count > 0) {
        prepare_jit_callee_locals(callee, callee_fn, caller, first_arg_reg);
    }
    mirror_jit_callee_args_to_registers(callee, callee_fn, caller, first_arg_reg);
}

bool Vm::try_nested_mono_fast_integer_bind(jit::JitExecState& callee, const FunctionInfo& callee_fn,
                                           const jit::JitExecState& caller, std::uint32_t first_arg_reg) {
    if (callee_fn.arity == 0 || callee_fn.arity > 8 || callee_fn.local_count < callee_fn.arity) {
        return false;
    }
    if (first_arg_reg + callee_fn.arity > caller.registers.size()) {
        return false;
    }
    for (std::uint32_t i = 0; i < callee_fn.arity; ++i) {
        if (caller.registers[first_arg_reg + i].kind != ValueKind::Integer) {
            return false;
        }
    }
    callee.locals.resize(callee_fn.local_count);
    for (std::uint32_t i = 0; i < callee_fn.arity; ++i) {
        if (!callee.locals[i]) {
            callee.locals[i] = acquire_local_cell();
        }
        const std::int64_t arg_int = caller.registers[first_arg_reg + i].integer;
        std::optional<Value>& cell = callee.locals[i]->value;
        if (!cell) {
            cell = Runtime::make_integer(arg_int);
        } else {
            cell->kind = ValueKind::Integer;
            cell->integer = arg_int;
        }
    }
    for (std::uint32_t i = callee_fn.arity; i < callee_fn.local_count; ++i) {
        if (!callee.locals[i]) {
            callee.locals[i] = acquire_local_cell();
        } else {
            callee.locals[i]->value = std::nullopt;
        }
    }
    const std::uint32_t needed = std::max(callee_fn.register_count, callee_fn.arity);
    if (callee.registers.size() < needed) {
        callee.registers.resize(needed);
    }
    for (std::uint32_t i = 0; i < callee_fn.arity; ++i) {
        Value& dest = callee.registers[i];
        dest.kind = ValueKind::Integer;
        dest.integer = caller.registers[first_arg_reg + i].integer;
    }
    for (std::uint32_t i = callee_fn.arity; i < needed; ++i) {
        callee.registers[i] = Value{};
    }
    jit::refresh_register_data(callee);
    return true;
}

void Vm::mirror_jit_callee_args_to_registers(jit::JitExecState& callee, const FunctionInfo& callee_fn,
                                             const jit::JitExecState& caller, std::uint32_t first_arg_reg) {
    if (callee.ic_metrics != nullptr) {
        ++callee.ic_metrics->mirror_jit_callee_args_calls;
    }
    if (callee_fn.arity == 0) {
        return;
    }
    const std::uint32_t needed = std::max(callee_fn.register_count, callee_fn.arity);
    if (callee.registers.size() < needed) {
        callee.registers.resize(needed);
        jit::refresh_register_data(callee);
    }
    if (callee_fn.arity == 1 && first_arg_reg < caller.registers.size()) {
        const Value& arg = caller.registers[first_arg_reg];
        Value& dest = callee.registers[0];
        if (arg.kind == ValueKind::Integer) {
            dest.kind = ValueKind::Integer;
            dest.integer = arg.integer;
        } else if (arg.kind == ValueKind::Float) {
            dest.kind = ValueKind::Float;
            dest.floating = arg.floating;
        } else {
            dest = arg;
        }
        return;
    }
    for (std::uint32_t i = 0; i < callee_fn.arity; ++i) {
        const std::uint32_t arg_reg = first_arg_reg + i;
        if (arg_reg >= caller.registers.size()) {
            callee.registers[i] = Runtime::make_null();
            continue;
        }
        const Value& arg = caller.registers[arg_reg];
        Value& dest = callee.registers[i];
        if (arg.kind == ValueKind::Integer) {
            dest.kind = ValueKind::Integer;
            dest.integer = arg.integer;
        } else if (arg.kind == ValueKind::Float) {
            dest.kind = ValueKind::Float;
            dest.floating = arg.floating;
        } else {
            dest = arg;
        }
    }
}

namespace {

void bind_local_argument(const std::shared_ptr<Cell>& cell, const Value& arg) {
    if (!cell) {
        return;
    }
    std::optional<Value>& dest = cell->value;
    if (arg.kind == ValueKind::Integer) {
        if (!dest.has_value()) {
            dest = Value{};
            dest->kind = ValueKind::Integer;
            dest->integer = arg.integer;
        } else if (dest->kind == ValueKind::Integer) {
            dest->integer = arg.integer;
        } else {
            dest = arg;
        }
        return;
    }
    if (arg.kind == ValueKind::Float) {
        if (!dest.has_value()) {
            dest = Value{};
            dest->kind = ValueKind::Float;
            dest->floating = arg.floating;
        } else if (dest->kind == ValueKind::Float) {
            dest->floating = arg.floating;
        } else {
            dest = arg;
        }
        return;
    }
    dest = arg;
}

}  // namespace

void Vm::init_locals_for_function(std::uint32_t local_count) {
    locals_.resize(local_count);
    for (std::uint32_t i = 0; i < local_count; ++i) {
        locals_[i] = acquire_local_cell();
    }
}

bool Vm::function_allows_leaf_local_cache(const FunctionInfo& fn) const {
    return fn.captures.empty() && fn.local_count == fn.arity && !fn.is_async && !fn.is_native && !fn.is_constructor;
}

bool Vm::try_take_leaf_local_slots(std::uint32_t function_index, std::uint32_t local_count) {
    if (function_index >= leaf_local_slots_.size()) {
        return false;
    }
    std::vector<std::shared_ptr<Cell>>& cached = leaf_local_slots_[function_index];
    if (cached.size() != local_count) {
        return false;
    }
    for (const std::shared_ptr<Cell>& cell : cached) {
        if (!cell) {
            return false;
        }
    }
    locals_ = std::move(cached);
    return true;
}

bool Vm::stash_leaf_local_slots(std::uint32_t function_index) {
    if (locals_.empty()) {
        return true;
    }
    if (function_index >= code().functions.size()) {
        recycle_local_cells(locals_);
        return false;
    }
    const FunctionInfo& fn = code().functions[function_index];
    if (!function_allows_leaf_local_cache(fn)) {
        recycle_local_cells(locals_);
        return false;
    }
    if (function_index >= leaf_local_slots_.size()) {
        leaf_local_slots_.resize(static_cast<std::size_t>(function_index) + 1);
    }
    leaf_local_slots_[function_index] = std::move(locals_);
    return true;
}

void Vm::prepare_locals_for_function(const FunctionInfo& fn, std::uint32_t function_index) {
    if (function_allows_leaf_local_cache(fn) && try_take_leaf_local_slots(function_index, fn.local_count)) {
        return;
    }
    init_locals_for_function(fn.local_count);
}

void Vm::release_leaving_locals(std::uint32_t function_index) {
    if (locals_.empty()) {
        return;
    }
    if (function_index < code().functions.size()) {
        const FunctionInfo& fn = code().functions[function_index];
        if (function_allows_leaf_local_cache(fn) && stash_leaf_local_slots(function_index)) {
            return;
        }
    }
    recycle_local_cells(locals_);
}

std::uint32_t Vm::current_register_count() const {
    if (frames_.empty()) {
        return code().register_count;
    }
    const std::uint32_t index = frames_.back().function_index;
    if (index >= code().functions.size()) {
        fail("invalid function index");
    }
    return code().functions[index].register_count;
}

void Vm::clear_register_window(std::uint32_t register_count) {
    const std::size_t needed = register_base_ + register_count;
    if (registers_.size() < needed) {
        registers_.resize(needed);
    }
    for (std::uint32_t i = 0; i < register_count; ++i) {
        registers_[register_base_ + i] = std::nullopt;
    }
}

void Vm::enter_function_prologue(const FunctionInfo& fn, std::uint32_t function_index, std::size_t return_ip,
                                 std::uint32_t dest) {
    ensure_call_stack_capacity();
    const std::uint32_t caller_register_count = current_register_count();
    CallFrame frame;
    frame.return_ip = return_ip;
    frame.dest = dest;
    frame.function_index = function_index;
    frame.register_base = register_base_;
    frame.locals = std::move(locals_);
    frame.upvalues = std::move(upvalues_);
    frames_.push_back(std::move(frame));

    register_base_ += caller_register_count;
    clear_register_window(fn.register_count);
    prepare_locals_for_function(fn, function_index);
}

void Vm::enter_function_epilogue(const FunctionInfo& fn, std::uint32_t function_index) {
    if (session_profiler_ != nullptr) {
        session_profiler_->on_function_enter(function_index);
    }
    if (debug_hook_ == nullptr && jit_enabled_ && jit_vm_fallback_depth_ == 0 && frames_.size() <= 64 &&
        !fn.is_native) {
        ensure_jit_engine_bound();
        jit_engine().record_invocation(function_index, fn);
        function_may_use_jit(function_index);
        if (try_execute_jit_for_entered_function(fn, function_index)) {
            return;
        }
    }
    jump_to(fn.entry);
    if (debug_hook_ != nullptr) {
        debug_hook_->on_function_enter(*this, function_index);
    }
}

void Vm::setup_run_session() {
    execution_bytecode_ = &bytecode_;
    reset_nested_jit_for_run();
    jit_vm_fallback_depth_ = 0;
    if (const auto error = bytecode_.validate()) {
        fail(*error);
    }

    ensure_module_states();
    register_base_ = 0;
    registers_.assign(bytecode_.register_count, std::nullopt);
    variables_.assign(bytecode_.variables.size(), std::nullopt);
    locals_.clear();
    leaf_local_slots_.clear();
    callvalue_mono_cache_.assign(bytecode_.instructions.size(), {});
    upvalues_.clear();
    frames_.clear();
    handler_stack_.clear();
    pending_exception_ = std::nullopt;
    pending_control_.kind = PendingControlKind::None;
    ip_ = 0;
    halted_ = false;
    instruction_steps_ = 0;
    debug_execution_paused_ = false;
    jit_eligibility_hints_.clear();
    resolve_builtin_exception_classes();

    if (runtime_ != nullptr) {
        runtime_->set_callable_invoker(
            [this](const Value& callable, const std::vector<Value>& args) { return invoke_callable_for_runtime(callable, args); });
        runtime_->set_parallel_thread_invoker_factory([this]() {
            Bytecode worker_bytecode = bytecode_;
            Runtime* worker_runtime = runtime_;
            auto worker_vm = std::make_shared<Vm>(std::move(worker_bytecode), *worker_runtime);
            worker_vm->set_jit_enabled(false);
            worker_vm->prepare_runtime_invocation_state();
            return [worker_vm, worker_runtime](const Value& callable, const std::vector<Value>& args) {
                std::lock_guard<std::mutex> lock(worker_runtime->parallel_worker_invoke_mutex());
                return worker_vm->invoke_callable_isolated(callable, args);
            };
        });
        runtime_->set_testing_runner_hooks(
            TestingRunnerHooks{[this](std::optional<std::string> filter) { return run_discovered_tests(filter); }});
    }
}

void Vm::finish_run_session() {
    if (debug_hook_ != nullptr && !debug_execution_paused_) {
        debug_hook_->on_program_exit(*this);
    }
    if (session_profiler_ != nullptr) {
        session_profiler_->end_session();
        session_profiler_ = nullptr;
    }
    if (runtime_ != nullptr) {
        runtime_->clear_parallel_thread_invoker_factory();
        runtime_->clear_callable_invoker();
    }
}

void Vm::run() {
    std::optional<ProfilerThreadScope> profiler_scope;
    session_profiler_ = profiler_if_enabled();
    if (session_profiler_ != nullptr) {
        session_profiler_->begin_session();
        profiler_scope.emplace(session_profiler_);
    }
    setup_run_session();
    try {
        if (bytecode_.async_main_function_index != 0xFFFFFFFFu) {
            run_top_level_init();
            run_async_scheduler();
            finish_run_session();
            return;
        }

        run_sync_loop();
        if (!uses_async_scheduler()) {
            drain_background_tasks_after_sync_program();
        }
    } catch (const ProcessExit&) {
        halted_ = true;
    } catch (const std::bad_alloc&) {
        fail("out of memory");
    }
    finish_run_session();
}

int Vm::run_project_tests(const std::optional<std::string>& filter) {
    std::optional<ProfilerThreadScope> profiler_scope;
    session_profiler_ = profiler_if_enabled();
    if (session_profiler_ != nullptr) {
        session_profiler_->begin_session();
        profiler_scope.emplace(session_profiler_);
    }
    setup_run_session();
    try {
        if (bytecode_.async_main_function_index != 0xFFFFFFFFu) {
            run_top_level_init();
        } else {
            run_sync_loop();
            if (!uses_async_scheduler()) {
                drain_background_tasks_after_sync_program();
            }
        }
    } catch (const ProcessExit&) {
        halted_ = true;
    } catch (const std::bad_alloc&) {
        fail("out of memory");
    }
    int failed = 0;
    if (!process_exit_requested()) {
        failed = run_discovered_tests(filter);
    }
    finish_run_session();
    return failed;
}

bool Vm::process_exit_requested() const {
    return runtime_ != nullptr && runtime_->process_exit_requested();
}

int Vm::process_exit_code() const {
    return runtime_ != nullptr ? runtime_->process_exit_code() : 0;
}

bool Vm::uses_async_scheduler() const {
    return bytecode_.async_main_function_index != 0xFFFFFFFFu;
}

void Vm::run_sync_loop() {
    while (!halted_) {
        try {
            if (!step_one_instruction()) {
                return;
            }
        } catch (...) {
            absorb_foreign_exception([this](const RuntimeError& err) { raise_runtime_error(err); });
        }
    }
}

bool Vm::step_one_instruction() {
    if (debug_hook_ != nullptr && debug_hook_->before_instruction(*this, ip_)) {
        debug_execution_paused_ = true;
        return false;
    }
    execute(fetch());
    if (debug_hook_ != nullptr) {
        debug_hook_->after_instruction(*this, ip_ > 0 ? ip_ - 1 : 0);
    }
    return true;
}

void Vm::run_top_level_init() {
    while (!halted_) {
        try {
            if (!step_one_instruction()) {
                return;
            }
        } catch (...) {
            absorb_foreign_exception([this](const RuntimeError& err) { raise_runtime_error(err); });
        }
    }
    halted_ = false;
    register_base_ = 0;
    registers_.clear();
    locals_.clear();
    upvalues_.clear();
    frames_.clear();
    handler_stack_.clear();
    pending_exception_ = std::nullopt;
    pending_control_.kind = PendingControlKind::None;
}

const Value& Vm::read_register(std::uint32_t index) const {
    const std::size_t physical = register_base_ + index;
    if (physical >= registers_.size()) {
        fail("invalid register");
    }
    if (!registers_[physical]) {
        fail("uninitialized register");
    }
    return *registers_[physical];
}

Value& Vm::write_register(std::uint32_t index) {
    const std::size_t physical = register_base_ + index;
    if (physical >= registers_.size()) {
        fail("invalid register");
    }
    std::optional<Value>& slot = registers_[physical];
    if (!slot) {
        slot = Value{};
    }
    return *slot;
}

void Vm::assign_register_integer(Value& dest, std::int64_t value) {
    if (dest.kind == ValueKind::Integer) {
        dest.integer = value;
        return;
    }
    dest = Value{};
    dest.kind = ValueKind::Integer;
    dest.integer = value;
}

bool Vm::try_fast_integer_arith(Opcode opcode, const Value& left, const Value& right, Value& dest) {
    if (left.kind != ValueKind::Integer || right.kind != ValueKind::Integer) {
        return false;
    }
    const std::int64_t a = left.integer;
    const std::int64_t b = right.integer;
    std::int64_t result = 0;
    switch (opcode) {
        case Opcode::Add:
            result = a + b;
            break;
        case Opcode::Sub:
            result = a - b;
            break;
        case Opcode::Mul:
            result = a * b;
            break;
        case Opcode::Div:
            if (b == 0) {
                user_runtime_error("division by zero");
            }
            result = a / b;
            break;
        case Opcode::Mod:
            if (b == 0) {
                user_runtime_error("modulo by zero");
            }
            result = a % b;
            break;
        default:
            return false;
    }
    assign_register_integer(dest, result);
    return true;
}

std::int64_t Vm::cached_integer_constant(std::uint32_t index) {
    const Bytecode& active = code();
    if (integer_constant_cache_owner_ != &active) {
        integer_constant_cache_owner_ = &active;
        integer_constant_cache_.clear();
        integer_constant_ready_.clear();
    }
    if (index >= active.constants.size()) {
        fail("invalid constant index");
    }
    const Constant& constant = active.constants[index];
    if (constant.kind != ConstantKind::Integer) {
        fail("invalid integer constant");
    }
    if (integer_constant_cache_.size() != active.constants.size()) {
        integer_constant_cache_.assign(active.constants.size(), 0);
        integer_constant_ready_.assign(active.constants.size(), 0);
    }
    if (!integer_constant_ready_[index]) {
        integer_constant_cache_[index] = parse_integer(constant.lexeme);
        integer_constant_ready_[index] = 1;
    }
    return integer_constant_cache_[index];
}

Value Vm::load_constant(std::uint32_t index) {
    if (index >= code().constants.size()) {
        fail("invalid constant index");
    }
    return constant_to_value(code().constants[index]);
}

const Value& Vm::read_variable(std::uint32_t index) const {
    if (index >= variables_.size()) {
        fail("invalid variable index");
    }
    if (!variables_[index]) {
        fail("uninitialized variable");
    }
    return *variables_[index];
}

void Vm::write_variable(std::uint32_t index, Value value) {
    if (index >= variables_.size()) {
        fail("invalid variable index");
    }
    variables_[index] = std::move(value);
}

const Value& Vm::read_local(std::uint32_t index) const {
    if (index >= locals_.size() || !locals_[index]) {
        fail("invalid local index");
    }
    if (!locals_[index]->value) {
        fail("uninitialized variable");
    }
    runtime_debug::assert_valid_value(*locals_[index]->value);
    return *locals_[index]->value;
}

void Vm::write_local(std::uint32_t index, Value value) {
    if (index >= locals_.size() || !locals_[index]) {
        fail("invalid local index");
    }
    locals_[index]->value = std::move(value);
}

const Value& Vm::read_upvalue(std::uint32_t index) const {
    if (index >= upvalues_.size() || !upvalues_[index]) {
        fail("invalid closure access");
    }
    if (!upvalues_[index]->value) {
        fail("uninitialized variable");
    }
    return *upvalues_[index]->value;
}

void Vm::write_upvalue(std::uint32_t index, Value value) {
    if (index >= upvalues_.size() || !upvalues_[index]) {
        fail("invalid closure access");
    }
    upvalues_[index]->value = std::move(value);
}

void Vm::ensure_call_stack_capacity() {
    if (frames_.size() >= kMaxCallStackDepth) {
        user_runtime_error("maximum call stack depth exceeded");
    }
}

void Vm::enter_function(const FunctionInfo& fn, std::uint32_t function_index, std::vector<Value> args,
                        std::vector<std::shared_ptr<Cell>> captures, std::uint32_t dest, std::size_t return_ip,
                        std::optional<Value> implicit_this) {
    enter_function_prologue(fn, function_index, return_ip, dest);
    const std::uint32_t arg_offset = implicit_this ? 1u : 0u;
    if (implicit_this) {
        locals_[0]->value = std::move(*implicit_this);
    }
    for (std::uint32_t i = 0; i < fn.arity && i < args.size(); ++i) {
        locals_[arg_offset + i]->value = std::move(args[i]);
    }
    upvalues_ = std::move(captures);
    enter_function_epilogue(fn, function_index);
}

void Vm::execute_call_builtin(const Instruction& instruction) {
    if (instruction.c >= code().calls.size()) {
        fail("invalid call index");
    }
    const CallInfo& info = code().calls[instruction.c];
    const BuiltinId builtin_id = builtin_id_from_name(info.name);

    constexpr std::uint32_t kInlineArgCap = 8;
    Value inline_args[kInlineArgCap];
    const Value* arg_ptr = nullptr;
    std::vector<Value> heap_args;
    const std::uint32_t argc = info.argc;
    if (argc <= kInlineArgCap) {
        for (std::uint32_t i = 0; i < argc; ++i) {
            inline_args[i] = read_register(instruction.b + i);
        }
        arg_ptr = inline_args;
    } else {
        heap_args.reserve(argc);
        for (std::uint32_t i = 0; i < argc; ++i) {
            heap_args.push_back(read_register(instruction.b + i));
        }
        arg_ptr = heap_args.data();
    }

    try {
        switch (builtin_id) {
            case BuiltinId::Delay:
                if (argc != 1) {
                    user_runtime_error("'delay' expects 1 argument");
                }
                if (arg_ptr[0].kind != ValueKind::Integer) {
                    user_runtime_error("'delay' expects a duration literal");
                }
                write_register(instruction.a) = Runtime::make_task(create_timer_task(arg_ptr[0].integer));
                return;
            case BuiltinId::Race:
                if (argc == 0) {
                    user_runtime_error("'race' expects at least 1 argument");
                }
                write_register(instruction.a) =
                    Runtime::make_task(create_race_task(heap_args.empty()
                                                          ? std::vector<Value>(arg_ptr, arg_ptr + argc)
                                                          : std::move(heap_args)));
                return;
            case BuiltinId::Timeout:
                if (argc != 2) {
                    user_runtime_error("'timeout' expects 2 arguments");
                }
                if (arg_ptr[0].kind != ValueKind::Task || !arg_ptr[0].task) {
                    user_runtime_error("'timeout' expects a Task as the first argument");
                }
                if (arg_ptr[1].kind != ValueKind::Integer) {
                    user_runtime_error("'timeout' expects a duration literal as the second argument");
                }
                write_register(instruction.a) =
                    Runtime::make_task(create_timeout_task(arg_ptr[0].task, arg_ptr[1].integer));
                return;
            case BuiltinId::WaitAll:
                if (argc == 0) {
                    user_runtime_error("'wait_all' expects at least 1 argument");
                }
                write_register(instruction.a) =
                    Runtime::make_task(create_wait_all_task(heap_args.empty()
                                                              ? std::vector<Value>(arg_ptr, arg_ptr + argc)
                                                              : std::move(heap_args)));
                return;
            case BuiltinId::WaitAny:
                if (argc == 0) {
                    user_runtime_error("'wait_any' expects at least 1 argument");
                }
                write_register(instruction.a) =
                    Runtime::make_task(create_race_task(heap_args.empty()
                                                            ? std::vector<Value>(arg_ptr, arg_ptr + argc)
                                                            : std::move(heap_args)));
                return;
            case BuiltinId::AsyncInput:
                if (argc > 1) {
                    user_runtime_error("'async_input' expects 0 or 1 argument");
                }
                {
                    std::optional<std::string> prompt;
                    if (argc == 1) {
                        if (arg_ptr[0].kind != ValueKind::String) {
                            user_runtime_error("'async_input' expects a string prompt");
                        }
                        prompt = arg_ptr[0].string;
                    }
                    write_register(instruction.a) = runtime_->async_io().submit_stdin_line(std::move(prompt));
                }
                return;
            case BuiltinId::TypeOf:
                if (argc != 1) {
                    user_runtime_error("'type_of' expects 1 argument");
                }
                write_register(instruction.a) = Runtime::make_string(reflection_type_name(code(), arg_ptr[0]));
                return;
            case BuiltinId::Annotations:
                if (argc != 1) {
                    user_runtime_error("'annotations' expects 1 argument");
                }
                write_register(instruction.a) = reflection_annotations(code(), arg_ptr[0]);
                return;
            case BuiltinId::Members:
                if (argc != 1) {
                    user_runtime_error("'members' expects 1 argument");
                }
                write_register(instruction.a) = reflection_members(code(), arg_ptr[0]);
                return;
            default:
                break;
        }
        Value dispatched;
        if (builtin_id != BuiltinId::Unknown &&
            dispatch_runtime_builtin(*runtime_, builtin_id, arg_ptr, argc, dispatched)) {
            write_register(instruction.a) = std::move(dispatched);
            return;
        }
        if (heap_args.empty()) {
            heap_args.assign(arg_ptr, arg_ptr + argc);
        }
        write_register(instruction.a) = runtime_->call(info.name, heap_args);
    } catch (const RuntimeError& err) {
        raise_runtime_error(err);
    }
}

void Vm::call_function(const Instruction& instruction) {
    const std::uint32_t function_index = instruction.c;
    if (function_index >= code().functions.size()) {
        fail("invalid function index");
    }
    const FunctionInfo& fn = code().functions[function_index];
    if (fn.is_async) {
        std::vector<Value> args;
        args.reserve(fn.arity);
        for (std::uint32_t i = 0; i < fn.arity; ++i) {
            args.push_back(read_register(instruction.b + i));
        }
        spawn_async_function(fn, function_index, std::move(args), {}, instruction.a, 0xFFFFFFFFu);
        return;
    }
    if (function_allows_leaf_local_cache(fn) && fn.arity > 0 && fn.arity <= 16) {
        const std::size_t caller_base = register_base_;
        if (fn.arity == 1) {
            const std::size_t physical = caller_base + instruction.b;
            if (physical >= registers_.size() || !registers_[physical]) {
                fail("uninitialized register");
            }
            record_call_site_profile();
            enter_function_prologue(fn, function_index, ip_, instruction.a);
            bind_local_argument(locals_[0], *registers_[physical]);
            upvalues_.clear();
            enter_function_epilogue(fn, function_index);
            return;
        }
        Value arg_scratch[16];
        for (std::uint32_t i = 0; i < fn.arity; ++i) {
            const std::size_t physical = caller_base + instruction.b + i;
            if (physical >= registers_.size() || !registers_[physical]) {
                fail("uninitialized register");
            }
            arg_scratch[i] = *registers_[physical];
        }
        record_call_site_profile();
        enter_function_prologue(fn, function_index, ip_, instruction.a);
        for (std::uint32_t i = 0; i < fn.arity; ++i) {
            bind_local_argument(locals_[i], arg_scratch[i]);
        }
        upvalues_.clear();
        enter_function_epilogue(fn, function_index);
        return;
    }
    record_call_site_profile();
    enter_function_prologue(fn, function_index, ip_, instruction.a);
    const std::size_t caller_base = frames_.back().register_base;
    for (std::uint32_t i = 0; i < fn.arity; ++i) {
        const std::uint32_t reg = instruction.b + i;
        const std::size_t physical = caller_base + reg;
        if (physical >= registers_.size() || !registers_[physical]) {
            fail("uninitialized register");
        }
        bind_local_argument(locals_[i], *registers_[physical]);
    }
    upvalues_.clear();
    enter_function_epilogue(fn, function_index);
}

void Vm::call_value(const Instruction& instruction) {
    const std::uint32_t site_ip = static_cast<std::uint32_t>(ip_ - 1);
    const Value& callee = read_register(instruction.b);
    if (callee.kind != ValueKind::Function || !callee.function) {
        user_runtime_error("invalid function value call");
    }
    if (callee.function->module_index == 0xFFFFFFFFu && site_ip < callvalue_mono_cache_.size()) {
        const CallValueMonoCacheEntry& slot = callvalue_mono_cache_[site_ip];
        if (slot.closure_token != nullptr && callee.function.get() == slot.closure_token &&
            slot.function_index < code().functions.size()) {
            const FunctionInfo& cached_fn = code().functions[slot.function_index];
            if (!cached_fn.is_async && instruction.c == cached_fn.arity && cached_fn.arity <= 16 &&
                cached_fn.captures.empty() && callee.function->captures.empty()) {
                Value arg_scratch[16];
                const std::size_t caller_base = register_base_;
                for (std::uint32_t i = 0; i < cached_fn.arity; ++i) {
                    const std::size_t physical = caller_base + instruction.b + 1 + i;
                    if (physical >= registers_.size() || !registers_[physical]) {
                        fail("uninitialized register");
                    }
                    arg_scratch[i] = *registers_[physical];
                }
                record_call_site_profile();
                enter_function_prologue(cached_fn, slot.function_index, ip_, instruction.a);
                for (std::uint32_t i = 0; i < cached_fn.arity; ++i) {
                    bind_local_argument(locals_[i], arg_scratch[i]);
                }
                upvalues_.clear();
                enter_function_epilogue(cached_fn, slot.function_index);
                return;
            }
        }
    }
    if (callee.function->module_index != 0xFFFFFFFFu) {
        std::vector<Value> args;
        args.reserve(instruction.c);
        for (std::uint32_t i = 0; i < instruction.c; ++i) {
            args.push_back(read_register(instruction.b + 1 + i));
        }
        if (callee.function->module_class_ctor) {
            invoke_module_class_ctor(callee.function->module_index, callee.function->class_index, args, instruction.a);
            return;
        }
        if (callee.function->module_struct_ctor) {
            if (!args.empty()) {
                user_runtime_error("'" + callee.function->name + "' expects 0 arguments");
            }
            invoke_module_struct_ctor(callee.function->module_index, callee.function->struct_index, instruction.a);
            return;
        }
        const Bytecode& module_bc = module_code(callee.function->module_index);
        if (callee.function->function_index >= module_bc.functions.size()) {
            fail("invalid function index");
        }
        const FunctionInfo& fn = module_bc.functions[callee.function->function_index];
        if (!fn.is_native && instruction.c != fn.arity) {
            user_runtime_error("'" + fn.name + "' expects " + std::to_string(fn.arity) + " arguments");
        }
        if (fn.is_native) {
            try {
                bind_native_call_site();
                write_register(instruction.a) = call_native_function(
                    static_cast<NativeFunctionId>(fn.native_function_id), *runtime_, args);
                clear_native_call_site_binding();
            } catch (const RuntimeError& err) {
                clear_native_call_site_binding();
                raise_runtime_error(err);
            }
            return;
        }
        if (fn.is_async) {
            spawn_async_function(fn, callee.function->function_index, std::move(args), callee.function->captures,
                                 instruction.a, callee.function->module_index);
            return;
        }
        enter_module_bound_function(fn, callee.function->module_index, callee.function->function_index, std::move(args),
                                    callee.function->captures, instruction.a, ip_);
        return;
    }
    if (callee.function->function_index >= code().functions.size()) {
        fail("invalid function index");
    }
    const FunctionInfo& fn = code().functions[callee.function->function_index];
    if (instruction.c != fn.arity) {
        user_runtime_error("'" + fn.name + "' expects " + std::to_string(fn.arity) + " arguments");
    }
    if (fn.is_async) {
        std::vector<Value> args;
        args.reserve(fn.arity);
        for (std::uint32_t i = 0; i < fn.arity; ++i) {
            args.push_back(read_register(instruction.b + 1 + i));
        }
        spawn_async_function(fn, callee.function->function_index, std::move(args), callee.function->captures,
                             instruction.a, 0xFFFFFFFFu);
        return;
    }
    if (callee.function->captures.empty() && fn.captures.empty() && fn.arity <= 16) {
        const std::uint32_t function_index = callee.function->function_index;
        CallValueMonoCacheEntry& mono_slot = callvalue_mono_cache_[site_ip];
        mono_slot.closure_token = callee.function.get();
        mono_slot.function_index = function_index;
        Value arg_scratch[16];
        for (std::uint32_t i = 0; i < fn.arity; ++i) {
            arg_scratch[i] = read_register(instruction.b + 1 + i);
        }
        record_call_site_profile();
        enter_function_prologue(fn, function_index, ip_, instruction.a);
        for (std::uint32_t i = 0; i < fn.arity; ++i) {
            bind_local_argument(locals_[i], arg_scratch[i]);
        }
        upvalues_.clear();
        enter_function_epilogue(fn, function_index);
        return;
    }
    std::vector<Value> args;
    args.reserve(fn.arity);
    for (std::uint32_t i = 0; i < fn.arity; ++i) {
        args.push_back(read_register(instruction.b + 1 + i));
    }
    enter_function(fn, callee.function->function_index, std::move(args), callee.function->captures, instruction.a, ip_);
}

void Vm::invoke_sync_function_value(const Value& callee, std::vector<Value> args) {
    if (callee.kind != ValueKind::Function || !callee.function) {
        user_runtime_error("defer requires a callable");
    }
    if (callee.function->module_index != 0xFFFFFFFFu) {
        user_runtime_error("defer requires a callable");
    }
    if (callee.function->function_index >= code().functions.size()) {
        fail("invalid function index");
    }
    const FunctionInfo& fn = code().functions[callee.function->function_index];
    if (fn.is_async) {
        user_runtime_error("defer cannot call async function");
    }
    if (args.size() != fn.arity) {
        user_runtime_error("'" + fn.name + "' expects " + std::to_string(fn.arity) + " arguments");
    }
    const std::size_t depth_before = frames_.size();
    enter_function(fn, callee.function->function_index, std::move(args), callee.function->captures, 0, ip_);
    while (frames_.size() > depth_before) {
        if (halted_ || debug_execution_paused_) {
            return;
        }
        if (ip_ >= code().instructions.size()) {
            fail("invalid bytecode");
        }
        if (debug_hook_ != nullptr) {
            if (!step_one_instruction()) {
                return;
            }
            continue;
        }
        execute(fetch());
    }
}

void Vm::run_function_defers(std::vector<Value>& defers) {
    while (!defers.empty()) {
        Value thunk = std::move(defers.back());
        defers.pop_back();
        invoke_sync_function_value(thunk, {});
    }
}

void Vm::invoke_class(const Instruction& instruction) {
    if (instruction.c >= code().class_calls.size()) {
        fail("invalid class call index");
    }
    const ClassCallInfo& call = code().class_calls[instruction.c];
    if (call.class_index >= code().classes.size()) {
        fail("invalid class index");
    }
    const ClassInfo& info = code().classes[call.class_index];
    if (info.native_class_kind != 0) {
        const NativeClassKind kind = static_cast<NativeClassKind>(info.native_class_kind);
        if (kind == NativeClassKind::TaskGroup) {
            if (call.argc != 0) {
                fail("'" + info.name + "' expects 0 arguments");
            }
            write_register(instruction.a) = make_native_task_group(call.class_index);
            return;
        }
        if (kind == NativeClassKind::AsyncStream) {
            std::vector<std::string> chunks;
            if (call.argc == 1) {
                const Value& arg = read_register(instruction.b);
                if (arg.kind == ValueKind::List && arg.list) {
                    for (const Value& item : arg.list->items) {
                        if (item.kind != ValueKind::String) {
                            user_runtime_error("AsyncStream expects a list of strings");
                        }
                        chunks.push_back(item.string);
                    }
                } else {
                    user_runtime_error("AsyncStream expects a list argument");
                }
            } else if (call.argc != 0) {
                fail("'" + info.name + "' expects 0 or 1 arguments");
            }
            write_register(instruction.a) = make_native_async_stream(call.class_index, std::move(chunks));
            return;
        }
    }
    if (info.module_index != 0xFFFFFFFFu) {
        std::vector<Value> args;
        args.reserve(call.argc);
        for (std::uint32_t i = 0; i < call.argc; ++i) {
            args.push_back(read_register(instruction.b + i));
        }
        const std::uint32_t module_class_index =
            info.external_index != 0xFFFFFFFFu ? info.external_index : call.class_index;
        invoke_module_class_ctor(info.module_index, module_class_index, args, instruction.a);
        return;
    }
    std::vector<std::string> field_names;
    field_names.reserve(info.layout.size());
    for (const ClassFieldLayout& field : info.layout) {
        field_names.push_back(field.name);
    }
    Value instance = Runtime::make_class(call.class_index, info.name, std::move(field_names));

    if (call.function_index != 0xFFFFFFFFu) {
        if (call.function_index >= code().functions.size()) {
            fail("invalid function index");
        }
        const FunctionInfo& ctor = code().functions[call.function_index];
        if (call.argc != ctor.arity) {
            fail("'" + info.name + "' expects " + std::to_string(ctor.arity) + " arguments");
        }
        std::vector<Value> args;
        args.reserve(call.argc);
        for (std::uint32_t i = 0; i < call.argc; ++i) {
            args.push_back(read_register(instruction.b + i));
        }
        enter_function(ctor, call.function_index, std::move(args), {}, instruction.a, ip_, std::move(instance));
        return;
    }
    if (call.argc != 0) {
        fail("'" + info.name + "' expects 0 arguments");
    }
    if (info.parent_index != 0xFFFFFFFFu) {
        const ClassInfo& parent = code().classes[info.parent_index];
        if (parent.module_index != 0xFFFFFFFFu) {
            if (parent.constructor_index != 0xFFFFFFFFu) {
                const Bytecode& parent_bc = module_code(parent.module_index);
                if (parent.constructor_index < parent_bc.functions.size()) {
                    const FunctionInfo& parent_ctor = parent_bc.functions[parent.constructor_index];
                    if (parent_ctor.arity == 0) {
                        enter_module_bound_function(parent_ctor, parent.module_index, parent.constructor_index, {},
                                                    {}, instruction.a, ip_, std::move(instance));
                        return;
                    }
                }
            }
        } else if (parent.constructor_index != 0xFFFFFFFFu) {
            const FunctionInfo& parent_ctor = code().functions[parent.constructor_index];
            if (parent_ctor.arity == 0) {
                enter_function(parent_ctor, parent.constructor_index, {}, {}, instruction.a, ip_, std::move(instance));
                return;
            }
        }
    }
    write_register(instruction.a) = std::move(instance);
}

void Vm::call_class_method(const Instruction& instruction) {
    if (instruction.c >= code().class_calls.size()) {
        fail("invalid class call index");
    }
    const ClassCallInfo& call = code().class_calls[instruction.c];
    if (call.class_index >= code().classes.size()) {
        fail("invalid class index");
    }
    const ClassInfo& class_info = code().classes[call.class_index];
    if (call.function_index >= code().functions.size() && class_info.module_index == 0xFFFFFFFFu) {
        fail("invalid function index");
    }
    const Value& receiver = read_register(instruction.b);
    if (receiver.kind != ValueKind::Class || !receiver.class_value) {
        fail("invalid class method call");
    }
    std::vector<Value> args;
    args.reserve(call.argc);
    for (std::uint32_t i = 0; i < call.argc; ++i) {
        args.push_back(read_register(instruction.b + 1 + i));
    }
    if (class_info.module_index != 0xFFFFFFFFu) {
        const Bytecode& module_bc = module_code(class_info.module_index);
        if (call.function_index >= module_bc.functions.size()) {
            fail("invalid function index");
        }
        const FunctionInfo& fn = module_bc.functions[call.function_index];
        if (call.argc != fn.arity && !is_native_class_object(receiver)) {
            fail("'" + fn.name + "' expects " + std::to_string(fn.arity) + " arguments");
        }
        if (is_native_class_object(receiver)) {
            try {
                write_register(instruction.a) = call_native_class_method(receiver, fn.name, args, *runtime_);
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        }
        if (fn.is_async) {
            spawn_async_function(fn, call.function_index, std::move(args), {}, instruction.a, class_info.module_index,
                                 receiver);
            return;
        }
        enter_module_bound_function(fn, class_info.module_index, call.function_index, std::move(args), {}, instruction.a,
                                    ip_, receiver);
        return;
    }
    const FunctionInfo& fn = code().functions[call.function_index];
    if (call.argc != fn.arity) {
        fail("'" + fn.name + "' expects " + std::to_string(fn.arity) + " arguments");
    }
    if (is_native_class_object(receiver)) {
        try {
            write_register(instruction.a) = call_native_class_method(receiver, fn.name, args, *runtime_);
        } catch (const RuntimeError& err) {
            raise_runtime_error(err);
        }
        return;
    }
    if (fn.is_async) {
        spawn_async_function(fn, call.function_index, std::move(args), {}, instruction.a, 0xFFFFFFFFu, receiver);
        return;
    }
    enter_function(fn, call.function_index, std::move(args), {}, instruction.a, ip_, receiver);
}

void Vm::invoke_super(const Instruction& instruction) {
    if (instruction.b >= code().class_calls.size()) {
        fail("invalid class call index");
    }
    const ClassCallInfo& call = code().class_calls[instruction.b];
    if (call.function_index >= code().functions.size()) {
        fail("invalid function index");
    }
    const FunctionInfo& fn = code().functions[call.function_index];
    if (call.argc != fn.arity) {
        fail("parent constructor expects " + std::to_string(fn.arity) + " arguments");
    }
    if (locals_.empty() || !locals_[0] || !locals_[0]->value) {
        fail("invalid super constructor call");
    }
    const Value& receiver = *locals_[0]->value;
    std::vector<Value> args;
    args.reserve(call.argc);
    for (std::uint32_t i = 0; i < call.argc; ++i) {
        args.push_back(read_register(instruction.a + i));
    }
    enter_function(fn, call.function_index, std::move(args), {}, 0, ip_, receiver);
}

Vm::ResolvedInstanceMethod Vm::lookup_instance_method(const Value& receiver, std::string_view method) const {
    ResolvedInstanceMethod result;
    if (receiver.kind != ValueKind::Class || !receiver.class_value) {
        return result;
    }
    const std::uint32_t class_index = receiver.class_value->type_index;
    if (class_index >= bytecode_.classes.size()) {
        return result;
    }
    for (std::uint32_t current = class_index; current != 0xFFFFFFFFu;
         current = bytecode_.classes[current].parent_index) {
        const ClassInfo& info = bytecode_.classes[current];
        for (const ClassMethodInfo& method_info : info.methods) {
            if (method_info.name == method) {
                result.found = true;
                result.module_index = info.module_index;
                result.function_index = method_info.function_index;
                if (info.module_index != 0xFFFFFFFFu) {
                    const Bytecode& module_bc = module_code(info.module_index);
                    if (method_info.function_index >= module_bc.functions.size()) {
                        return ResolvedInstanceMethod{};
                    }
                    result.fn = &module_bc.functions[method_info.function_index];
                } else {
                    if (method_info.function_index >= bytecode_.functions.size()) {
                        return ResolvedInstanceMethod{};
                    }
                    result.fn = &bytecode_.functions[method_info.function_index];
                }
                return result;
            }
        }
    }
    return result;
}

Vm::MethodInvokeStatus Vm::invoke_instance_method_sync(const Value& receiver, std::string_view method,
                                                         std::vector<Value> args, Value* out,
                                                         bool catch_stop_iteration) {
    if (is_native_iterator(receiver)) {
        try {
            if (method == "next") {
                *out = native_iterator_next(receiver);
                return MethodInvokeStatus::Returned;
            }
            if (method == "iter") {
                *out = native_iterator_iter(receiver);
                return MethodInvokeStatus::Returned;
            }
            user_runtime_error(std::string("Iterator has no method '") + std::string(method) + "'");
        } catch (const RuntimeError& err) {
            if (catch_stop_iteration && is_stop_iteration_runtime_error(err)) {
                return MethodInvokeStatus::StopIteration;
            }
            raise_runtime_error(err);
            return MethodInvokeStatus::Exception;
        }
    }

    const ResolvedInstanceMethod resolved = lookup_instance_method(receiver, method);
    if (!resolved.found || resolved.fn == nullptr) {
        if (method == "iter") {
            const std::string type_name =
                receiver.class_value && !receiver.class_value->type_name.empty() ? receiver.class_value->type_name
                                                                                 : "object";
            user_runtime_error("object is not iterable: " + type_name, RuntimeErrorCategory::Type);
        }
        user_runtime_error(std::string("object has no method '") + std::string(method) + "'",
                           RuntimeErrorCategory::Type);
    }
    if (resolved.fn->is_async) {
        user_runtime_error("'" + resolved.fn->name + "' cannot be used in iterator protocol");
    }

    if (is_native_class_object(receiver)) {
        try {
            *out = call_native_class_method(receiver, method, args, *runtime_);
            return MethodInvokeStatus::Returned;
        } catch (const RuntimeError& err) {
            if (catch_stop_iteration && is_stop_iteration_runtime_error(err)) {
                return MethodInvokeStatus::StopIteration;
            }
            raise_runtime_error(err);
            return MethodInvokeStatus::Exception;
        }
    }

    const std::size_t saved_frames = frames_.size();
    const std::size_t saved_handlers = handler_stack_.size();
    protocol_invoke_boundary_ = saved_frames;
    protocol_catch_stop_iteration_ = catch_stop_iteration;
    protocol_stop_iteration_ = false;
    protocol_invoke_resume_.saved_handlers = saved_handlers;
    const std::uint32_t scratch = static_cast<std::uint32_t>(registers_.size());
    registers_.emplace_back(std::nullopt);

    if (resolved.module_index != 0xFFFFFFFFu) {
        enter_module_bound_function(*resolved.fn, resolved.module_index, resolved.function_index, std::move(args), {},
                                    scratch, ip_, receiver);
    } else {
        enter_function(*resolved.fn, resolved.function_index, std::move(args), {}, scratch, ip_, receiver);
    }

    return MethodInvokeStatus::Awaiting;
}

void Vm::complete_protocol_invoke_success(Value result) {
    if (!protocol_invoke_resume_.active) {
        return;
    }
    write_register(protocol_invoke_resume_.dest_reg) = std::move(result);
    handler_stack_.resize(protocol_invoke_resume_.saved_handlers);
    protocol_invoke_boundary_ = std::nullopt;
    protocol_catch_stop_iteration_ = false;
    protocol_stop_iteration_ = false;
    protocol_invoke_resume_.active = false;
    pending_control_.kind = PendingControlKind::None;
}

void Vm::complete_protocol_invoke_stop_iteration() {
    if (!protocol_invoke_resume_.active) {
        return;
    }
    if (protocol_invoke_resume_.is_iter_next) {
        jump_to(protocol_invoke_resume_.exit_ip);
    }
    handler_stack_.resize(protocol_invoke_resume_.saved_handlers);
    protocol_invoke_boundary_ = std::nullopt;
    protocol_catch_stop_iteration_ = false;
    protocol_stop_iteration_ = false;
    pending_exception_ = std::nullopt;
    protocol_invoke_resume_.active = false;
}

void Vm::complete_protocol_invoke_exception() {
    if (!protocol_invoke_resume_.active) {
        return;
    }
    handler_stack_.resize(protocol_invoke_resume_.saved_handlers);
    protocol_invoke_boundary_ = std::nullopt;
    protocol_catch_stop_iteration_ = false;
    protocol_stop_iteration_ = false;
    protocol_invoke_resume_.active = false;
    if (pending_exception_) {
        Value exception = std::move(*pending_exception_);
        pending_exception_ = std::nullopt;
        throw_value(std::move(exception));
    }
}

void Vm::execute_get_iter(const Instruction& instruction) {
    const Value& value = read_register(instruction.b);
    try {
        if (value.kind == ValueKind::Class && !is_native_iterator(value)) {
            if (is_native_sql_rows(value)) {
                write_register(instruction.a) = call_native_class_method(value, "iter", {}, *runtime_);
                return;
            }
            if (is_native_archive(value)) {
                write_register(instruction.a) = runtime_get_iter(value);
                return;
            }
            const ResolvedInstanceMethod iter_method = lookup_instance_method(value, "iter");
            if (iter_method.found && iter_method.fn != nullptr) {
                if (iter_method.fn->is_async) {
                    user_runtime_error("'" + iter_method.fn->name + "' cannot be used in iterator protocol");
                }
                if (iter_method.module_index != 0xFFFFFFFFu) {
                    enter_module_bound_function(*iter_method.fn, iter_method.module_index, iter_method.function_index,
                                                {}, {}, instruction.a, ip_, value);
                } else {
                    enter_function(*iter_method.fn, iter_method.function_index, {}, {}, instruction.a, ip_, value);
                }
                return;
            }
            const ResolvedInstanceMethod next_method = lookup_instance_method(value, "next");
            if (next_method.found && next_method.fn != nullptr) {
                write_register(instruction.a) = value;
                return;
            }
            const std::string type_name =
                value.class_value && !value.class_value->type_name.empty() ? value.class_value->type_name : "object";
            user_runtime_error("object is not iterable: " + type_name, RuntimeErrorCategory::Type);
        }
        write_register(instruction.a) = runtime_get_iter(value);
    } catch (const RuntimeError& err) {
        raise_runtime_error(err);
    }
}

void Vm::execute_iter_next(const Instruction& instruction) {
    const Value& iterator = read_register(instruction.b);
    if (is_native_iterator(iterator)) {
        Value next;
        try {
            if (!native_iterator_try_next(iterator, next)) {
                jump_to(instruction.c);
                return;
            }
            write_register(instruction.a) = std::move(next);
        } catch (const RuntimeError& err) {
            raise_runtime_error(err);
        }
        return;
    }

    const ResolvedInstanceMethod resolved = lookup_instance_method(iterator, "next");
    if (!resolved.found || resolved.fn == nullptr) {
        user_runtime_error("object has no method 'next'", RuntimeErrorCategory::Type);
    }
    if (resolved.fn->is_async) {
        user_runtime_error("'" + resolved.fn->name + "' cannot be used in iterator protocol");
    }
    if (resolved.module_index != 0xFFFFFFFFu) {
        enter_module_bound_function(*resolved.fn, resolved.module_index, resolved.function_index, {}, {},
                                    instruction.a, ip_, iterator);
    } else {
        enter_function(*resolved.fn, resolved.function_index, {}, {}, instruction.a, ip_, iterator);
    }
}

void Vm::make_closure(const Instruction& instruction) {
    if (instruction.b >= code().functions.size()) {
        fail("invalid function index");
    }
    const FunctionInfo& fn = code().functions[instruction.b];
    auto data = std::make_shared<ClosureData>();
    data->function_index = instruction.b;
    data->name = fn.name;
    data->captures.reserve(fn.captures.size());
    for (const Capture& capture : fn.captures) {
        if (capture.from_local) {
            if (capture.index >= locals_.size() || !locals_[capture.index]) {
                fail("invalid closure access");
            }
            data->captures.push_back(locals_[capture.index]);
        } else {
            if (capture.index >= upvalues_.size() || !upvalues_[capture.index]) {
                fail("invalid closure access");
            }
            data->captures.push_back(upvalues_[capture.index]);
        }
    }
    write_register(instruction.a) = Runtime::make_function(std::move(data));
}

void Vm::store_catch_binding(const CatchEntry& entry, Value value) {
    if (entry.catch_slot == kInvalidHandlerIndex) {
        return;
    }
    if (entry.catch_is_local) {
        write_local(entry.catch_slot, std::move(value));
    } else {
        write_variable(entry.catch_slot, std::move(value));
    }
}

void Vm::restore_module_context(CallFrame& frame) {
    if (frame.module_index != 0xFFFFFFFFu) {
        module_states_[frame.module_index].globals = variables_;
        variables_ = std::move(frame.saved_variables);
        execution_bytecode_ = frame.saved_code != nullptr ? frame.saved_code : &bytecode_;
    }
}

void Vm::restore_call_frame_state(CallFrame& frame) {
    restore_module_context(frame);
    if (!frame.registers.empty()) {
        registers_ = std::move(frame.registers);
    }
    register_base_ = frame.register_base;
    release_leaving_locals(frame.function_index);
    locals_ = std::move(frame.locals);
    upvalues_ = std::move(frame.upvalues);
    ip_ = frame.return_ip;
}

void Vm::unwind_to_frame_depth(std::size_t depth) {
    while (frames_.size() > depth) {
        std::vector<Value> defers = std::move(frames_.back().defer_stack);
        run_function_defers(defers);
        CallFrame frame = std::move(frames_.back());
        frames_.pop_back();
        restore_call_frame_state(frame);
    }
}

const Bytecode& Vm::handler_bytecode(const HandlerFrame& frame) const {
    return frame.owner_code != nullptr ? *frame.owner_code : code();
}

bool Vm::catch_entry_matches(const CatchEntry& entry, const Value& value, const Bytecode& handler_bc) const {
    if (entry.catch_class_index == kInvalidHandlerIndex) {
        return true;
    }
    if (value.kind != ValueKind::Class || !value.class_value) {
        return false;
    }
    return is_class_or_subclass(value.class_value->type_index, entry.catch_class_index, handler_bc.classes);
}

void Vm::run_finally_for_handler(std::uint32_t handler_index) {
    if (handler_index >= code().handlers.size()) {
        fail("invalid handler index");
    }
    const HandlerInfo& handler = code().handlers[handler_index];
    if (handler.finally_ip == kInvalidHandlerIndex) {
        return;
    }
    jump_to(handler.finally_ip);
}

void Vm::enter_try(std::uint32_t handler_index) {
    if (handler_index >= code().handlers.size()) {
        fail("invalid handler index");
    }
    HandlerFrame frame;
    frame.handler_index = handler_index;
    frame.frame_depth = frames_.size();
    frame.next_catch = 0;
    frame.owner_code = execution_bytecode_ != nullptr ? execution_bytecode_ : &bytecode_;
    handler_stack_.push_back(frame);
}

void Vm::leave_try(std::uint32_t handler_index) {
    if (handler_stack_.empty() || handler_stack_.back().handler_index != handler_index) {
        fail("invalid bytecode");
    }
    const HandlerFrame& handler_frame = handler_stack_.back();
    const Bytecode& handler_bc = handler_bytecode(handler_frame);
    handler_stack_.pop_back();
    const HandlerInfo& handler = handler_bc.handlers[handler_index];
    if (handler.finally_ip != kInvalidHandlerIndex) {
        jump_to(handler.finally_ip);
    }
}

void Vm::complete_pending_control() {
    if (pending_control_.kind == PendingControlKind::None) {
        return;
    }
    if (pending_control_.kind == PendingControlKind::Return) {
        Value result = std::move(pending_control_.return_value);
        pending_control_.kind = PendingControlKind::None;
        if (frames_.empty()) {
            if (current_task_) {
                finish_current_async_task(std::move(result), 0xFFFFFFFFu);
                return;
            }
            fail("invalid bytecode");
        }
        CallFrame frame = std::move(frames_.back());
        if (frame.function_index < code().functions.size()) {
            const FunctionInfo& fn = code().functions[frame.function_index];
            if (fn.is_constructor && !locals_.empty() && locals_[0] && locals_[0]->value) {
                result = *locals_[0]->value;
            }
        }
        frames_.pop_back();
        if (frames_.empty() && current_task_) {
            finish_current_async_task(std::move(result), frame.module_index);
            return;
        }
        restore_call_frame_state(frame);
        write_register(frame.dest) = std::move(result);
        return;
    }
    const std::uint32_t jump_index = pending_control_.jump_index;
    pending_control_.kind = PendingControlKind::None;
    if (jump_index >= code().instructions.size()) {
        fail("invalid jump");
    }
    const Instruction& jump = code().instructions[jump_index];
    if (jump.opcode != Opcode::Jump) {
        fail("invalid bytecode");
    }
    jump_to(jump.a);
}

void Vm::defer_return(std::uint32_t handler_index, std::uint32_t src) {
    if (handler_stack_.empty() || handler_stack_.back().handler_index != handler_index) {
        fail("invalid bytecode");
    }
    pending_control_.kind = PendingControlKind::Return;
    pending_control_.return_value = read_register(src);
    const HandlerFrame& handler_frame = handler_stack_.back();
    const Bytecode& handler_bc = handler_bytecode(handler_frame);
    handler_stack_.pop_back();
    const HandlerInfo& handler = handler_bc.handlers[handler_index];
    if (handler.finally_ip != kInvalidHandlerIndex) {
        jump_to(handler.finally_ip);
    } else {
        complete_pending_control();
    }
}

void Vm::defer_break(std::uint32_t handler_index, std::uint32_t jump_index) {
    if (handler_stack_.empty() || handler_stack_.back().handler_index != handler_index) {
        fail("invalid bytecode");
    }
    pending_control_.kind = PendingControlKind::Break;
    pending_control_.jump_index = jump_index;
    const HandlerFrame& handler_frame = handler_stack_.back();
    const Bytecode& handler_bc = handler_bytecode(handler_frame);
    handler_stack_.pop_back();
    const HandlerInfo& handler = handler_bc.handlers[handler_index];
    if (handler.finally_ip != kInvalidHandlerIndex) {
        jump_to(handler.finally_ip);
    } else {
        complete_pending_control();
    }
}

void Vm::defer_continue(std::uint32_t handler_index, std::uint32_t jump_index) {
    if (handler_stack_.empty() || handler_stack_.back().handler_index != handler_index) {
        fail("invalid bytecode");
    }
    pending_control_.kind = PendingControlKind::Continue;
    pending_control_.jump_index = jump_index;
    const HandlerFrame& handler_frame = handler_stack_.back();
    const Bytecode& handler_bc = handler_bytecode(handler_frame);
    handler_stack_.pop_back();
    const HandlerInfo& handler = handler_bc.handlers[handler_index];
    if (handler.finally_ip != kInvalidHandlerIndex) {
        jump_to(handler.finally_ip);
    } else {
        complete_pending_control();
    }
}

void Vm::resolve_builtin_exception_classes() {
    builtin_exceptions_ = resolve_builtin_exceptions(bytecode_);
}

const Bytecode& Vm::code() const {
    return execution_bytecode_ != nullptr ? *execution_bytecode_ : bytecode_;
}

const Bytecode& Vm::module_code(std::uint32_t module_index) const {
    if (module_index >= bytecode_.modules.size()) {
        fail("invalid module index");
    }
    const ModuleDescriptor& descriptor = bytecode_.modules[module_index];
    if (descriptor.bytecode_index == 0xFFFFFFFFu ||
        (descriptor.canonical_path.empty() && !descriptor.is_native)) {
        fail("invalid module bytecode");
    }
    return bytecode_.module_bytecodes[descriptor.bytecode_index];
}

void Vm::ensure_module_states() {
    if (module_states_.size() < bytecode_.modules.size()) {
        module_states_.resize(bytecode_.modules.size());
    }
}

void Vm::begin_module_initialization(std::uint32_t module_index) {
    ensure_module_states();
    if (module_index >= bytecode_.modules.size()) {
        user_runtime_error("invalid module index");
    }
    ModuleRuntimeState& state = module_states_[module_index];
    if (state.state == ModuleInitState::Loaded) {
        return;
    }
    if (state.state == ModuleInitState::Failed) {
        user_runtime_error(state.failure_message.empty() ? "module initialization failed" : state.failure_message);
    }
    if (state.state == ModuleInitState::Loading) {
        return;
    }
    initialize_module(module_index);
}

void Vm::initialize_module(std::uint32_t module_index) {
    ensure_module_states();
    if (module_index >= bytecode_.modules.size()) {
        user_runtime_error("invalid module index");
    }
    ModuleRuntimeState& state = module_states_[module_index];
    if (state.state == ModuleInitState::Loaded) {
        return;
    }
    if (state.state == ModuleInitState::Failed) {
        user_runtime_error(state.failure_message.empty() ? "module initialization failed" : state.failure_message);
    }
    if (state.state == ModuleInitState::Loading) {
        return;
    }
    const ModuleDescriptor& descriptor = bytecode_.modules[module_index];
    if (descriptor.canonical_path.empty() && !descriptor.is_native) {
        user_runtime_error("cannot find module '" + descriptor.specifier + "'", RuntimeErrorCategory::Name);
    }
    const Bytecode& module_bc = module_code(module_index);
    if (descriptor.is_native) {
        state.globals.assign(module_bc.variables.size(), std::nullopt);
        state.state = ModuleInitState::Loading;
        try {
            initialize_native_module(module_index, bytecode_, state.globals, *runtime_);
            state.state = ModuleInitState::Loaded;
        } catch (const RuntimeError& err) {
            state.state = ModuleInitState::Failed;
            state.failure_message = err.message();
            user_runtime_error(err.message(), err.category());
        }
        return;
    }
    state.globals.assign(module_bc.variables.size(), std::nullopt);
    state.state = ModuleInitState::Loading;

    const std::size_t saved_ip = ip_;
    const bool saved_halted = halted_;
    const std::size_t saved_register_base = register_base_;
    std::vector<std::optional<Value>> saved_registers = std::move(registers_);
    std::vector<std::optional<Value>> saved_variables = std::move(variables_);
    std::vector<CallFrame> saved_frames = std::move(frames_);
    std::vector<HandlerFrame> saved_handlers = std::move(handler_stack_);
    const Bytecode* saved_code = execution_bytecode_;

    execution_bytecode_ = &module_bc;
    variables_ = state.globals;
    register_base_ = 0;
    registers_.assign(module_bc.register_count, std::nullopt);
    frames_.clear();
    handler_stack_.clear();
    pending_exception_ = std::nullopt;
    pending_control_.kind = PendingControlKind::None;
    ip_ = 0;
    halted_ = false;

    try {
        while (!halted_) {
            try {
                execute(fetch());
            } catch (...) {
                absorb_foreign_exception([this](const RuntimeError& err) { raise_runtime_error(err); });
            }
        }
        state.globals = variables_;
        state.state = ModuleInitState::Loaded;
    } catch (const VmError& err) {
        state.state = ModuleInitState::Failed;
        state.failure_message = err.message();
        execution_bytecode_ = saved_code != nullptr ? saved_code : &bytecode_;
        variables_ = std::move(saved_variables);
        register_base_ = saved_register_base;
        registers_ = std::move(saved_registers);
        frames_ = std::move(saved_frames);
        handler_stack_ = std::move(saved_handlers);
        ip_ = saved_ip;
        halted_ = saved_halted;
        throw;
    }

    execution_bytecode_ = saved_code != nullptr ? saved_code : &bytecode_;
    variables_ = std::move(saved_variables);
    register_base_ = saved_register_base;
    registers_ = std::move(saved_registers);
    frames_ = std::move(saved_frames);
    handler_stack_ = std::move(saved_handlers);
    ip_ = saved_ip;
    halted_ = saved_halted;
}

Value Vm::make_module_enum_export(std::uint32_t module_index, const ModuleExport& export_info) {
    const Bytecode& module_bc = module_code(module_index);
    if (export_info.index >= module_bc.enums.size()) {
        user_runtime_error("invalid module export '" + export_info.name + "'");
    }
    const EnumInfo& info = module_bc.enums[export_info.index];
    std::vector<std::pair<std::string, Value>> entries;
    entries.reserve(info.members.size());
    for (std::size_t i = 0; i < info.members.size(); ++i) {
        entries.emplace_back(info.members[i],
                             Runtime::make_enum(export_info.index, static_cast<std::uint32_t>(i), info.name,
                                                info.members[i]));
    }
    return Runtime::make_dict(std::move(entries));
}

void Vm::invoke_module_struct_ctor(std::uint32_t module_index, std::uint32_t struct_index, std::uint32_t dest) {
    initialize_module(module_index);
    const Bytecode& module_bc = module_code(module_index);
    if (struct_index >= module_bc.structs.size()) {
        fail("invalid struct index");
    }
    const StructInfo& info = module_bc.structs[struct_index];
    std::vector<std::string> field_names;
    field_names.reserve(info.fields.size());
    for (const StructFieldInfo& field : info.fields) {
        field_names.push_back(field.name);
    }
    write_register(dest) = Runtime::make_struct(struct_index, info.name, std::move(field_names));
}

void Vm::set_module_export_value(const Value& module_value, const std::string& export_name, Value value) {
    if (module_value.kind != ValueKind::Module || !module_value.module_namespace) {
        user_runtime_error("invalid module assignment");
    }
    const std::uint32_t module_index = module_value.module_namespace->module_index;
    begin_module_initialization(module_index);
    if (module_index >= bytecode_.modules.size()) {
        user_runtime_error("invalid module index");
    }
    for (const ModuleExport& export_info : bytecode_.modules[module_index].exports) {
        if (export_info.name != export_name) {
            continue;
        }
        if (export_info.kind != ExportKind::Variable) {
            user_runtime_error("cannot assign to module export '" + export_name + "'");
        }
        ModuleRuntimeState& state = module_states_[module_index];
        if (export_info.index >= state.globals.size()) {
            user_runtime_error("invalid module export '" + export_name + "'");
        }
        state.globals[export_info.index] = std::move(value);
        if (execution_bytecode_ == &module_code(module_index)) {
            variables_[export_info.index] = state.globals[export_info.index];
        }
        return;
    }
    user_runtime_error("module has no export '" + export_name + "'", RuntimeErrorCategory::Name);
}

Value Vm::make_module_export_value(std::uint32_t module_index, const ModuleExport& export_info) {
    switch (export_info.kind) {
        case ExportKind::Variable: {
            ModuleRuntimeState& state = module_states_[module_index];
            if (export_info.index >= state.globals.size() || !state.globals[export_info.index]) {
                user_runtime_error("uninitialized module export '" + export_info.name + "'");
            }
            return *state.globals[export_info.index];
        }
        case ExportKind::Function: {
            auto data = std::make_shared<ClosureData>();
            data->function_index = export_info.index;
            if (export_info.index < module_code(module_index).functions.size()) {
                data->name = module_code(module_index).functions[export_info.index].name;
            }
            data->module_index = module_index;
            return Runtime::make_function(std::move(data));
        }
        case ExportKind::Class: {
            auto data = std::make_shared<ClosureData>();
            data->module_index = module_index;
            data->module_class_ctor = true;
            data->class_index = export_info.index;
            if (export_info.index < module_code(module_index).classes.size()) {
                data->name = module_code(module_index).classes[export_info.index].name;
            }
            return Runtime::make_function(std::move(data));
        }
        case ExportKind::Enum:
            return make_module_enum_export(module_index, export_info);
        case ExportKind::Struct: {
            auto data = std::make_shared<ClosureData>();
            data->module_index = module_index;
            data->module_struct_ctor = true;
            data->struct_index = export_info.index;
            if (export_info.index < module_code(module_index).structs.size()) {
                data->name = module_code(module_index).structs[export_info.index].name;
            }
            return Runtime::make_function(std::move(data));
        }
    }
    user_runtime_error("unsupported module export '" + export_info.name + "'");
}

Value Vm::import_export_value(std::uint32_t module_index, const std::string& export_name) {
    begin_module_initialization(module_index);
    if (module_index >= bytecode_.modules.size()) {
        user_runtime_error("invalid module index");
    }
    for (const ModuleExport& export_info : bytecode_.modules[module_index].exports) {
        if (export_info.name == export_name) {
            return make_module_export_value(module_index, export_info);
        }
    }
    user_runtime_error("module has no export '" + export_name + "'");
}

Value Vm::import_module_value(std::uint32_t module_index) {
    begin_module_initialization(module_index);
    return Runtime::make_module(module_index);
}

Value Vm::resolve_module_index_value(const Value& object, const Value& index) {
    if (object.kind == ValueKind::Dict && object.dict) {
        if (index.kind != ValueKind::String) {
            user_runtime_error("dictionary key must be a string");
        }
        const Value* found = Runtime::dict_lookup(*object.dict, index.string);
        if (!found) {
            user_runtime_error("undefined dictionary key");
        }
        return *found;
    }
    if (object.kind != ValueKind::Module || !object.module_namespace) {
        return Runtime::get_index(object, index);
    }
    if (index.kind != ValueKind::String) {
        user_runtime_error("module member name must be a string");
    }
    return import_export_value(object.module_namespace->module_index, index.string);
}

void Vm::enter_module_bound_function(const FunctionInfo& fn, std::uint32_t module_index, std::uint32_t function_index,
                                     std::vector<Value> args, std::vector<std::shared_ptr<Cell>> captures,
                                     std::uint32_t dest, std::size_t return_ip, std::optional<Value> implicit_this) {
    initialize_module(module_index);
    if (fn.is_native) {
        try {
            bind_native_call_site();
            write_register(dest) =
                call_native_function(static_cast<NativeFunctionId>(fn.native_function_id), *runtime_, args);
            clear_native_call_site_binding();
        } catch (const RuntimeError& err) {
            clear_native_call_site_binding();
            raise_runtime_error(err);
        }
        return;
    }
    ensure_call_stack_capacity();
    CallFrame frame;
    frame.return_ip = return_ip;
    frame.dest = dest;
    frame.function_index = function_index;
    frame.module_index = module_index;
    frame.saved_code = execution_bytecode_;
    frame.saved_variables = std::move(variables_);
    frame.register_base = register_base_;
    frame.registers = std::move(registers_);
    frame.locals = std::move(locals_);
    frame.upvalues = std::move(upvalues_);
    frames_.push_back(std::move(frame));

    execution_bytecode_ = &module_code(module_index);
    variables_ = module_states_[module_index].globals;

    register_base_ = 0;
    registers_.assign(fn.register_count, std::nullopt);
    prepare_locals_for_function(fn, function_index);
    const std::uint32_t arg_offset = implicit_this ? 1u : 0u;
    if (implicit_this) {
        locals_[0]->value = std::move(*implicit_this);
    }
    for (std::uint32_t i = 0; i < fn.arity && i < args.size(); ++i) {
        locals_[arg_offset + i]->value = std::move(args[i]);
    }
    upvalues_ = std::move(captures);
    jump_to(fn.entry);
}

void Vm::invoke_module_class_ctor(std::uint32_t module_index, std::uint32_t class_index, const std::vector<Value>& args,
                                   std::uint32_t dest) {
    initialize_module(module_index);
    const Bytecode& module_bc = module_code(module_index);
    if (class_index >= module_bc.classes.size()) {
        fail("invalid class index");
    }
    const ClassInfo& info = module_bc.classes[class_index];
    if (info.native_class_kind != 0) {
        if (!args.empty()) {
            user_runtime_error("'" + info.name + "' expects 0 arguments");
        }
        const NativeClassKind kind = static_cast<NativeClassKind>(info.native_class_kind);
        if (kind == NativeClassKind::FileHandle || kind == NativeClassKind::DateTime ||
            kind == NativeClassKind::Duration || kind == NativeClassKind::TcpConnection ||
            kind == NativeClassKind::TcpServer || kind == NativeClassKind::UdpSocket ||
            kind == NativeClassKind::HttpResponse || kind == NativeClassKind::HttpRequest ||
            kind == NativeClassKind::HttpServer || kind == NativeClassKind::Database ||
            kind == NativeClassKind::SqlRows || kind == NativeClassKind::SqlStatement ||
            kind == NativeClassKind::SqlTransaction) {
            user_runtime_error("'" + info.name + "' cannot be constructed directly");
        }
        try {
            write_register(dest) = make_native_collection(kind, class_index);
        } catch (const RuntimeError& err) {
            raise_runtime_error(err);
        }
        return;
    }
    std::vector<std::string> field_names;
    field_names.reserve(info.layout.size());
    for (const ClassFieldLayout& field : info.layout) {
        field_names.push_back(field.name);
    }
    Value instance = Runtime::make_class(class_index, info.name, std::move(field_names));
    if (info.constructor_index != 0xFFFFFFFFu) {
        if (info.constructor_index >= module_bc.functions.size()) {
            fail("invalid function index");
        }
        const FunctionInfo& ctor = module_bc.functions[info.constructor_index];
        if (args.size() != ctor.arity) {
            user_runtime_error("'" + info.name + "' expects " + std::to_string(ctor.arity) + " arguments");
        }
        enter_module_bound_function(ctor, module_index, info.constructor_index, args, {}, dest, ip_, std::move(instance));
        return;
    }
    write_register(dest) = std::move(instance);
}

std::vector<StackTraceFrame> Vm::capture_stack_trace(std::size_t ip) const {
    const Bytecode& active_bytecode = code();
    std::vector<VmCallFrameInfo> frame_infos;
    frame_infos.reserve(frames_.size());
    for (const CallFrame& frame : frames_) {
        VmCallFrameInfo info;
        info.return_ip = frame.return_ip;
        info.function_index = frame.function_index;
        if (frame.module_index != 0xFFFFFFFFu) {
            info.function_bytecode = &module_code(frame.module_index);
        } else {
            info.function_bytecode = &bytecode_;
        }
        info.caller_bytecode = frame.saved_code != nullptr ? frame.saved_code : &bytecode_;
        frame_infos.push_back(info);
    }
    return ::pscript::capture_stack_trace(active_bytecode, ip, frame_infos, bytecode_);
}

void Vm::raise_runtime_error(const RuntimeError& err) {
    const std::size_t error_ip = ip_ > 0 ? ip_ - 1 : 0;
    const std::vector<StackTraceFrame> trace = capture_stack_trace(error_ip);
    if (is_stop_iteration_runtime_error(err) && builtin_exceptions_.stop_iteration != 0xFFFFFFFFu) {
        throw_value(make_named_exception_object(bytecode_, builtin_exceptions_.stop_iteration, err.message(),
                                                Runtime::make_null(), source_location_at(code(), error_ip), trace));
        return;
    }
    Value exception = make_exception_object(bytecode_, builtin_exceptions_, err.category(), err.message(),
                                              Runtime::make_null(), source_location_at(code(), error_ip), trace);
    throw_value(std::move(exception));
}

void Vm::fail_uncaught(Value value) {
    throw VmError(format_uncaught_exception(value, bytecode_));
}

void Vm::throw_value(Value value) {
    const std::size_t throw_ip = ip_ > 0 ? ip_ - 1 : 0;
    const std::vector<StackTraceFrame> trace = capture_stack_trace(throw_ip);
    const SourceLocation location = source_location_at(code(), throw_ip);
    value = enrich_exception_value(bytecode_, std::move(value), trace, location);
    if (debug_hook_ != nullptr) {
        debug_hook_->on_exception(*this, value, !handler_stack_.empty());
    }
    pending_exception_ = std::nullopt;
    pending_control_.kind = PendingControlKind::None;
    while (true) {
        while (!handler_stack_.empty()) {
            HandlerFrame& frame = handler_stack_.back();
            const Bytecode& handler_bc = handler_bytecode(frame);
            const HandlerInfo& handler = handler_bc.handlers[frame.handler_index];
            bool matched = false;
            for (std::uint32_t i = frame.next_catch; i < handler.catches.size(); ++i) {
                const CatchEntry& entry = handler.catches[i];
                if (!catch_entry_matches(entry, value, handler_bc)) {
                    continue;
                }
                matched = true;
                frame.next_catch = i + 1;
                unwind_to_frame_depth(frame.frame_depth);
                store_catch_binding(entry, value);
                jump_to(entry.catch_ip);
                return;
            }
            const std::size_t frame_depth = frame.frame_depth;
            handler_stack_.pop_back();
            if (!matched && handler.finally_ip != kInvalidHandlerIndex) {
                pending_exception_ = std::move(value);
                unwind_to_frame_depth(frame_depth);
                jump_to(handler.finally_ip);
                return;
            }
        }

        if (frames_.empty()) {
            if (runtime_callable_sandbox_depth_ > 0) {
                std::string message = format_exception_summary(value);
                if (value.kind == ValueKind::Class && value.class_value) {
                    const std::string prefix = value.class_value->type_name + ": ";
                    if (message.rfind(prefix, 0) == 0) {
                        message = message.substr(prefix.size());
                    }
                }
                throw RuntimeError(std::move(message), RuntimeErrorCategory::Value);
            }
            if (current_task_) {
                if (current_task_->cancel_requested || is_cancellation_exception(value)) {
                    cancel_task(current_task_, std::move(value));
                } else {
                    fail_task(current_task_, std::move(value));
                }
                task_yield_ = true;
                return;
            }
            fail_uncaught(std::move(value));
        }

        CallFrame frame = std::move(frames_.back());
        std::vector<Value> defers = std::move(frame.defer_stack);
        run_function_defers(defers);
        frames_.pop_back();
        restore_call_frame_state(frame);

        if (protocol_invoke_boundary_ && frames_.size() <= *protocol_invoke_boundary_) {
            if (protocol_catch_stop_iteration_ && is_stop_iteration(value, builtin_exceptions_, bytecode_)) {
                complete_protocol_invoke_stop_iteration();
            } else {
                handler_stack_.resize(protocol_invoke_resume_.saved_handlers);
                protocol_invoke_boundary_ = std::nullopt;
                protocol_catch_stop_iteration_ = false;
                protocol_invoke_resume_.active = false;
                throw_value(std::move(value));
            }
            return;
        }
    }
}

void Vm::end_finally() {
    if (pending_exception_) {
        Value value = std::move(*pending_exception_);
        pending_exception_ = std::nullopt;
        throw_value(std::move(value));
        return;
    }
    complete_pending_control();
}

void Vm::return_from_function(const Instruction& instruction) {
    // Preserve pending_exception_ across returns from finally helpers (e.g. context manager exit).
    pending_control_.kind = PendingControlKind::None;
    Value result = read_register(instruction.a);
    if (frames_.empty()) {
        fail("invalid bytecode");
    }
    if (!frames_.back().defer_stack.empty()) {
        std::vector<Value> defers = std::move(frames_.back().defer_stack);
        run_function_defers(defers);
    }
    CallFrame frame = std::move(frames_.back());
    if (session_profiler_ != nullptr) {
        session_profiler_->on_function_leave(frame.function_index);
    }
    if (frame.function_index < code().functions.size() && code().functions[frame.function_index].is_constructor &&
        !locals_.empty() && locals_[0] && locals_[0]->value) {
        result = *locals_[0]->value;
    }
    frames_.pop_back();
    if (frames_.empty() && current_task_) {
        finish_current_async_task(std::move(result), frame.module_index);
        return;
    }
    restore_call_frame_state(frame);
    write_register(frame.dest) = std::move(result);
    if (!frames_.empty() && frames_.back().runtime_invoke_root) {
        frames_.pop_back();
        halted_ = true;
    }
    if (protocol_invoke_resume_.active && protocol_invoke_boundary_ &&
        frames_.size() <= *protocol_invoke_boundary_) {
        complete_protocol_invoke_success(read_register(frame.dest));
    }
}

const Instruction& Vm::fetch() {
    if (ip_ >= code().instructions.size()) {
        fail("invalid bytecode");
    }
    return code().instructions[ip_++];
}

void Vm::execute(const Instruction& instruction) {
    if (++instruction_steps_ > limits::kMaxInstructionExecutions) {
        user_runtime_error("instruction execution limit exceeded");
    }
    if (session_profiler_ != nullptr) {
        session_profiler_->on_instruction(instruction.opcode, static_cast<std::uint32_t>(ip_ - 1),
                                          current_profile_function_index());
    }
    switch (instruction.opcode) {
        case Opcode::LoadConst:
            if (instruction.b < code().constants.size() &&
                code().constants[instruction.b].kind == ConstantKind::Integer) {
                assign_register_integer(write_register(instruction.a), cached_integer_constant(instruction.b));
            } else {
                write_register(instruction.a) = load_constant(instruction.b);
            }
            return;
        case Opcode::LoadVar:
            write_register(instruction.a) = read_variable(instruction.b);
            return;
        case Opcode::StoreVar:
            write_variable(instruction.a, read_register(instruction.b));
            return;
        case Opcode::LoadLocal: {
            const Value& local = read_local(instruction.b);
            if (local.kind == ValueKind::Integer) {
                assign_register_integer(write_register(instruction.a), local.integer);
            } else {
                write_register(instruction.a) = local;
            }
            return;
        }
        case Opcode::StoreLocal: {
            const Value& reg = read_register(instruction.b);
            if (reg.kind == ValueKind::Integer && instruction.a < locals_.size() && locals_[instruction.a]) {
                std::optional<Value>& cell = locals_[instruction.a]->value;
                if (!cell || cell->kind != ValueKind::Integer) {
                    write_local(instruction.a, reg);
                } else {
                    cell->integer = reg.integer;
                }
            } else {
                write_local(instruction.a, reg);
            }
            return;
        }
        case Opcode::LoadUpvalue:
            write_register(instruction.a) = read_upvalue(instruction.b);
            return;
        case Opcode::StoreUpvalue:
            write_upvalue(instruction.a, read_register(instruction.b));
            return;
        case Opcode::MakeClosure:
            make_closure(instruction);
            return;
        case Opcode::LoadFunction: {
            if (instruction.b >= code().functions.size()) {
                fail("invalid function index");
            }
            auto data = std::make_shared<ClosureData>();
            data->function_index = instruction.b;
            data->name = code().functions[instruction.b].name;
            write_register(instruction.a) = Runtime::make_function(std::move(data));
            return;
        }
        case Opcode::CallValue:
            call_value(instruction);
            return;
        case Opcode::UnaryPlus:
            write_register(instruction.a) = unary_plus(read_register(instruction.b));
            return;
        case Opcode::UnaryMinus:
            write_register(instruction.a) = unary_minus(read_register(instruction.b));
            return;
        case Opcode::UnaryNot:
            write_register(instruction.a) = Runtime::make_bool(!Runtime::is_truthy(read_register(instruction.b)));
            return;
        case Opcode::UnaryBitNot:
            write_register(instruction.a) = bitwise_not(read_register(instruction.b));
            return;
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::Div:
        case Opcode::Mod: {
            const Value& left = read_register(instruction.b);
            const Value& right = read_register(instruction.c);
            Value& dest = write_register(instruction.a);
            if (!try_fast_integer_arith(instruction.opcode, left, right, dest)) {
                dest = binary_arith(instruction.opcode, left, right);
            }
            return;
        }
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::Shl:
        case Opcode::Shr:
            write_register(instruction.a) =
                binary_bitwise(instruction.opcode, read_register(instruction.b), read_register(instruction.c));
            return;
        case Opcode::Contains: {
            try {
                write_register(instruction.a) =
                    Runtime::contains(read_register(instruction.b), read_register(instruction.c));
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        }
        case Opcode::Equal:
        case Opcode::NotEqual:
        case Opcode::Less:
        case Opcode::LessEqual:
        case Opcode::Greater:
        case Opcode::GreaterEqual:
            write_register(instruction.a) =
                binary_compare(instruction.opcode, read_register(instruction.b), read_register(instruction.c));
            return;
        case Opcode::Print:
            try {
                runtime_->print({read_register(instruction.a)});
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::PrintN: {
            if (instruction.b == 0) {
                fail("invalid instruction");
            }
            std::vector<Value> args;
            args.reserve(instruction.b);
            for (std::uint32_t i = 0; i < instruction.b; ++i) {
                args.push_back(read_register(instruction.a + i));
            }
            try {
                runtime_->print(args);
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        }
        case Opcode::Halt:
            halted_ = true;
            return;
        case Opcode::Jump:
            jump_to(instruction.a);
            return;
        case Opcode::JumpIfFalse:
            if (!Runtime::is_truthy(read_register(instruction.a))) {
                jump_to(instruction.b);
            }
            return;
        case Opcode::JumpIfTrue:
            if (Runtime::is_truthy(read_register(instruction.a))) {
                jump_to(instruction.b);
            }
            return;
        case Opcode::MakeList: {
            std::vector<Value> items;
            items.reserve(instruction.c);
            for (std::uint32_t i = 0; i < instruction.c; ++i) {
                items.push_back(read_register(instruction.b + i));
            }
            write_register(instruction.a) = Runtime::make_list(std::move(items));
            return;
        }
        case Opcode::MakeTuple: {
            std::vector<Value> items;
            items.reserve(instruction.c);
            for (std::uint32_t i = 0; i < instruction.c; ++i) {
                items.push_back(read_register(instruction.b + i));
            }
            write_register(instruction.a) = Runtime::make_tuple(std::move(items));
            return;
        }
        case Opcode::MakeSet: {
            std::vector<Value> items;
            items.reserve(instruction.c);
            for (std::uint32_t i = 0; i < instruction.c; ++i) {
                items.push_back(read_register(instruction.b + i));
            }
            write_register(instruction.a) = Runtime::make_set(std::move(items));
            return;
        }
        case Opcode::MakeStruct: {
            if (instruction.b >= code().structs.size()) {
                fail("invalid struct index");
            }
            const StructInfo& info = code().structs[instruction.b];
            std::vector<std::string> field_names;
            field_names.reserve(info.fields.size());
            for (const StructFieldInfo& field : info.fields) {
                field_names.push_back(field.name);
            }
            write_register(instruction.a) =
                Runtime::make_struct(instruction.b, info.name, std::move(field_names));
            return;
        }
        case Opcode::InvokeClass:
            invoke_class(instruction);
            return;
        case Opcode::CallClassMethod:
            call_class_method(instruction);
            return;
        case Opcode::InvokeSuper:
            invoke_super(instruction);
            return;
        case Opcode::GetField:
            try {
                write_register(instruction.a) =
                    Runtime::get_field(read_register(instruction.b), instruction.c);
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::SetField:
            try {
                Runtime::set_field(read_register(instruction.a), instruction.b, read_register(instruction.c));
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::LoadEnum: {
            if (instruction.b >= code().enums.size()) {
                fail("invalid enum index");
            }
            const EnumInfo& info = code().enums[instruction.b];
            if (instruction.c >= info.members.size()) {
                fail("invalid enum member");
            }
            write_register(instruction.a) = Runtime::make_enum(instruction.b, instruction.c, info.name,
                                                               info.members[instruction.c]);
            return;
        }
        case Opcode::MakeDict: {
            std::vector<std::pair<std::string, Value>> entries;
            entries.reserve(instruction.c);
            for (std::uint32_t i = 0; i < instruction.c; ++i) {
                const Value& key = read_register(instruction.b + i * 2);
                if (key.kind != ValueKind::String) {
                    user_runtime_error("dictionary key must be a string", RuntimeErrorCategory::Type);
                }
                entries.emplace_back(key.string, read_register(instruction.b + i * 2 + 1));
            }
            write_register(instruction.a) = Runtime::make_dict(std::move(entries));
            return;
        }
        case Opcode::CheckDestruct:
            try {
                Runtime::check_destructure(read_register(instruction.a), instruction.b, instruction.c);
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::TestDestructShape: {
            const std::uint32_t expected_count = instruction.c >> 8;
            const std::uint32_t kind = instruction.c & 0xFFu;
            write_register(instruction.a) =
                Runtime::make_bool(Runtime::test_destructure_shape(read_register(instruction.b), expected_count, kind));
            return;
        }
        case Opcode::TestInstanceOf: {
            const Value& value = read_register(instruction.b);
            bool matches = false;
            const std::uint32_t type_index = instruction.c & 0x7FFFFFFFu;
            const bool expect_class = (instruction.c & 0x80000000u) != 0;
            if (expect_class) {
                if (value.kind == ValueKind::Class && value.class_value) {
                    matches = is_class_or_subclass(value.class_value->type_index, type_index, code().classes);
                }
            } else if (value.kind == ValueKind::Struct && value.struct_value) {
                matches = value.struct_value->type_index == type_index;
            }
            write_register(instruction.a) = Runtime::make_bool(matches);
            return;
        }
        case Opcode::DestructRest:
            try {
                write_register(instruction.a) =
                    Runtime::destructure_rest(read_register(instruction.b), instruction.c);
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::DestructDictRest: {
            try {
                const std::uint32_t first = instruction.c >> 16;
                const std::uint32_t count = instruction.c & 0xFFFFu;
                std::vector<std::string> excluded;
                excluded.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    const Value& key = read_register(first + i);
                    if (key.kind != ValueKind::String) {
                        user_runtime_error("dictionary key must be a string", RuntimeErrorCategory::Type);
                    }
                    excluded.push_back(key.string);
                }
                write_register(instruction.a) =
                    Runtime::destructure_dict_rest(read_register(instruction.b), excluded);
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        }
        case Opcode::GetIndex:
            try {
                write_register(instruction.a) =
                    resolve_module_index_value(read_register(instruction.b), read_register(instruction.c));
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::ImportModule:
            write_register(instruction.a) = import_module_value(instruction.b);
            return;
        case Opcode::ImportName: {
            const Value name_value = load_constant(instruction.c);
            if (name_value.kind != ValueKind::String) {
                fail("invalid bytecode");
            }
            write_register(instruction.a) = import_export_value(instruction.b, name_value.string);
            return;
        }
        case Opcode::SetIndex: {
            const Value& object = read_register(instruction.a);
            const Value& index = read_register(instruction.b);
            const Value& value = read_register(instruction.c);
            try {
                if (object.kind == ValueKind::Module && index.kind == ValueKind::String) {
                    set_module_export_value(object, index.string, value);
                } else {
                    Runtime::set_index(object, index, value);
                }
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        }
        case Opcode::CallMethod: {
            if (instruction.c >= code().calls.size()) {
                fail("invalid call index");
            }
            const CallInfo& info = code().calls[instruction.c];
            const Value& receiver = read_register(instruction.b);
            std::vector<Value> args;
            args.reserve(info.argc);
            for (std::uint32_t i = 0; i < info.argc; ++i) {
                args.push_back(read_register(instruction.b + 1 + i));
            }
            try {
                if (receiver.kind == ValueKind::Module && receiver.module_namespace) {
                    Value callee = import_export_value(receiver.module_namespace->module_index, info.name);
                    if (callee.kind != ValueKind::Function || !callee.function) {
                        user_runtime_error("'" + info.name + "' is not callable");
                    }
                    if (callee.function->module_index != 0xFFFFFFFFu) {
                        if (callee.function->module_class_ctor) {
                            invoke_module_class_ctor(callee.function->module_index, callee.function->class_index, args,
                                                     instruction.a);
                        } else {
                            const Bytecode& module_bc = module_code(callee.function->module_index);
                            const FunctionInfo& fn = module_bc.functions[callee.function->function_index];
                            if (!fn.is_native && info.argc != fn.arity) {
                                user_runtime_error("'" + fn.name + "' expects " + std::to_string(fn.arity) +
                                                   " arguments");
                            }
                            if (fn.is_async) {
                                spawn_async_function(fn, callee.function->function_index, std::move(args),
                                                     callee.function->captures, instruction.a,
                                                     callee.function->module_index);
                            } else {
                                enter_module_bound_function(fn, callee.function->module_index,
                                                            callee.function->function_index, std::move(args),
                                                            callee.function->captures, instruction.a, ip_);
                            }
                        }
                    } else {
                        if (callee.function->function_index >= code().functions.size()) {
                            fail("invalid function index");
                        }
                        const FunctionInfo& fn = code().functions[callee.function->function_index];
                        if (info.argc != fn.arity) {
                            user_runtime_error("'" + fn.name + "' expects " + std::to_string(fn.arity) + " arguments");
                        }
                        enter_function(fn, callee.function->function_index, std::move(args), callee.function->captures,
                                       instruction.a, ip_);
                    }
                } else if (receiver.kind == ValueKind::Task && receiver.task) {
                    if (info.name == "cancel") {
                        if (info.argc != 0) {
                            user_runtime_error("'cancel' expects 0 arguments");
                        }
                        request_task_cancellation(receiver.task);
                        write_register(instruction.a) = Runtime::make_null();
                    } else {
                        user_runtime_error("Task has no method '" + info.name + "'");
                    }
                } else if (is_native_class_object(receiver)) {
                    write_register(instruction.a) = call_native_class_method(receiver, info.name, args, *runtime_);
                } else if (is_native_iterator(receiver)) {
                    if (info.name == "next") {
                        write_register(instruction.a) = native_iterator_next(receiver);
                    } else if (info.name == "iter") {
                        write_register(instruction.a) = native_iterator_iter(receiver);
                    } else {
                        user_runtime_error(std::string("Iterator has no method '") + info.name + "'");
                    }
                } else if (receiver.kind == ValueKind::Class && receiver.class_value) {
                    const ResolvedInstanceMethod resolved = lookup_instance_method(receiver, info.name);
                    if (!resolved.found || resolved.fn == nullptr) {
                        user_runtime_error(std::string("object has no method '") + info.name + "'",
                                           RuntimeErrorCategory::Type);
                    }
                    if (info.argc != resolved.fn->arity) {
                        user_runtime_error("'" + resolved.fn->name + "' expects " +
                                           std::to_string(resolved.fn->arity) + " arguments");
                    }
                    if (resolved.fn->is_async) {
                        spawn_async_function(*resolved.fn, resolved.function_index, std::move(args), {},
                                             instruction.a, resolved.module_index, receiver);
                    } else if (resolved.module_index != 0xFFFFFFFFu) {
                        enter_module_bound_function(*resolved.fn, resolved.module_index, resolved.function_index,
                                                    std::move(args), {}, instruction.a, ip_, receiver);
                    } else {
                        enter_function(*resolved.fn, resolved.function_index, std::move(args), {}, instruction.a, ip_,
                                       receiver);
                    }
                } else {
                    write_register(instruction.a) = Runtime::call_method(receiver, info.name, args);
                }
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        }
        case Opcode::CallBuiltin:
            execute_call_builtin(instruction);
            return;
        case Opcode::Await:
            execute_await(instruction);
            return;
        case Opcode::Call:
            call_function(instruction);
            return;
        case Opcode::Return:
            return_from_function(instruction);
            return;
        case Opcode::IterLen:
            try {
                write_register(instruction.a) = Runtime::iter_len(read_register(instruction.b));
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::IterGet:
            try {
                write_register(instruction.a) =
                    Runtime::iter_get(read_register(instruction.b), read_register(instruction.c));
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::GetIter:
            execute_get_iter(instruction);
            return;
        case Opcode::IterNext:
            execute_iter_next(instruction);
            return;
        case Opcode::CoerceToFloat:
            try {
                write_register(instruction.a) = coerce_to_float(read_register(instruction.b));
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        case Opcode::CheckAssign: {
            if (instruction.c >= code().type_checks.size()) {
                fail("invalid bytecode");
            }
            try {
                write_register(instruction.a) =
                    check_assign_type(read_register(instruction.b), code().type_checks[instruction.c],
                                      code().classes, code().type_checks);
            } catch (const RuntimeError& err) {
                raise_runtime_error(err);
            }
            return;
        }
        case Opcode::EnterTry:
            enter_try(instruction.a);
            return;
        case Opcode::LeaveTry:
            leave_try(instruction.a);
            return;
        case Opcode::Throw:
            throw_value(read_register(instruction.a));
            return;
        case Opcode::LoadPendingException:
            write_register(instruction.a) =
                pending_exception_ ? *pending_exception_ : Runtime::make_null();
            return;
        case Opcode::EndFinally:
            end_finally();
            return;
        case Opcode::DeferReturn:
            defer_return(instruction.a, instruction.b);
            return;
        case Opcode::DeferBreak:
            defer_break(instruction.a, instruction.b);
            return;
        case Opcode::DeferContinue:
            defer_continue(instruction.a, instruction.b);
            return;
        case Opcode::PushDefer: {
            if (frames_.empty()) {
                user_runtime_error("defer outside function");
            }
            const Value& thunk = read_register(instruction.a);
            if (thunk.kind != ValueKind::Function || !thunk.function) {
                user_runtime_error("defer requires a callable");
            }
            frames_.back().defer_stack.push_back(thunk);
            return;
        }
        case Opcode::SetEnumPayload: {
            Value& target = write_register(instruction.a);
            if (target.kind != ValueKind::Enum || !target.enum_value) {
                user_runtime_error("invalid enum value");
            }
            target.enum_value->payload = read_register(instruction.b);
            return;
        }
        case Opcode::GetEnumPayload: {
            const Value& value = read_register(instruction.b);
            if (value.kind != ValueKind::Enum || !value.enum_value) {
                user_runtime_error("invalid enum value");
            }
            write_register(instruction.a) = value.enum_value->payload;
            return;
        }
        case Opcode::TestEnumMember: {
            const Value& value = read_register(instruction.b);
            const std::uint32_t enum_index = instruction.c & 0xFFFFu;
            const std::uint32_t member_index = instruction.c >> 16;
            bool ok = value.kind == ValueKind::Enum && value.enum_value &&
                      value.enum_value->enum_index == enum_index &&
                      value.enum_value->member_index == member_index;
            write_register(instruction.a) = Runtime::make_bool(ok);
            return;
        }
        case Opcode::MakeEnumEx: {
            const std::uint32_t enum_index = instruction.b & 0xFFFFu;
            const std::uint32_t member_index = instruction.b >> 16;
            if (enum_index >= code().enums.size()) {
                fail("invalid enum index");
            }
            const EnumInfo& info = code().enums[enum_index];
            if (member_index >= info.members.size()) {
                fail("invalid enum member");
            }
            write_register(instruction.a) =
                Runtime::make_enum(enum_index, member_index, info.name, info.members[member_index],
                                   read_register(instruction.c));
            return;
        }
        default:
            fail("invalid instruction");
    }
}

void Vm::jump_to(std::uint32_t target) {
    if (target >= code().instructions.size()) {
        fail("invalid jump");
    }
    const std::size_t from = ip_;
    if (target < from) {
        if (session_profiler_ != nullptr) {
            session_profiler_->on_loop_back(current_profile_function_index(), target);
        }
        if (jit_enabled_ && !frames_.empty()) {
            const std::uint32_t fn_idx = frames_.back().function_index;
            const bool skip_osr =
                fn_idx < jit_eligibility_hints_.size() && jit_eligibility_hints_[fn_idx] == 2;
            if (!skip_osr && try_jit_osr_at_loop(target)) {
                return;
            }
        }
    }
    ip_ = target;
}

Value Vm::constant_to_value(const Constant& constant) {
    Value value;
    switch (constant.kind) {
        case ConstantKind::Integer:
            value.kind = ValueKind::Integer;
            value.integer = parse_integer(constant.lexeme);
            return value;
        case ConstantKind::Float:
            value.kind = ValueKind::Float;
            value.floating = parse_float(constant.lexeme);
            return value;
        case ConstantKind::String:
            value.kind = ValueKind::String;
            value.string = decode_string_lexeme(constant.lexeme);
            return value;
        case ConstantKind::Bool:
            value.kind = ValueKind::Bool;
            value.boolean = constant.bool_value;
            return value;
        case ConstantKind::Null:
            value.kind = ValueKind::Null;
            return value;
        case ConstantKind::Bytes:
            value = Runtime::make_bytes(constant.bytes_value);
            return value;
    }
    fail("invalid bytecode");
}

std::string Vm::decode_string_lexeme(const std::string& lexeme) {
    if (lexeme.size() < 2 || lexeme.front() != '"' || lexeme.back() != '"') {
        fail("invalid bytecode");
    }
    std::string out;
    out.reserve(lexeme.size() - 2);
    for (std::size_t i = 1; i + 1 < lexeme.size(); ++i) {
        const char c = lexeme[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= lexeme.size() - 1) {
            fail("invalid bytecode");
        }
        const char escaped = lexeme[++i];
        switch (escaped) {
            case '"':
            case '\\':
                out.push_back(escaped);
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'r':
                out.push_back('\r');
                break;
            default:
                fail("invalid bytecode");
        }
    }
    return out;
}

bool Vm::is_number(const Value& value) {
    return value.kind == ValueKind::Integer || value.kind == ValueKind::Float;
}

double Vm::as_number(const Value& value) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    if (value.kind == ValueKind::Float) {
        return value.floating;
    }
    user_runtime_error("unsupported operand type");
}

bool Vm::values_equal(const Value& left, const Value& right) {
    if (&left == &right) {
        return true;
    }
    if (left.kind == ValueKind::String && right.kind == ValueKind::String) {
        return Runtime::strings_equal(left.string, right.string);
    }
    return Runtime::equals(left, right);
}

Value Vm::unary_plus(const Value& value) {
    if (!is_number(value)) {
        user_runtime_error("unsupported operand type");
    }
    return value;
}

Value Vm::unary_minus(const Value& value) {
    if (is_native_duration(value)) {
        return duration_negate(value);
    }
    if (value.kind == ValueKind::Integer) {
        Value result;
        result.kind = ValueKind::Integer;
        result.integer = -value.integer;
        return result;
    }
    if (value.kind == ValueKind::Float) {
        Value result;
        result.kind = ValueKind::Float;
        result.floating = -value.floating;
        return result;
    }
    user_runtime_error("unsupported operand type");
}

Value Vm::bitwise_not(const Value& value) {
    if (value.kind != ValueKind::Integer) {
        user_runtime_error("unsupported operand type");
    }
    Value result;
    result.kind = ValueKind::Integer;
    result.integer = ~value.integer;
    return result;
}

Value Vm::binary_arith(Opcode opcode, const Value& left, const Value& right) {
    if (opcode == Opcode::Add && is_native_datetime(left) && is_native_duration(right)) {
        return datetime_add(left, right);
    }
    if (opcode == Opcode::Sub) {
        if (is_native_datetime(left) && is_native_duration(right)) {
            return datetime_subtract_duration(left, right);
        }
        if (is_native_datetime(left) && is_native_datetime(right)) {
            return datetime_subtract_datetime(left, right);
        }
    }

    if (opcode == Opcode::Mod) {
        if (left.kind != ValueKind::Integer || right.kind != ValueKind::Integer) {
            user_runtime_error("unsupported operand type");
        }
        if (right.integer == 0) {
            user_runtime_error("modulo by zero");
        }
        Value result;
        result.kind = ValueKind::Integer;
        result.integer = left.integer % right.integer;
        return result;
    }

    if (opcode == Opcode::Add && left.kind == ValueKind::String && right.kind == ValueKind::String) {
        return Runtime::concat_strings(left.string, right.string);
    }

    if (!is_number(left) || !is_number(right)) {
        user_runtime_error("unsupported operand type");
    }

    const bool integer_op = left.kind == ValueKind::Integer && right.kind == ValueKind::Integer;
    if (integer_op) {
        const std::int64_t a = left.integer;
        const std::int64_t b = right.integer;
        Value result;
        result.kind = ValueKind::Integer;
        switch (opcode) {
            case Opcode::Add:
                result.integer = a + b;
                return result;
            case Opcode::Sub:
                result.integer = a - b;
                return result;
            case Opcode::Mul:
                result.integer = a * b;
                return result;
            case Opcode::Div:
                if (b == 0) {
                    user_runtime_error("division by zero");
                }
                result.integer = a / b;
                return result;
            default:
                fail("invalid instruction");
        }
    }

    const double a = as_number(left);
    const double b = as_number(right);
    Value result;
    result.kind = ValueKind::Float;
    switch (opcode) {
        case Opcode::Add:
            result.floating = a + b;
            return result;
        case Opcode::Sub:
            result.floating = a - b;
            return result;
        case Opcode::Mul:
            result.floating = a * b;
            return result;
        case Opcode::Div:
            if (b == 0.0) {
                user_runtime_error("division by zero");
            }
            result.floating = a / b;
            return result;
        default:
            fail("invalid instruction");
    }
}

Value Vm::binary_bitwise(Opcode opcode, const Value& left, const Value& right) {
    if (left.kind != ValueKind::Integer || right.kind != ValueKind::Integer) {
        user_runtime_error("unsupported operand type");
    }
    Value result;
    result.kind = ValueKind::Integer;
    if (opcode == Opcode::Shl || opcode == Opcode::Shr) {
        if (right.integer < 0 || right.integer >= 64) {
            user_runtime_error("invalid shift");
        }
        const auto shift = static_cast<unsigned>(right.integer);
        if (opcode == Opcode::Shl) {
            result.integer = static_cast<std::int64_t>(static_cast<std::uint64_t>(left.integer) << shift);
            return result;
        }
        result.integer = left.integer >> shift;
        return result;
    }
    switch (opcode) {
        case Opcode::BitAnd:
            result.integer = left.integer & right.integer;
            return result;
        case Opcode::BitOr:
            result.integer = left.integer | right.integer;
            return result;
        case Opcode::BitXor:
            result.integer = left.integer ^ right.integer;
            return result;
        default:
            fail("invalid instruction");
    }
}

Value Vm::binary_compare(Opcode opcode, const Value& left, const Value& right) {
    Value result;
    result.kind = ValueKind::Bool;
    if (opcode == Opcode::Equal) {
        result.boolean = values_equal(left, right);
        return result;
    }
    if (opcode == Opcode::NotEqual) {
        result.boolean = !values_equal(left, right);
        return result;
    }
    if (is_native_datetime(left) && is_native_datetime(right)) {
        const int cmp = compare_datetime(left, right);
        switch (opcode) {
            case Opcode::Less:
                result.boolean = cmp < 0;
                return result;
            case Opcode::LessEqual:
                result.boolean = cmp <= 0;
                return result;
            case Opcode::Greater:
                result.boolean = cmp > 0;
                return result;
            case Opcode::GreaterEqual:
                result.boolean = cmp >= 0;
                return result;
            default:
                fail("invalid instruction");
        }
    }
    if (is_native_duration(left) && is_native_duration(right)) {
        const int cmp = compare_duration(left, right);
        switch (opcode) {
            case Opcode::Less:
                result.boolean = cmp < 0;
                return result;
            case Opcode::LessEqual:
                result.boolean = cmp <= 0;
                return result;
            case Opcode::Greater:
                result.boolean = cmp > 0;
                return result;
            case Opcode::GreaterEqual:
                result.boolean = cmp >= 0;
                return result;
            default:
                fail("invalid instruction");
        }
    }
    if (!is_number(left) || !is_number(right)) {
        user_runtime_error("unsupported operand type");
    }
    if (left.kind == ValueKind::Integer && right.kind == ValueKind::Integer) {
        const std::int64_t a = left.integer;
        const std::int64_t b = right.integer;
        switch (opcode) {
            case Opcode::Less:
                result.boolean = a < b;
                return result;
            case Opcode::LessEqual:
                result.boolean = a <= b;
                return result;
            case Opcode::Greater:
                result.boolean = a > b;
                return result;
            case Opcode::GreaterEqual:
                result.boolean = a >= b;
                return result;
            default:
                fail("invalid instruction");
        }
    }
    const double a = as_number(left);
    const double b = as_number(right);
    switch (opcode) {
        case Opcode::Less:
            result.boolean = a < b;
            return result;
        case Opcode::LessEqual:
            result.boolean = a <= b;
            return result;
        case Opcode::Greater:
            result.boolean = a > b;
            return result;
        case Opcode::GreaterEqual:
            result.boolean = a >= b;
            return result;
        default:
            fail("invalid instruction");
    }
}

const char* Vm::runtime_type_name(ValueKind kind) {
    switch (kind) {
        case ValueKind::Integer:
            return "int";
        case ValueKind::Float:
            return "float";
        case ValueKind::Bool:
            return "bool";
        case ValueKind::String:
            return "string";
        case ValueKind::Null:
            return "null";
        case ValueKind::Bytes:
            return "bytes";
        case ValueKind::List:
            return "list";
        case ValueKind::Dict:
            return "dict";
        case ValueKind::Tuple:
            return "tuple";
        case ValueKind::Set:
            return "set";
        case ValueKind::Struct:
            return "struct";
        case ValueKind::Class:
            return "class";
        case ValueKind::Enum:
            return "enum";
        case ValueKind::Range:
            return "range";
        case ValueKind::Function:
            return "function";
        case ValueKind::Module:
            return "module";
        case ValueKind::Task:
            return "task";
    }
    return "unknown";
}

Value Vm::coerce_to_float(const Value& value) {
    if (value.kind == ValueKind::Integer) {
        return Runtime::make_float(static_cast<double>(value.integer));
    }
    if (value.kind == ValueKind::Float) {
        return value;
    }
    user_runtime_error(std::string("Type error: expected float, got ") + runtime_type_name(value.kind),
                       RuntimeErrorCategory::Type);
}

bool Vm::is_class_or_subclass(std::uint32_t class_index, std::uint32_t ancestor_index,
                              const std::vector<ClassInfo>& classes) {
    for (std::uint32_t current = class_index; current != 0xFFFFFFFFu; current = classes[current].parent_index) {
        if (current == ancestor_index) {
            return true;
        }
    }
    return false;
}

Value Vm::check_assign_type(const Value& value, const TypeCheckInfo& check, const std::vector<ClassInfo>& classes,
                            const std::vector<TypeCheckInfo>& type_checks) {
    switch (check.expected) {
        case StaticType::Integer:
            if (value.kind != ValueKind::Integer) {
                user_runtime_error(std::string("Type error: expected int, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::Float:
            if (value.kind == ValueKind::Integer) {
                return Runtime::make_float(static_cast<double>(value.integer));
            }
            if (value.kind == ValueKind::Float) {
                return value;
            }
            user_runtime_error(std::string("Type error: expected float, got ") + runtime_type_name(value.kind),
                               RuntimeErrorCategory::Type);
        case StaticType::Bool:
            if (value.kind != ValueKind::Bool) {
                user_runtime_error(std::string("Type error: expected bool, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::String:
            if (value.kind != ValueKind::String) {
                user_runtime_error(std::string("Type error: expected string, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::Null:
            if (value.kind != ValueKind::Null) {
                user_runtime_error(std::string("Type error: expected null, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::List:
            if (value.kind != ValueKind::List || !value.list) {
                user_runtime_error(std::string("Type error: expected list, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            if (!check.type_arg_indices.empty()) {
                if (check.type_arg_indices[0] >= type_checks.size()) {
                    fail("invalid bytecode");
                }
                const TypeCheckInfo& element_check = type_checks[check.type_arg_indices[0]];
                for (const Value& item : value.list->items) {
                    check_assign_type(item, element_check, classes, type_checks);
                }
            }
            return value;
        case StaticType::Dict:
            if (value.kind != ValueKind::Dict || !value.dict) {
                user_runtime_error(std::string("Type error: expected dict, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            if (check.type_arg_indices.size() >= 2) {
                if (check.type_arg_indices[0] >= type_checks.size() ||
                    check.type_arg_indices[1] >= type_checks.size()) {
                    fail("invalid bytecode");
                }
                const TypeCheckInfo& key_check = type_checks[check.type_arg_indices[0]];
                const TypeCheckInfo& value_check = type_checks[check.type_arg_indices[1]];
                for (const auto& entry : value.dict->entries) {
                    Value key_value;
                    key_value.kind = ValueKind::String;
                    key_value.string = entry.first;
                    check_assign_type(key_value, key_check, classes, type_checks);
                    check_assign_type(entry.second, value_check, classes, type_checks);
                }
            }
            return value;
        case StaticType::Tuple:
            if (value.kind != ValueKind::Tuple) {
                user_runtime_error(std::string("Type error: expected tuple, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::Set:
            if (value.kind != ValueKind::Set) {
                user_runtime_error(std::string("Type error: expected set, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::Struct:
            if (value.kind != ValueKind::Struct || !value.struct_value) {
                user_runtime_error(std::string("Type error: expected struct, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            if (value.struct_value->type_index != check.struct_index) {
                user_runtime_error(std::string("Type error: incompatible struct assignment"),
                                   RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::Class: {
            if (value.kind != ValueKind::Class || !value.class_value) {
                const std::string expected =
                    check.class_index < classes.size() ? classes[check.class_index].name : "class";
                user_runtime_error(std::string("Type error: expected ") + expected + ", got " +
                                       runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            if (!is_class_or_subclass(value.class_value->type_index, check.class_index, classes)) {
                const std::string expected =
                    check.class_index < classes.size() ? classes[check.class_index].name : "class";
                user_runtime_error(std::string("Type error: expected ") + expected + ", got " +
                                       value.class_value->type_name,
                                   RuntimeErrorCategory::Type);
            }
            return value;
        }
        case StaticType::Enum:
            if (value.kind != ValueKind::Enum || !value.enum_value) {
                user_runtime_error(std::string("Type error: expected enum, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            if (value.enum_value->enum_index != check.enum_index) {
                user_runtime_error(std::string("Type error: expected enum, got enum"), RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::Task:
            if (value.kind != ValueKind::Task || !value.task) {
                user_runtime_error(std::string("Type error: expected task, got ") + runtime_type_name(value.kind),
                                   RuntimeErrorCategory::Type);
            }
            return value;
        case StaticType::Union:
            for (const std::uint32_t member_index : check.union_member_indices) {
                if (member_index >= type_checks.size()) {
                    fail("invalid bytecode");
                }
                try {
                    return check_assign_type(value, type_checks[member_index], classes, type_checks);
                } catch (const RuntimeError&) {
                }
            }
            user_runtime_error("Type error: value does not match union type", RuntimeErrorCategory::Type);
        case StaticType::Any:
        case StaticType::Unknown:
            return value;
    }
    fail("invalid bytecode");
}

void Vm::enqueue_task(std::shared_ptr<TaskData> task) {
    if (!task || !is_task_runnable(*task)) {
        return;
    }
    run_queue_.push_back(std::move(task));
}

void Vm::track_task(const std::shared_ptr<TaskData>& task) {
    if (!task) {
        return;
    }
    if (tracked_tasks_.size() >= limits::kMaxTrackedAsyncTasks) {
        user_runtime_error("maximum async task count exceeded");
    }
    tracked_tasks_.push_back(task);
}

bool Vm::is_task_terminal(const TaskData& task) const {
    return task.state == TaskState::Completed || task.state == TaskState::Failed ||
           task.state == TaskState::Cancelled;
}

bool Vm::is_task_cancellable(const TaskData& task) const {
    return !is_task_terminal(task);
}

bool Vm::is_task_runnable(const TaskData& task) const {
    return !is_task_terminal(task);
}

void Vm::prune_completed_timers() {
    timer_tasks_.erase(std::remove_if(timer_tasks_.begin(), timer_tasks_.end(),
                                      [](const std::shared_ptr<TaskData>& task) {
                                          return !task || task->state == TaskState::Completed ||
                                                 task->state == TaskState::Failed ||
                                                 task->state == TaskState::Cancelled;
                                      }),
                       timer_tasks_.end());
}

void Vm::release_terminal_task_resources(TaskData& task) {
    if (!is_task_terminal(task)) {
        return;
    }
    task.args.clear();
    task.captures.clear();
    task.saved_state.reset();
    task.race_contestants.clear();
    task.wait_all_contestants.clear();
    task.wait_all_results.clear();
    task.waiters.clear();
    task.timeout_target.reset();
    task.timeout_timer_task.reset();
}

void Vm::prune_tracked_tasks() {
    tracked_tasks_.erase(
        std::remove_if(tracked_tasks_.begin(), tracked_tasks_.end(),
                       [this](const std::shared_ptr<TaskData>& task) {
                           if (!task) {
                               return true;
                           }
                           if (!is_task_terminal(*task)) {
                               return false;
                           }
                           if (task == root_task_ || task == current_task_) {
                               return false;
                           }
                           return task.use_count() <= 1;
                       }),
        tracked_tasks_.end());
}

void Vm::check_scheduler_shutdown() {
    if (root_task_ && root_task_->state == TaskState::Failed) {
        fail_uncaught(root_task_->exception);
    }
    for (const std::shared_ptr<TaskData>& task : tracked_tasks_) {
        if (task && task->state == TaskState::Failed && !task->exception_consumed && task != root_task_) {
            fail_uncaught(task->exception);
        }
    }
}

void Vm::finish_current_async_task(Value result, std::uint32_t module_index) {
    if (module_index != 0xFFFFFFFFu) {
        module_states_[module_index].globals = variables_;
    }
    complete_task(current_task_, std::move(result));
    register_base_ = 0;
    registers_.clear();
    locals_.clear();
    upvalues_.clear();
    frames_.clear();
    handler_stack_.clear();
    pending_exception_ = std::nullopt;
    pending_control_.kind = PendingControlKind::None;
    task_yield_ = true;
}

const FunctionInfo& Vm::task_function(const TaskData& task) const {
    if (task.module_index != 0xFFFFFFFFu) {
        const Bytecode& module_bc = module_code(task.module_index);
        if (task.function_index >= module_bc.functions.size()) {
            fail("invalid function index");
        }
        return module_bc.functions[task.function_index];
    }
    if (task.function_index >= bytecode_.functions.size()) {
        fail("invalid function index");
    }
    return bytecode_.functions[task.function_index];
}

void Vm::spawn_async_function(const FunctionInfo& fn, std::uint32_t function_index, std::vector<Value> args,
                              std::vector<std::shared_ptr<Cell>> captures, std::uint32_t dest,
                              std::uint32_t module_index, std::optional<Value> implicit_this) {
    auto task = std::make_shared<TaskData>();
    task->function_index = function_index;
    task->module_index = module_index;
    task->args = std::move(args);
    task->captures = std::move(captures);
    task->implicit_this = std::move(implicit_this);
    task->return_dest = dest;
    write_register(dest) = Runtime::make_task(task);
    track_task(task);
    track_task_group_child(task);
    if (scheduler_active_) {
        enqueue_task(task);
    } else {
        deferred_background_tasks_.push_back(task);
    }
    (void)fn;
}

std::shared_ptr<TaskData> Vm::create_timer_task(std::int64_t milliseconds) {
    auto task = std::make_shared<TaskData>();
    task->wake_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    timer_tasks_.push_back(task);
    track_task(task);
    return task;
}

void Vm::complete_task(const std::shared_ptr<TaskData>& task, Value result) {
    if (!task) {
        return;
    }
    task->state = TaskState::Completed;
    task->result = std::move(result);
    task->saved_state.reset();
    task->awaiting_task.reset();
    wake_task_waiters(task);
    release_terminal_task_resources(*task);
    prune_tracked_tasks();
}

void Vm::cancel_task(const std::shared_ptr<TaskData>& task, Value exception) {
    if (!task || !is_task_cancellable(*task)) {
        return;
    }
    task->state = TaskState::Cancelled;
    task->exception = std::move(exception);
    task->saved_state.reset();
    task->awaiting_task.reset();
    task->cancel_requested = false;
    if (!task->is_wait_all_meta) {
        propagate_contestant_to_wait_all_metas(task, task->exception, true);
    }
    wake_task_waiters(task);
    release_terminal_task_resources(*task);
    prune_tracked_tasks();
}

void Vm::propagate_contestant_to_wait_all_metas(const std::shared_ptr<TaskData>& contestant, const Value& exception,
                                                bool cancelled) {
    if (!contestant) {
        return;
    }
    for (const std::shared_ptr<TaskData>& meta : tracked_tasks_) {
        if (!meta || !meta->is_wait_all_meta || is_task_terminal(*meta)) {
            continue;
        }
        bool contains = false;
        for (const std::shared_ptr<TaskData>& entry : meta->wait_all_contestants) {
            if (entry == contestant) {
                contains = true;
                break;
            }
        }
        if (!contains) {
            continue;
        }
        contestant->exception_consumed = true;
        if (cancelled) {
            cancel_task(meta, exception);
        } else {
            fail_task(meta, exception);
        }
        return;
    }
}

void Vm::fail_task(const std::shared_ptr<TaskData>& task, Value exception) {
    if (!task) {
        return;
    }
    if (is_task_terminal(*task)) {
        return;
    }
    cancel_task_group_siblings(task);
    if (task->task_group_member.lock()) {
        task->exception_consumed = true;
    }
    task->state = TaskState::Failed;
    task->exception = std::move(exception);
    task->saved_state.reset();
    task->awaiting_task.reset();
    if (!task->is_wait_all_meta) {
        propagate_contestant_to_wait_all_metas(task, task->exception, false);
    }
    wake_task_waiters(task);
    release_terminal_task_resources(*task);
    prune_tracked_tasks();
}

void Vm::wake_task_waiters(const std::shared_ptr<TaskData>& task) {
    for (const std::weak_ptr<TaskData>& weak : task->waiters) {
        if (auto waiter = weak.lock()) {
            if (is_task_terminal(*waiter)) {
                continue;
            }
            waiter->state = TaskState::Pending;
            enqueue_task(waiter);
        }
    }
    task->waiters.clear();
}

void Vm::remove_task_waiter(const std::shared_ptr<TaskData>& awaited, const std::shared_ptr<TaskData>& waiter) {
    if (!awaited || !waiter) {
        return;
    }
    auto& waiters = awaited->waiters;
    waiters.erase(std::remove_if(waiters.begin(), waiters.end(),
                                 [&waiter](const std::weak_ptr<TaskData>& weak) {
                                     const auto locked = weak.lock();
                                     return !locked || locked == waiter;
                                 }),
                  waiters.end());
}

bool Vm::is_timeout_timer_task(const TaskData& task) const {
    return task.timeout_meta_owner.lock() != nullptr;
}

bool Vm::is_cancellation_exception(const Value& value) const {
    if (builtin_exceptions_.cancellation == 0xFFFFFFFFu) {
        return false;
    }
    return is_subclass_of_exception(value, builtin_exceptions_.cancellation, bytecode_);
}

Value Vm::make_cancellation_exception(const std::string& message) {
    const std::size_t error_ip = ip_ > 0 ? ip_ - 1 : 0;
    const std::vector<StackTraceFrame> trace = capture_stack_trace(error_ip);
    const SourceLocation location = source_location_at(code(), error_ip);
    return make_named_exception_object(bytecode_, builtin_exceptions_.cancellation, message, Runtime::make_null(),
                                       location, trace);
}

Value Vm::make_timeout_exception(const std::string& message) {
    const std::size_t error_ip = ip_ > 0 ? ip_ - 1 : 0;
    const std::vector<StackTraceFrame> trace = capture_stack_trace(error_ip);
    const SourceLocation location = source_location_at(code(), error_ip);
    return make_named_exception_object(bytecode_, builtin_exceptions_.timeout, message, Runtime::make_null(), location,
                                       trace);
}

void Vm::request_task_cancellation(const std::shared_ptr<TaskData>& task) {
    if (!task || !is_task_cancellable(*task)) {
        return;
    }
    if (task->cancel_requested) {
        return;
    }
    task->cancel_requested = true;
    if (task->io_operation_id != 0) {
        runtime_->async_io().cancel_task_operations(task);
    }
    if (task->saved_state) {
        if (auto awaited = task->awaiting_task.lock()) {
            if (awaited->io_operation_id != 0) {
                runtime_->async_io().cancel_task_operations(awaited);
                runtime_->async_io().wake_waiters();
            }
            remove_task_waiter(awaited, task);
            if (awaited->function_index == 0xFFFFFFFFu && awaited->wake_time.has_value() && awaited->waiters.empty()) {
                complete_task(awaited, Runtime::make_null());
            }
        }
        enqueue_task(task);
    } else if (task->state == TaskState::Pending) {
        enqueue_task(task);
    }
}

void Vm::execute_task_cancellation(const std::shared_ptr<TaskData>& task) {
    if (!task || !is_task_cancellable(*task)) {
        current_task_.reset();
        return;
    }

    cancel_owned_task_groups(task);

    if (task->io_operation_id != 0 && !task->saved_state) {
        runtime_->async_io().cancel_task_operations(task);
        current_task_.reset();
        task_yield_ = true;
        return;
    }

    if (task->function_index == 0xFFFFFFFFu && task->wake_time.has_value() && !task->is_race_meta &&
        !task->is_timeout_meta) {
        cancel_task(task, make_cancellation_exception("Task was cancelled"));
        task_yield_ = true;
        current_task_.reset();
        return;
    }

    if (task->saved_state) {
        restore_execution_state(*task->saved_state);
        ip_ = task->saved_state->suspend_ip;
        task->saved_state.reset();
        task->awaiting_task.reset();
        throw_value(make_cancellation_exception("Task was cancelled"));
    }
}

void Vm::finish_race_meta_task(const std::shared_ptr<TaskData>& race) {
    if (!race || !race->is_race_meta || is_task_terminal(*race)) {
        current_task_.reset();
        return;
    }
    current_task_ = race;
    race->state = TaskState::Running;
    task_yield_ = true;
    for (const std::shared_ptr<TaskData>& contestant : race->race_contestants) {
        if (!contestant) {
            continue;
        }
        if (contestant->state == TaskState::Completed) {
            complete_task(race, contestant->result);
            current_task_.reset();
            return;
        }
        if (contestant->state == TaskState::Failed) {
            contestant->exception_consumed = true;
            fail_task(race, contestant->exception);
            current_task_.reset();
            return;
        }
        if (contestant->state == TaskState::Cancelled) {
            contestant->exception_consumed = true;
            cancel_task(race, contestant->exception);
            current_task_.reset();
            return;
        }
    }
    race->state = TaskState::Pending;
    current_task_.reset();
}

void Vm::disarm_timeout_timer(const std::shared_ptr<TaskData>& meta) {
    if (!meta || !meta->timeout_timer_task) {
        return;
    }
    if (is_task_runnable(*meta->timeout_timer_task)) {
        complete_task(meta->timeout_timer_task, Runtime::make_null());
    }
    meta->timeout_timer_task.reset();
}

void Vm::finish_timeout_meta_task(const std::shared_ptr<TaskData>& meta) {
    if (!meta || !meta->is_timeout_meta || is_task_terminal(*meta)) {
        current_task_.reset();
        return;
    }
    const std::shared_ptr<TaskData>& target = meta->timeout_target;
    if (!target) {
        complete_task(meta, Runtime::make_null());
        current_task_.reset();
        return;
    }
    disarm_timeout_timer(meta);
    if (target->state == TaskState::Completed) {
        complete_task(meta, target->result);
    } else if (target->state == TaskState::Failed) {
        target->exception_consumed = true;
        fail_task(meta, target->exception);
    } else if (target->state == TaskState::Cancelled) {
        target->exception_consumed = true;
        if (meta->timeout_expired) {
            fail_task(meta, make_timeout_exception("Task timed out"));
        } else {
            cancel_task(meta, target->exception);
        }
    }
    current_task_.reset();
}

void Vm::finish_timeout_on_timer(const std::shared_ptr<TaskData>& meta) {
    if (!meta || !meta->is_timeout_meta || is_task_terminal(*meta)) {
        return;
    }
    if (meta->timeout_target && is_task_terminal(*meta->timeout_target)) {
        finish_timeout_meta_task(meta);
        return;
    }
    meta->timeout_expired = true;
    disarm_timeout_timer(meta);
    if (meta->timeout_target) {
        if (meta->timeout_target->is_race_meta || meta->timeout_target->is_timeout_meta) {
            fail_task(meta, make_timeout_exception("Task timed out"));
            return;
        }
        if (is_task_cancellable(*meta->timeout_target)) {
            request_task_cancellation(meta->timeout_target);
        }
    }
}

void Vm::push_task_group(const std::shared_ptr<TaskGroupData>& group) {
    if (!group) {
        return;
    }
    if (current_task_) {
        group->owner_task = current_task_;
    }
    task_group_stack_.push_back(group);
}

void Vm::pop_task_group(const std::shared_ptr<TaskGroupData>& group) {
    if (task_group_stack_.empty()) {
        return;
    }
    if (group && task_group_stack_.back() != group) {
        auto it = std::find(task_group_stack_.begin(), task_group_stack_.end(), group);
        if (it != task_group_stack_.end()) {
            task_group_stack_.erase(it);
        }
        return;
    }
    task_group_stack_.pop_back();
}

void Vm::track_task_group_child(const std::shared_ptr<TaskData>& child) {
    if (!child || task_group_stack_.empty()) {
        return;
    }
    const std::shared_ptr<TaskGroupData>& group = task_group_stack_.back();
    group->track(child);
    child->task_group_member = group;
}

void Vm::cancel_task_group_siblings(const std::shared_ptr<TaskData>& failed_task) {
    if (!failed_task) {
        return;
    }
    if (const std::shared_ptr<TaskGroupData> group = failed_task->task_group_member.lock()) {
        for (const std::shared_ptr<TaskData>& child : group->live_children()) {
            if (child.get() != failed_task.get() && is_task_cancellable(*child)) {
                request_task_cancellation(child);
            }
        }
    }
    for (const std::shared_ptr<TaskData>& tracked : tracked_tasks_) {
        if (!tracked || !tracked->is_wait_all_meta) {
            continue;
        }
        bool contains = false;
        for (const std::shared_ptr<TaskData>& contestant : tracked->wait_all_contestants) {
            if (contestant == failed_task) {
                contains = true;
                break;
            }
        }
        if (!contains) {
            continue;
        }
        for (const std::shared_ptr<TaskData>& contestant : tracked->wait_all_contestants) {
            if (contestant && contestant != failed_task && is_task_cancellable(*contestant)) {
                request_task_cancellation(contestant);
            }
        }
    }
}

void Vm::cancel_owned_task_groups(const std::shared_ptr<TaskData>& owner) {
    if (!owner) {
        return;
    }
    for (const std::shared_ptr<TaskGroupData>& group : task_group_stack_) {
        if (!group) {
            continue;
        }
        if (group->owner_task.lock() != owner) {
            continue;
        }
        for (const std::shared_ptr<TaskData>& child : group->live_children()) {
            if (child != owner && is_task_cancellable(*child)) {
                request_task_cancellation(child);
            }
        }
    }
}

std::shared_ptr<TaskData> Vm::create_completed_value_task(Value value) {
    auto task = std::make_shared<TaskData>();
    track_task(task);
    complete_task(task, std::move(value));
    return task;
}

void Vm::finish_wait_all_meta_task(const std::shared_ptr<TaskData>& meta) {
    if (!meta || !meta->is_wait_all_meta || is_task_terminal(*meta)) {
        current_task_.reset();
        return;
    }
    const std::vector<std::shared_ptr<TaskData>>& contestants = meta->wait_all_contestants;
    if (contestants.empty()) {
        complete_task(meta, Runtime::make_list({}));
        current_task_.reset();
        return;
    }
    for (const std::shared_ptr<TaskData>& contestant : contestants) {
        if (contestant && contestant->state == TaskState::Failed) {
            contestant->exception_consumed = true;
            fail_task(meta, contestant->exception);
            current_task_.reset();
            return;
        }
    }
    for (const std::shared_ptr<TaskData>& contestant : contestants) {
        if (!contestant || contestant->state != TaskState::Cancelled) {
            continue;
        }
        bool others_pending = false;
        for (const std::shared_ptr<TaskData>& other : contestants) {
            if (other && other != contestant && !is_task_terminal(*other)) {
                others_pending = true;
                break;
            }
        }
        if (others_pending) {
            continue;
        }
        contestant->exception_consumed = true;
        cancel_task(meta, contestant->exception);
        current_task_.reset();
        return;
    }
    meta->wait_all_results.assign(contestants.size(), Runtime::make_null());
    bool all_terminal = true;
    for (std::size_t i = 0; i < contestants.size(); ++i) {
        const std::shared_ptr<TaskData>& contestant = contestants[i];
        if (!contestant) {
            all_terminal = false;
            continue;
        }
        if (contestant->state == TaskState::Completed) {
            meta->wait_all_results[i] = contestant->result;
            continue;
        }
        all_terminal = false;
    }
    if (all_terminal) {
        complete_task(meta, Runtime::make_list(meta->wait_all_results));
        current_task_.reset();
        return;
    }
    meta->state = TaskState::Pending;
    for (const std::shared_ptr<TaskData>& contestant : contestants) {
        if (!contestant || is_task_terminal(*contestant)) {
            continue;
        }
        ensure_task_io_started(contestant);
        contestant->waiters.push_back(meta);
    }
    current_task_.reset();
}

std::shared_ptr<TaskData> Vm::create_wait_all_task(const std::vector<Value>& contestants) {
    const std::shared_ptr<TaskData> caller = current_task_;
    auto meta = std::make_shared<TaskData>();
    meta->is_wait_all_meta = true;
    meta->wait_all_contestants.reserve(contestants.size());
    for (const Value& value : contestants) {
        if (value.kind != ValueKind::Task || !value.task) {
            user_runtime_error("'wait_all' expects Task arguments");
        }
        meta->wait_all_contestants.push_back(value.task);
    }
    track_task(meta);
    finish_wait_all_meta_task(meta);
    current_task_ = caller;
    return meta;
}

std::shared_ptr<TaskData> Vm::create_race_task(const std::vector<Value>& contestants) {
    auto race = std::make_shared<TaskData>();
    race->is_race_meta = true;
    race->race_contestants.reserve(contestants.size());
    for (const Value& value : contestants) {
        if (value.kind != ValueKind::Task || !value.task) {
            user_runtime_error("'race' expects Task arguments");
        }
        race->race_contestants.push_back(value.task);
    }
    track_task(race);
    for (const std::shared_ptr<TaskData>& contestant : race->race_contestants) {
        if (contestant->state == TaskState::Completed) {
            complete_task(race, contestant->result);
            return race;
        }
        if (contestant->state == TaskState::Failed) {
            contestant->exception_consumed = true;
            fail_task(race, contestant->exception);
            return race;
        }
        if (contestant->state == TaskState::Cancelled) {
            contestant->exception_consumed = true;
            cancel_task(race, contestant->exception);
            return race;
        }
    }
    for (const std::shared_ptr<TaskData>& contestant : race->race_contestants) {
        ensure_task_io_started(contestant);
        if (is_task_runnable(*contestant)) {
            contestant->waiters.push_back(race);
        }
    }
    return race;
}

std::shared_ptr<TaskData> Vm::create_timeout_task(const std::shared_ptr<TaskData>& target, std::int64_t milliseconds) {
    if (!target) {
        user_runtime_error("'timeout' expects a Task as the first argument");
    }
    if (milliseconds < 0) {
        user_runtime_error("'timeout' expects a non-negative integer timeout");
    }
    auto meta = std::make_shared<TaskData>();
    meta->is_timeout_meta = true;
    meta->timeout_target = target;
    track_task(meta);

    if (target->state == TaskState::Completed) {
        complete_task(meta, target->result);
        return meta;
    }
    if (target->state == TaskState::Failed) {
        fail_task(meta, target->exception);
        return meta;
    }
    if (target->state == TaskState::Cancelled) {
        cancel_task(meta, target->exception);
        return meta;
    }

    auto timer = std::make_shared<TaskData>();
    timer->wake_time = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    timer->timeout_meta_owner = meta;
    meta->timeout_timer_task = timer;
    timer_tasks_.push_back(timer);
    track_task(timer);
    ensure_task_io_started(target);
    target->waiters.push_back(meta);
    return meta;
}

void Vm::save_current_execution_state(const std::shared_ptr<TaskData>& task, std::uint32_t await_dest) {
    auto state = std::make_shared<AsyncExecutionState>();
    state->ip = ip_;
    state->suspend_ip = ip_ > 0 ? ip_ - 1 : 0;
    state->register_base = register_base_;
    state->registers = std::move(registers_);
    state->variables = std::move(variables_);
    state->locals = std::move(locals_);
    state->upvalues = std::move(upvalues_);
    state->execution_bytecode = execution_bytecode_;
    state->await_dest = await_dest;
    state->frames.reserve(frames_.size());
    for (const CallFrame& frame : frames_) {
        AsyncExecutionState::SavedCallFrame saved;
        saved.return_ip = frame.return_ip;
        saved.dest = frame.dest;
        saved.function_index = frame.function_index;
        saved.module_index = frame.module_index;
        saved.saved_code = frame.saved_code;
        saved.saved_variables = frame.saved_variables;
        saved.register_base = frame.register_base;
        saved.registers = frame.registers;
        saved.locals = frame.locals;
        saved.upvalues = frame.upvalues;
        saved.defer_stack = frame.defer_stack;
        state->frames.push_back(std::move(saved));
    }
    state->handler_stack.reserve(handler_stack_.size());
    for (const HandlerFrame& frame : handler_stack_) {
        AsyncExecutionState::SavedHandlerFrame saved;
        saved.handler_index = frame.handler_index;
        saved.frame_depth = frame.frame_depth;
        saved.next_catch = frame.next_catch;
        saved.owner_code = frame.owner_code;
        state->handler_stack.push_back(saved);
    }
    state->pending_exception = pending_exception_;
    state->pending_control_kind = static_cast<std::uint32_t>(pending_control_.kind);
    state->pending_control_return = pending_control_.return_value;
    state->pending_control_jump = pending_control_.jump_index;
    task->saved_state = std::move(state);
    register_base_ = 0;
    registers_.clear();
    variables_.clear();
    locals_.clear();
    upvalues_.clear();
    frames_.clear();
    handler_stack_.clear();
}

void Vm::restore_execution_state(const AsyncExecutionState& state) {
    ip_ = state.ip;
    register_base_ = state.register_base;
    registers_ = state.registers;
    variables_ = state.variables;
    locals_ = state.locals;
    upvalues_ = state.upvalues;
    execution_bytecode_ = state.execution_bytecode;
    frames_.clear();
    frames_.reserve(state.frames.size());
    for (const AsyncExecutionState::SavedCallFrame& saved : state.frames) {
        CallFrame frame;
        frame.return_ip = saved.return_ip;
        frame.dest = saved.dest;
        frame.function_index = saved.function_index;
        frame.module_index = saved.module_index;
        frame.saved_code = saved.saved_code;
        frame.saved_variables = saved.saved_variables;
        frame.register_base = saved.register_base;
        frame.registers = saved.registers;
        frame.locals = saved.locals;
        frame.upvalues = saved.upvalues;
        frame.defer_stack = saved.defer_stack;
        frames_.push_back(std::move(frame));
    }
    handler_stack_.clear();
    handler_stack_.reserve(state.handler_stack.size());
    for (const AsyncExecutionState::SavedHandlerFrame& saved : state.handler_stack) {
        HandlerFrame frame;
        frame.handler_index = saved.handler_index;
        frame.frame_depth = saved.frame_depth;
        frame.next_catch = saved.next_catch;
        frame.owner_code = saved.owner_code;
        handler_stack_.push_back(frame);
    }
    pending_exception_ = state.pending_exception;
    pending_control_.kind = static_cast<PendingControlKind>(state.pending_control_kind);
    pending_control_.return_value = state.pending_control_return;
    pending_control_.jump_index = state.pending_control_jump;
}

void Vm::resume_task_after_await(const std::shared_ptr<TaskData>& task) {
    if (!task || !task->saved_state) {
        fail("invalid task resume");
    }
    const std::shared_ptr<AsyncExecutionState> state = task->saved_state;
    const std::uint32_t await_dest = state->await_dest;
    restore_execution_state(*state);
    task->saved_state.reset();
    if (auto awaited = task->awaiting_task.lock()) {
        if (awaited->state == TaskState::Failed) {
            awaited->exception_consumed = true;
            task->awaiting_task.reset();
            throw_value(awaited->exception);
            return;
        }
        if (awaited->state == TaskState::Cancelled) {
            awaited->exception_consumed = true;
            task->awaiting_task.reset();
            throw_value(awaited->exception);
            return;
        }
        write_register(await_dest) = awaited->result;
        task->awaiting_task.reset();
    }
}

void Vm::start_task_execution(const std::shared_ptr<TaskData>& task) {
    if (!task || !is_task_runnable(*task)) {
        return;
    }
    if (task->is_race_meta) {
        finish_race_meta_task(task);
        return;
    }
    if (task->is_wait_all_meta) {
        finish_wait_all_meta_task(task);
        return;
    }
    if (task->is_timeout_meta) {
        finish_timeout_meta_task(task);
        return;
    }

    current_task_ = task;
    task->state = TaskState::Running;
    task_yield_ = false;
    // Top-level HALT sets halted_; background drain and scheduler must still run task bodies.
    halted_ = false;

    if (task->cancel_requested && task->function_index == 0xFFFFFFFFu && task->wake_time.has_value()) {
        execute_task_cancellation(task);
        current_task_.reset();
        return;
    }

    if (task->io_operation_id != 0 && !task->saved_state) {
        ensure_task_io_started(task);
        if (task->cancel_requested) {
            execute_task_cancellation(task);
        }
        current_task_.reset();
        return;
    }

    if (task->cancel_requested && task->saved_state) {
        const std::shared_ptr<AsyncExecutionState> state = task->saved_state;
        std::shared_ptr<TaskData> awaited;
        if (auto locked = task->awaiting_task.lock()) {
            awaited = std::move(locked);
            if (awaited->io_operation_id != 0) {
                runtime_->async_io().cancel_task_operations(awaited);
                runtime_->async_io().wake_waiters();
            }
        }
        restore_execution_state(*state);
        task->saved_state.reset();
        if (awaited) {
            remove_task_waiter(awaited, task);
            if (awaited->function_index == 0xFFFFFFFFu && awaited->wake_time.has_value() && awaited->waiters.empty()) {
                complete_task(awaited, Runtime::make_null());
            }
        }
        task->awaiting_task.reset();
        task->cancel_requested = false;
        Value cancellation = make_cancellation_exception("Task was cancelled");
        if (!handler_stack_.empty()) {
            HandlerFrame frame = handler_stack_.back();
            const Bytecode& handler_bc = handler_bytecode(frame);
            const HandlerInfo& handler = handler_bc.handlers[frame.handler_index];
            if (handler.finally_ip != kInvalidHandlerIndex) {
                pending_exception_ = std::move(cancellation);
                const std::size_t frame_depth = frame.frame_depth;
                handler_stack_.pop_back();
                unwind_to_frame_depth(frame_depth);
                jump_to(handler.finally_ip);
            } else {
                throw_value(std::move(cancellation));
                if (is_task_terminal(*task)) {
                    current_task_.reset();
                    return;
                }
            }
        } else {
            throw_value(std::move(cancellation));
            if (is_task_terminal(*task)) {
                current_task_.reset();
                return;
            }
        }
    } else if (task->saved_state) {
        resume_task_after_await(task);
    } else if (task->function_index != 0xFFFFFFFFu) {
        const FunctionInfo& fn = task_function(*task);
        if (variables_.empty() && !scheduler_globals_.empty()) {
            variables_ = scheduler_globals_;
        }
        if (task->module_index != 0xFFFFFFFFu) {
            enter_module_bound_function(fn, task->module_index, task->function_index, task->args, task->captures, 0, 0,
                                        task->implicit_this);
        } else {
            enter_function(fn, task->function_index, task->args, task->captures, 0, 0, task->implicit_this);
        }
    } else {
        complete_task(task, Runtime::make_null());
        task_yield_ = true;
        current_task_.reset();
        return;
    }

    while (!task_yield_ && !halted_ && !debug_execution_paused_) {
        try {
            if (!step_one_instruction()) {
                return;
            }
        } catch (...) {
            absorb_foreign_exception([this](const RuntimeError& err) { raise_runtime_error(err); });
        }
        if (task->cancel_requested) {
            task->cancel_requested = false;
            throw_value(make_cancellation_exception("Task was cancelled"));
            if (is_task_terminal(*task)) {
                current_task_.reset();
                return;
            }
        }
    }

    if (task->state == TaskState::Running && task_yield_) {
        task->state = TaskState::Pending;
    }
    current_task_.reset();
}

void Vm::run_task_until_yield(std::shared_ptr<TaskData> task) {
    start_task_execution(std::move(task));
}

void Vm::ensure_task_io_started(const std::shared_ptr<TaskData>& task) {
    if (runtime_ == nullptr || !task) {
        return;
    }
    if (task->io_operation_id != 0 && !task->saved_state) {
        runtime_->async_io().ensure_io_started(task->io_operation_id);
    }
}

void Vm::execute_await(const Instruction& instruction) {
    const Value& task_value = read_register(instruction.b);
    if (task_value.kind != ValueKind::Task || !task_value.task) {
        user_runtime_error("await expects a Task");
    }
    const std::shared_ptr<TaskData>& awaited = task_value.task;
    if (current_task_ && current_task_->cancel_requested) {
        current_task_->cancel_requested = false;
        throw_value(make_cancellation_exception("Task was cancelled"));
        return;
    }
    if (awaited->state == TaskState::Completed) {
        write_register(instruction.a) = awaited->result;
        return;
    }
    if (awaited->state == TaskState::Failed) {
        awaited->exception_consumed = true;
        throw_value(awaited->exception);
        return;
    }
    if (awaited->state == TaskState::Cancelled) {
        awaited->exception_consumed = true;
        throw_value(awaited->exception);
        return;
    }
    if (!current_task_) {
        user_runtime_error("await outside async execution");
    }
    ensure_task_io_started(awaited);
    save_current_execution_state(current_task_, instruction.a);
    current_task_->awaiting_task = awaited;
    awaited->waiters.push_back(current_task_);
    current_task_->state = TaskState::Pending;
    task_yield_ = true;
}

void Vm::process_ready_timers() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::shared_ptr<TaskData>> ready;
    ready.reserve(timer_tasks_.size());
    for (const std::shared_ptr<TaskData>& task : timer_tasks_) {
        if (!task || !is_task_runnable(*task)) {
            continue;
        }
        if (task->wake_time && *task->wake_time <= now) {
            ready.push_back(task);
        }
    }
    std::sort(ready.begin(), ready.end(),
              [](const std::shared_ptr<TaskData>& left, const std::shared_ptr<TaskData>& right) {
                  return *left->wake_time < *right->wake_time;
              });
    for (const std::shared_ptr<TaskData>& task : ready) {
        if (is_timeout_timer_task(*task)) {
            if (auto meta = task->timeout_meta_owner.lock()) {
                finish_timeout_on_timer(meta);
            }
            complete_task(task, Runtime::make_null());
        } else {
            complete_task(task, Runtime::make_null());
        }
    }
    prune_completed_timers();
    prune_tracked_tasks();
}

Value Vm::make_io_task_exception(RuntimeErrorCategory category, const std::string& message) {
    return make_exception_object(bytecode_, builtin_exceptions_, category, message, Runtime::make_null(),
                                 SourceLocation{0, 0}, {});
}

void Vm::process_io_completions() {
    if (runtime_ != nullptr) {
        runtime_->drain_callable_invokes();
        http_server_poll_pending_handlers(*runtime_);
    }
    for (AsyncIoCompletion& completion : runtime_->async_io().drain_completions()) {
        if (!completion.task || is_task_terminal(*completion.task)) {
            continue;
        }
        completion.task->io_operation_id = 0;
        if (completion.cancelled) {
            cancel_task(completion.task, make_cancellation_exception("Task was cancelled"));
            continue;
        }
        if (!completion.success) {
            fail_task(completion.task,
                      make_io_task_exception(completion.error_category, completion.error_message));
            continue;
        }
        complete_task(completion.task, std::move(completion.result));
    }
}

bool Vm::scheduler_has_pending_work() const {
    if (!run_queue_.empty()) {
        return true;
    }
    if (runtime_->has_pending_callable_invokes()) {
        return true;
    }
    if (runtime_->async_io().has_pending_completions()) {
        return true;
    }
    if (runtime_->async_io().has_in_flight_operations()) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    for (const std::shared_ptr<TaskData>& task : timer_tasks_) {
        if (task && task->state == TaskState::Pending && task->wake_time && *task->wake_time > now) {
            return true;
        }
    }
    for (const std::shared_ptr<TaskData>& task : timer_tasks_) {
        if (task && task->state == TaskState::Pending && task->wake_time && *task->wake_time <= now) {
            return true;
        }
    }
    return false;
}

void Vm::wait_for_scheduler_wake() {
    if (runtime_ != nullptr) {
        runtime_->drain_callable_invokes();
    }
    const auto now = std::chrono::steady_clock::now();
    auto next = now + std::chrono::hours(24);
    for (const std::shared_ptr<TaskData>& task : timer_tasks_) {
        if (task && task->wake_time && task->state == TaskState::Pending && *task->wake_time > now &&
            *task->wake_time < next) {
            next = *task->wake_time;
        }
    }
    runtime_->async_io().wait_until(next);
}

void Vm::run_async_scheduler() {
    scheduler_active_ = true;
    runtime_->set_async_scheduler_hooks(
        AsyncSchedulerHooks{
            [this](const std::shared_ptr<TaskData>& task) { track_task(task); },
            [this]() { return scheduler_active_; },
            [this](const Value& callable, const std::vector<Value>& args) {
                return invoke_callable_for_runtime(callable, args);
            },
            [this]() { return runtime_->has_pending_callable_invokes(); },
            [this](const std::shared_ptr<TaskGroupData>& group) { push_task_group(group); },
            [this](const std::shared_ptr<TaskGroupData>& group) { pop_task_group(group); },
            [this](const std::shared_ptr<TaskData>& child) { track_task_group_child(child); },
            [this](const std::vector<Value>& tasks) { return create_wait_all_task(tasks); },
        });
    if (bytecode_.async_main_function_index >= bytecode_.functions.size()) {
        fail("invalid async main function");
    }
    const FunctionInfo& main_fn = bytecode_.functions[bytecode_.async_main_function_index];
    auto main_task = std::make_shared<TaskData>();
    main_task->function_index = bytecode_.async_main_function_index;
    main_task->is_root = true;
    root_task_ = main_task;
    track_task(main_task);
    scheduler_globals_ = variables_;
    enqueue_task(main_task);

    run_async_scheduler_loop();
    if (!debug_execution_paused_) {
        finish_async_scheduler_shutdown();
    }
}

void Vm::run_async_scheduler_loop() {
    while (scheduler_has_pending_work()) {
        if (debug_execution_paused_) {
            return;
        }
        runtime_->drain_callable_invokes();
        process_io_completions();
        process_ready_timers();
        if (run_queue_.empty()) {
            bool pending_timer = false;
            for (const std::shared_ptr<TaskData>& timer_task : timer_tasks_) {
                if (timer_task && is_task_runnable(*timer_task) && timer_task->wake_time.has_value()) {
                    pending_timer = true;
                    break;
                }
            }
            if (root_task_ && is_task_terminal(*root_task_) && !pending_timer &&
                !runtime_->async_io().has_pending_completions() &&
                !runtime_->async_io().has_in_flight_operations() &&
                !runtime_->has_pending_callable_invokes()) {
                break;
            }
            wait_for_scheduler_wake();
            continue;
        }
        std::shared_ptr<TaskData> task = run_queue_.front();
        run_queue_.pop_front();
        if (!task || !is_task_runnable(*task)) {
            continue;
        }
        run_task_until_yield(task);
        if (debug_execution_paused_) {
            return;
        }
    }
}

void Vm::finish_async_scheduler_shutdown() {
    check_scheduler_shutdown();
    runtime_->clear_async_scheduler_hooks();
    runtime_->clear_callable_invoker();
    runtime_->clear_testing_runner_hooks();
    runtime_->async_io().shutdown();
    scheduler_active_ = false;
    halted_ = true;
    if (debug_hook_ != nullptr) {
        debug_hook_->on_program_exit(*this);
    }
}

void Vm::process_callable_invokes() {
    if (runtime_ != nullptr) {
        runtime_->drain_callable_invokes();
    }
}

void Vm::bind_native_call_site() {
    if (runtime_ == nullptr) {
        return;
    }
    const Bytecode& bytecode = code();
    const std::size_t site_ip = ip_ > 0 ? ip_ - 1 : 0;
    const SourceLocation location = source_location_at(bytecode, site_ip);
    const std::string file = bytecode.source_file.empty() ? "<stdin>" : bytecode.source_file;
    runtime_->set_native_call_site(file, location.line, location.column);
}

void Vm::clear_native_call_site_binding() {
    if (runtime_ != nullptr) {
        runtime_->clear_native_call_site();
    }
}

void Vm::run_scheduler_until_task_done(const std::shared_ptr<TaskData>& task) {
    while (task && !is_task_terminal(*task)) {
        runtime_->drain_callable_invokes();
        process_io_completions();
        process_ready_timers();
        if (!run_queue_.empty()) {
            std::shared_ptr<TaskData> next = run_queue_.front();
            run_queue_.pop_front();
            if (next && is_task_runnable(*next)) {
                run_task_until_yield(next);
            }
            continue;
        }
        if (!scheduler_has_pending_work()) {
            break;
        }
        wait_for_scheduler_wake();
    }
}

void Vm::drain_background_tasks_after_sync_program() {
    if (deferred_background_tasks_.empty()) {
        bool has_pending = false;
        for (const std::shared_ptr<TaskData>& task : tracked_tasks_) {
            if (task && !is_task_terminal(*task)) {
                has_pending = true;
                break;
            }
        }
        if (!has_pending) {
            return;
        }
    }
    ensure_scheduler_for_testing();
    scheduler_globals_ = variables_;
    for (const std::shared_ptr<TaskData>& task : deferred_background_tasks_) {
        if (task && !is_task_terminal(*task) && is_task_runnable(*task)) {
            enqueue_task(task);
        }
    }
    deferred_background_tasks_.clear();
    auto has_non_terminal_tracked = [this]() {
        for (const std::shared_ptr<TaskData>& task : tracked_tasks_) {
            if (task && !is_task_terminal(*task)) {
                return true;
            }
        }
        return false;
    };
    while (!run_queue_.empty() || has_non_terminal_tracked() || scheduler_has_pending_work()) {
        process_callable_invokes();
        process_io_completions();
        process_ready_timers();
        if (!run_queue_.empty()) {
            std::shared_ptr<TaskData> task = run_queue_.front();
            run_queue_.pop_front();
            if (task && is_task_runnable(*task)) {
                run_task_until_yield(task);
            }
            continue;
        }
        if (!has_non_terminal_tracked() && !scheduler_has_pending_work()) {
            break;
        }
        if (scheduler_has_pending_work()) {
            wait_for_scheduler_wake();
        } else {
            break;
        }
    }
    for (const std::shared_ptr<TaskData>& task : tracked_tasks_) {
        if (task && task->state == TaskState::Failed && !task->exception_consumed) {
            fail_uncaught(task->exception);
        }
    }
    scheduler_active_ = false;
    if (runtime_ != nullptr) {
        runtime_->clear_async_scheduler_hooks();
    }
}

void Vm::ensure_scheduler_for_testing() {
    if (scheduler_active_ || runtime_ == nullptr) {
        return;
    }
    runtime_->set_async_scheduler_hooks(
        AsyncSchedulerHooks{
            [this](const std::shared_ptr<TaskData>& task) { track_task(task); },
            [this]() { return scheduler_active_; },
            [this](const Value& callable, const std::vector<Value>& args) {
                return invoke_callable_for_runtime(callable, args);
            },
            [this]() { return runtime_->has_pending_callable_invokes(); },
            [this](const std::shared_ptr<TaskGroupData>& group) { push_task_group(group); },
            [this](const std::shared_ptr<TaskGroupData>& group) { pop_task_group(group); },
            [this](const std::shared_ptr<TaskData>& child) { track_task_group_child(child); },
            [this](const std::vector<Value>& tasks) { return create_wait_all_task(tasks); },
        });
    scheduler_active_ = true;
}

int Vm::run_discovered_tests(const std::optional<std::string>& filter) {
    if (runtime_ == nullptr) {
        return 1;
    }
    std::vector<std::string> names;
    for (const FunctionInfo& fn : bytecode_.functions) {
        if (fn.name.size() >= 5 && fn.name.compare(0, 5, "test_") == 0) {
            if (filter.has_value() && fn.name.find(*filter) == std::string::npos) {
                continue;
            }
            names.push_back(fn.name);
        }
    }
    std::sort(names.begin(), names.end());

    runtime_->output() << "Testing...\n";
    int passed = 0;
    int failed = 0;

    for (const std::string& name : names) {
        std::uint32_t function_index = 0xFFFFFFFFu;
        for (std::uint32_t i = 0; i < bytecode_.functions.size(); ++i) {
            if (bytecode_.functions[i].name == name) {
                function_index = i;
                break;
            }
        }
        if (function_index == 0xFFFFFFFFu) {
            runtime_->output() << "x " << name << " (not found)\n";
            ++failed;
            continue;
        }

        auto closure = std::make_shared<ClosureData>();
        closure->function_index = function_index;
        closure->name = name;
        const Value callable = Runtime::make_function(std::move(closure));
        const Bytecode& module_bc = bytecode_;
        if (function_index >= module_bc.functions.size()) {
            runtime_->output() << "x " << name << " (invalid)\n";
            ++failed;
            continue;
        }
        const std::shared_ptr<TaskData> saved_current_task = current_task_;
        const bool async_test = module_bc.functions[function_index].is_async;
        if (async_test) {
            ensure_scheduler_for_testing();
        }
        if (!async_test) {
            force_runtime_invoke_sandbox_ = true;
        }
        try {
            invoke_callable_for_runtime(callable, {});
            runtime_->output() << "ok " << name << '\n';
            ++passed;
        } catch (const VmError& err) {
            runtime_->output() << "x " << name << '\n';
            runtime_->output() << "  " << err.message() << '\n';
            ++failed;
        } catch (const RuntimeError& err) {
            runtime_->output() << "x " << name << '\n';
            runtime_->output() << "  " << err.message() << '\n';
            ++failed;
        } catch (const ProcessExit&) {
            runtime_->output() << "x " << name << '\n';
            runtime_->output() << "  unexpected process exit\n";
            ++failed;
        }
        force_runtime_invoke_sandbox_ = false;
        current_task_ = saved_current_task;
        runtime_->drain_callable_invokes();
        while (scheduler_active_ && scheduler_has_pending_work()) {
            process_callable_invokes();
            process_io_completions();
            process_ready_timers();
            if (!run_queue_.empty()) {
                std::shared_ptr<TaskData> task = run_queue_.front();
                run_queue_.pop_front();
                if (task && is_task_runnable(*task)) {
                    run_task_until_yield(task);
                }
            } else {
                wait_for_scheduler_wake();
            }
        }
    }

    runtime_->output() << passed << " passed, " << failed << " failed\n";
    if (failed > 0) {
        runtime_->signal_process_exit(1);
    }
    return failed > 0 ? 1 : 0;
}

void Vm::prepare_runtime_invocation_state() {
    execution_bytecode_ = &bytecode_;
    resolve_builtin_exception_classes();
}

Value Vm::invoke_callable_isolated(const Value& callee, const std::vector<Value>& args) {
    force_runtime_invoke_sandbox_ = true;
    return invoke_callable_for_runtime(callee, args);
}

Value Vm::invoke_callable_for_runtime(const Value& callee, const std::vector<Value>& args) {
    if (callee.kind != ValueKind::Function || !callee.function) {
        user_runtime_error("invalid callable");
    }
    const std::uint32_t module_index = callee.function->module_index;
    const std::uint32_t function_index = callee.function->function_index;
    const Bytecode& module_bc = module_index != 0xFFFFFFFFu ? module_code(module_index) : bytecode_;
    if (function_index >= module_bc.functions.size()) {
        fail("invalid function index");
    }
    const FunctionInfo& fn = module_bc.functions[function_index];
    std::vector<Value> call_args = args;
    const bool invoke_from_scheduler_idle = scheduler_active_ && frames_.empty();
    const bool sandboxed = force_runtime_invoke_sandbox_ || !invoke_from_scheduler_idle;

    const std::size_t saved_ip = ip_;
    std::vector<CallFrame> saved_frames;
    std::vector<std::optional<Value>> saved_registers;
    std::vector<std::optional<Value>> registers_backup;
    std::vector<std::shared_ptr<Cell>> saved_locals;
    std::vector<std::shared_ptr<Cell>> locals_backup;
    std::vector<std::shared_ptr<Cell>> saved_upvalues;
    std::vector<std::shared_ptr<Cell>> upvalues_backup;
    std::vector<HandlerFrame> saved_handlers;
    std::optional<Value> saved_pending;
    PendingControl saved_control{};
    bool saved_halted = false;
    bool saved_task_yield = false;
    const Bytecode* saved_code = execution_bytecode_;
    const std::size_t saved_register_base = register_base_;

    if (sandboxed) {
        ++runtime_callable_sandbox_depth_;
        if (registers_.empty()) {
            registers_.assign(1, std::nullopt);
        }
        saved_frames = std::move(frames_);
        saved_registers = std::move(registers_);
        registers_backup = saved_registers;
        saved_locals = std::move(locals_);
        locals_backup = saved_locals;
        saved_upvalues = std::move(upvalues_);
        upvalues_backup = saved_upvalues;
        saved_handlers = std::move(handler_stack_);
        saved_pending = pending_exception_;
        saved_control = pending_control_;
        saved_halted = halted_;
        saved_task_yield = task_yield_;
        saved_code = execution_bytecode_;

        frames_.clear();
        handler_stack_.clear();
        pending_exception_ = std::nullopt;
        pending_control_.kind = PendingControlKind::None;
        halted_ = false;
        task_yield_ = false;

        CallFrame root;
        root.return_ip = saved_ip;
        root.dest = 0;
        root.runtime_invoke_root = true;
        root.register_base = register_base_;
        root.registers = std::move(saved_registers);
        root.locals = std::move(saved_locals);
        root.upvalues = std::move(saved_upvalues);
        root.saved_code = saved_code;
        frames_.push_back(std::move(root));
        registers_ = frames_.back().registers;
        register_base_ = frames_.back().register_base;
        locals_ = frames_.back().locals;
        upvalues_ = frames_.back().upvalues;
    }

    const auto restore_sandbox = [&]() {
        if (!sandboxed) {
            return;
        }
        if (runtime_callable_sandbox_depth_ > 0) {
            --runtime_callable_sandbox_depth_;
        }
        ip_ = saved_ip;
        frames_ = std::move(saved_frames);
        register_base_ = saved_register_base;
        registers_ = std::move(registers_backup);
        locals_ = std::move(locals_backup);
        upvalues_ = std::move(upvalues_backup);
        handler_stack_ = std::move(saved_handlers);
        pending_exception_ = saved_pending;
        pending_control_ = saved_control;
        halted_ = saved_halted;
        task_yield_ = saved_task_yield;
        execution_bytecode_ = saved_code;
    };

    if (fn.is_async || invoke_from_scheduler_idle) {
        if (!scheduler_active_) {
            user_runtime_error("async handler requires async scheduler");
        }
        auto task = std::make_shared<TaskData>();
        task->function_index = function_index;
        task->module_index = module_index;
        task->args = std::move(call_args);
        task->captures = callee.function->captures;
        track_task(task);
        enqueue_task(task);
        try {
            run_scheduler_until_task_done(task);
        } catch (...) {
            restore_sandbox();
            throw;
        }
        if (task->state == TaskState::Failed) {
            task->exception_consumed = true;
            const Value exception = task->exception;
            restore_sandbox();
            std::string message = format_exception_summary(exception);
            if (exception.kind == ValueKind::Class && exception.class_value) {
                const std::string prefix = exception.class_value->type_name + ": ";
                if (message.rfind(prefix, 0) == 0) {
                    message = message.substr(prefix.size());
                }
            }
            throw RuntimeError(std::move(message), RuntimeErrorCategory::Value);
        }
        if (task->state == TaskState::Cancelled) {
            task->exception_consumed = true;
            const Value exception = task->exception;
            restore_sandbox();
            std::string message = format_exception_summary(exception);
            if (exception.kind == ValueKind::Class && exception.class_value) {
                const std::string prefix = exception.class_value->type_name + ": ";
                if (message.rfind(prefix, 0) == 0) {
                    message = message.substr(prefix.size());
                }
            }
            throw RuntimeError(std::move(message), RuntimeErrorCategory::Value);
        }
        Value result = task->result;
        restore_sandbox();
        return result;
    }

    constexpr std::uint32_t dest = 0;
    if (module_index != 0xFFFFFFFFu) {
        enter_module_bound_function(fn, module_index, function_index, std::move(call_args), callee.function->captures,
                                    dest, saved_ip);
    } else {
        enter_function(fn, function_index, std::move(call_args), callee.function->captures, dest, saved_ip);
    }

    std::exception_ptr invoke_error;
    while (!frames_.empty() && !halted_) {
        try {
            execute(fetch());
        } catch (const RuntimeError&) {
            invoke_error = std::current_exception();
            frames_.clear();
            break;
        } catch (const VmError&) {
            throw;
        } catch (const ProcessExit&) {
            throw;
        } catch (const std::exception& ex) {
            invoke_error = std::make_exception_ptr(RuntimeError(ex.what()));
            frames_.clear();
            break;
        }
    }

    Value result = Runtime::make_null();
    const std::size_t physical_dest = register_base_ + dest;
    if (physical_dest < registers_.size() && registers_[physical_dest]) {
        result = *registers_[physical_dest];
    }

    restore_sandbox();
    if (invoke_error) {
        std::rethrow_exception(invoke_error);
    }
    return result;
}

[[noreturn]] void Vm::fail(const std::string& message) {
    throw VmError(message);
}

}  // namespace pscript
