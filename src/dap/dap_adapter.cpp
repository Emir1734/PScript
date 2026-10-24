#include "dap/dap_adapter.h"

#include "dap/dap_framing.h"
#include "compiler/compiler.h"
#include "debug/debug_mapping.h"
#include "debug/debug_vm_inspector.h"
#include "parser/parser.h"
#include "vm/vm.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace pscript::dap {
namespace {

constexpr std::int64_t kGlobalsScopeBase = 1'000'000;

const lsp::JsonValue* field(const lsp::JsonValue& object, const char* key) {
    const std::optional<const lsp::JsonValue*> value = object.get(key);
    return value.has_value() ? *value : nullptr;
}

std::string json_string_field(const lsp::JsonValue& object, const char* key, std::string default_value = {}) {
    const lsp::JsonValue* value = field(object, key);
    if (value != nullptr && value->kind == lsp::JsonValue::Kind::String) {
        return value->string_value;
    }
    return default_value;
}

int json_int_field(const lsp::JsonValue& object, const char* key, int default_value = 0) {
    const lsp::JsonValue* value = field(object, key);
    if (value != nullptr && value->kind == lsp::JsonValue::Kind::Number) {
        return static_cast<int>(value->number_value);
    }
    return default_value;
}

bool json_bool_field(const lsp::JsonValue& object, const char* key, bool default_value = false) {
    const lsp::JsonValue* value = field(object, key);
    if (value != nullptr && value->kind == lsp::JsonValue::Kind::Bool) {
        return value->bool_value;
    }
    return default_value;
}

std::int64_t scope_ref(int frame_id, int tag) {
    return static_cast<std::int64_t>(frame_id) * 10 + tag;
}

int frame_id_from_scope_ref(std::int64_t ref, int& tag) {
    tag = static_cast<int>(ref % 10);
    return static_cast<int>(ref / 10);
}

}  // namespace

DapAdapter::DapAdapter() = default;

DapAdapter::~DapAdapter() {
    terminated_ = true;
    vm_cv_.notify_all();
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
}

int DapAdapter::send_event(const std::string& event, const lsp::JsonValue& body) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    lsp::JsonValue envelope = lsp::JsonValue::make_object({
        {"type", lsp::JsonValue::make_string("event")},
        {"seq", lsp::JsonValue::make_number(seq_++)},
        {"event", lsp::JsonValue::make_string(event)},
        {"body", body},
    });
    write_message(*out_, envelope.stringify());
    return 0;
}

int DapAdapter::send_response(int request_seq, const lsp::JsonValue& body) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    lsp::JsonValue envelope = lsp::JsonValue::make_object({
        {"type", lsp::JsonValue::make_string("response")},
        {"request_seq", lsp::JsonValue::make_number(request_seq)},
        {"success", lsp::JsonValue::make_bool(true)},
        {"seq", lsp::JsonValue::make_number(seq_++)},
        {"body", body},
    });
    write_message(*out_, envelope.stringify());
    return 0;
}

int DapAdapter::send_error_response(int request_seq, const std::string& command, const std::string& message,
                                    int error_code) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    lsp::JsonValue envelope = lsp::JsonValue::make_object({
        {"type", lsp::JsonValue::make_string("response")},
        {"request_seq", lsp::JsonValue::make_number(request_seq)},
        {"success", lsp::JsonValue::make_bool(false)},
        {"command", lsp::JsonValue::make_string(command)},
        {"message", lsp::JsonValue::make_string(message)},
        {"seq", lsp::JsonValue::make_number(seq_++)},
        {"body",
         lsp::JsonValue::make_object({
             {"error",
              lsp::JsonValue::make_object({
                  {"id", lsp::JsonValue::make_number(error_code)},
                  {"format", lsp::JsonValue::make_string(message)},
              })},
         })},
    });
    write_message(*out_, envelope.stringify());
    return 0;
}

std::string DapAdapter::normalize_path(std::string path) {
    try {
        std::filesystem::path p(path);
        p = std::filesystem::weakly_canonical(p);
        std::string out = p.generic_string();
        for (char& ch : out) {
            if (ch == '\\') {
                ch = '/';
            }
        }
        return out;
    } catch (...) {
        for (char& ch : path) {
            if (ch == '\\') {
                ch = '/';
            }
        }
        return path;
    }
}

std::string DapAdapter::resolve_source_path(const std::string& path) const {
    const std::string normalized = normalize_path(path);
    if (!program_path_.empty() && normalized == program_path_) {
        return bytecode_ ? bytecode_->source_file : normalized;
    }
    return normalized;
}

bool DapAdapter::compile_program(const std::string& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open program file";
        return false;
    }
    program_source_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    program_path_ = normalize_path(path);
    try {
        bytecode_ = std::make_unique<Bytecode>(compile_with_builtins(program_source_, program_path_));
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

void DapAdapter::on_event(const debug::DebugEvent& event) {
    switch (event.kind) {
        case debug::DebugEventKind::Continued:
            send_event("continued", lsp::JsonValue::make_object({{"threadId", lsp::JsonValue::make_number(1)}}));
            break;
        case debug::DebugEventKind::BreakpointHit:
        case debug::DebugEventKind::StepComplete:
        case debug::DebugEventKind::Exception:
        case debug::DebugEventKind::Stopped:
            send_stopped(event.kind == debug::DebugEventKind::Exception ? "exception" : "breakpoint");
            break;
        case debug::DebugEventKind::Exited:
            send_exited(0);
            send_terminated();
            terminated_ = true;
            break;
        case debug::DebugEventKind::Terminated:
            send_terminated();
            terminated_ = true;
            break;
        default:
            break;
    }
}

void DapAdapter::send_stopped(const std::string& reason, const std::string& text) {
    var_ref_to_child_id_.clear();
    std::unordered_map<std::string, lsp::JsonValue> body = {
        {"reason", lsp::JsonValue::make_string(reason)},
        {"threadId", lsp::JsonValue::make_number(1)},
        {"allThreadsStopped", lsp::JsonValue::make_bool(true)},
    };
    if (!text.empty()) {
        body.emplace("text", lsp::JsonValue::make_string(text));
    }
    send_event("stopped", lsp::JsonValue::make_object(std::move(body)));
}

void DapAdapter::send_terminated() {
    send_event("terminated", lsp::JsonValue::make_object({}));
}

void DapAdapter::send_exited(int exit_code) {
    send_event("exited", lsp::JsonValue::make_object({{"exitCode", lsp::JsonValue::make_number(exit_code)}}));
}

lsp::JsonValue DapAdapter::capabilities() const {
    return lsp::JsonValue::make_object({
        {"supportsConfigurationDoneRequest", lsp::JsonValue::make_bool(true)},
        {"supportsEvaluateForHovers", lsp::JsonValue::make_bool(true)},
        {"supportsConditionalBreakpoints", lsp::JsonValue::make_bool(true)},
        {"supportsHitConditionalBreakpoints", lsp::JsonValue::make_bool(true)},
        {"supportsFunctionBreakpoints", lsp::JsonValue::make_bool(true)},
        {"supportsExceptionFilterOptions", lsp::JsonValue::make_bool(true)},
        {"exceptionBreakpointFilters",
         lsp::JsonValue::make_array({
             lsp::JsonValue::make_object({
                 {"filter", lsp::JsonValue::make_string("uncaught")},
                 {"label", lsp::JsonValue::make_string("Uncaught Exceptions")},
                 {"default", lsp::JsonValue::make_bool(true)},
             }),
             lsp::JsonValue::make_object({
                 {"filter", lsp::JsonValue::make_string("all")},
                 {"label", lsp::JsonValue::make_string("All Exceptions")},
             }),
         })},
    });
}

void DapAdapter::handle_initialize(int seq, const lsp::JsonValue& /*args*/) {
    initialized_ = true;
    send_response(seq, lsp::JsonValue::make_object({{"capabilities", capabilities()}}));
    send_event("initialized", lsp::JsonValue::make_object({}));
}

void DapAdapter::handle_configuration_done(int seq) {
    configuration_done_ = true;
    send_response(seq, lsp::JsonValue::make_object({}));
    if (vm_ != nullptr && controller_ != nullptr) {
        start_debug_session();
    }
}

void DapAdapter::handle_launch(int seq, const lsp::JsonValue& args) {
    if (terminated_) {
        send_error_response(seq, "launch", "session already terminated");
        return;
    }
    const std::string program = json_string_field(args, "program");
    if (program.empty()) {
        send_error_response(seq, "launch", "launch requires 'program'");
        return;
    }
    std::string error;
    if (!compile_program(program, error)) {
        send_error_response(seq, "launch", error);
        return;
    }
    Bytecode compiled = std::move(*bytecode_);
    bytecode_ = std::make_unique<Bytecode>(compiled);
    program_output_.str("");
    program_output_.clear();
    runtime_ = std::make_unique<Runtime>(program_output_);
    vm_ = std::make_unique<Vm>(std::move(compiled), *runtime_);
    controller_ = std::make_unique<debug::ExecutionController>(vm_->bytecode());
    controller_->set_listener(this);
    controller_->attach(*vm_);
    send_response(seq, lsp::JsonValue::make_object({}));
    if (configuration_done_) {
        start_debug_session();
    }
}

void DapAdapter::start_debug_session() {
    if (vm_thread_running_.exchange(true)) {
        return;
    }
    vm_thread_ = std::thread([this] { vm_thread_main(); });
}

void DapAdapter::vm_thread_main() {
    while (!terminated_ && vm_ != nullptr && controller_ != nullptr) {
        vm_->run();
        if (terminated_) {
            break;
        }
        const debug::DebugState state = controller_->state();
        if (state == debug::DebugState::Exited || state == debug::DebugState::Stopped) {
            break;
        }
        if (state == debug::DebugState::Paused || state == debug::DebugState::Exception) {
            wait_for_vm_command();
        }
    }
    vm_thread_running_ = false;
}

void DapAdapter::post_command(PendingCommand::Kind kind) {
    {
        std::lock_guard<std::mutex> lock(vm_mutex_);
        pending_command_ = PendingCommand{kind};
    }
    vm_cv_.notify_one();
}

void DapAdapter::wait_for_vm_command() {
    std::unique_lock<std::mutex> lock(vm_mutex_);
    vm_cv_.wait(lock, [this] { return pending_command_.has_value() || terminated_; });
    if (terminated_ || !pending_command_.has_value()) {
        return;
    }
    const PendingCommand cmd = *pending_command_;
    pending_command_.reset();
    lock.unlock();

    if (!controller_ || !vm_) {
        return;
    }
    switch (cmd.kind) {
        case PendingCommand::Kind::Continue:
            controller_->continue_execution();
            vm_->debug_continue_execution();
            break;
        case PendingCommand::Kind::Next:
            controller_->step_over();
            vm_->debug_continue_execution();
            break;
        case PendingCommand::Kind::StepIn:
            controller_->step_into();
            vm_->debug_continue_execution();
            break;
        case PendingCommand::Kind::StepOut:
            controller_->step_out();
            vm_->debug_continue_execution();
            break;
        case PendingCommand::Kind::Pause:
            controller_->pause();
            send_stopped("pause");
            break;
        case PendingCommand::Kind::Disconnect:
        case PendingCommand::Kind::Terminate:
            controller_->detach(*vm_);
            terminated_ = true;
            break;
    }
}

void DapAdapter::handle_disconnect(int seq, const lsp::JsonValue& args) {
    const bool terminate = json_bool_field(args, "terminateDebuggee", true);
    if (terminate && controller_ && vm_) {
        post_command(PendingCommand::Kind::Disconnect);
    }
    terminated_ = true;
    vm_cv_.notify_all();
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
    send_response(seq, lsp::JsonValue::make_object({}));
}

void DapAdapter::handle_terminate(int seq) {
    post_command(PendingCommand::Kind::Terminate);
    terminated_ = true;
    vm_cv_.notify_all();
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
    send_response(seq, lsp::JsonValue::make_object({}));
}

void DapAdapter::handle_set_breakpoints(int seq, const lsp::JsonValue& args) {
    std::string source_path;
    if (const lsp::JsonValue* source = field(args, "source")) {
        source_path = resolve_source_path(json_string_field(*source, "path"));
    }
    const lsp::JsonValue* breakpoints = field(args, "breakpoints");
    std::vector<lsp::JsonValue> response_bps;
    if (controller_ == nullptr || !bytecode_) {
        send_response(seq, lsp::JsonValue::make_object({{"breakpoints", lsp::JsonValue::make_array({})}}));
        return;
    }
    for (const auto& entry : dap_to_internal_bp_) {
        controller_->remove_breakpoint(entry.second);
    }
    dap_to_internal_bp_.clear();

    if (breakpoints != nullptr && breakpoints->kind == lsp::JsonValue::Kind::Array) {
        for (const lsp::JsonValue& bp : breakpoints->array_value) {
            const int line = json_int_field(bp, "line", 0);
            const std::string condition = json_string_field(bp, "condition");
            const int hit_count = json_int_field(bp, "hitCondition", 0);
            const std::uint32_t dap_id = next_dap_bp_id_++;
            const std::uint32_t internal_id =
                controller_->add_line_breakpoint(bytecode_->source_file, static_cast<std::uint32_t>(line), condition);
            if (!condition.empty()) {
                controller_->set_breakpoint_condition(internal_id, condition);
            }
            if (hit_count > 0) {
                controller_->set_breakpoint_hit_count_target(internal_id, static_cast<std::uint32_t>(hit_count));
            }
            dap_to_internal_bp_[dap_id] = internal_id;
            const bool verified =
                debug::line_is_executable(*bytecode_, bytecode_->source_file, static_cast<std::uint32_t>(line));
            response_bps.push_back(lsp::JsonValue::make_object({
                {"id", lsp::JsonValue::make_number(dap_id)},
                {"verified", lsp::JsonValue::make_bool(verified)},
                {"line", lsp::JsonValue::make_number(line)},
            }));
        }
    }
    send_response(seq, lsp::JsonValue::make_object({{"breakpoints", lsp::JsonValue::make_array(response_bps)}}));
}

void DapAdapter::handle_set_function_breakpoints(int seq, const lsp::JsonValue& args) {
    const lsp::JsonValue* breakpoints = field(args, "breakpoints");
    std::vector<lsp::JsonValue> response_bps;
    if (controller_ != nullptr && breakpoints != nullptr && breakpoints->kind == lsp::JsonValue::Kind::Array) {
        for (const lsp::JsonValue& bp : breakpoints->array_value) {
            const std::string name = json_string_field(bp, "name");
            if (!name.empty()) {
                controller_->add_function_breakpoint(name);
            }
            response_bps.push_back(lsp::JsonValue::make_object({
                {"id", lsp::JsonValue::make_number(next_dap_bp_id_++)},
                {"verified", lsp::JsonValue::make_bool(!name.empty())},
            }));
        }
    }
    send_response(seq, lsp::JsonValue::make_object({{"breakpoints", lsp::JsonValue::make_array(response_bps)}}));
}

void DapAdapter::handle_set_exception_breakpoints(int seq, const lsp::JsonValue& args) {
    const lsp::JsonValue* filters = field(args, "filters");
    debug::ExceptionBreakpointMode mode = debug::ExceptionBreakpointMode::Uncaught;
    if (filters != nullptr && filters->kind == lsp::JsonValue::Kind::Array) {
        for (const lsp::JsonValue& filter : filters->array_value) {
            if (filter.kind == lsp::JsonValue::Kind::String && filter.string_value == "all") {
                mode = debug::ExceptionBreakpointMode::All;
            }
        }
        if (filters->array_value.empty()) {
            mode = debug::ExceptionBreakpointMode::None;
        }
    }
    if (controller_ != nullptr) {
        controller_->set_exception_breakpoint(mode);
    }
    send_response(seq, lsp::JsonValue::make_object({}));
}

void DapAdapter::handle_continue(int seq, const lsp::JsonValue& /*args*/) {
    post_command(PendingCommand::Kind::Continue);
    send_response(seq, lsp::JsonValue::make_object({{"allThreadsContinued", lsp::JsonValue::make_bool(true)}}));
}

void DapAdapter::handle_pause(int seq) {
    post_command(PendingCommand::Kind::Pause);
    send_response(seq, lsp::JsonValue::make_object({}));
}

void DapAdapter::handle_next(int seq) {
    post_command(PendingCommand::Kind::Next);
    send_response(seq, lsp::JsonValue::make_object({}));
}

void DapAdapter::handle_step_in(int seq) {
    post_command(PendingCommand::Kind::StepIn);
    send_response(seq, lsp::JsonValue::make_object({}));
}

void DapAdapter::handle_step_out(int seq) {
    post_command(PendingCommand::Kind::StepOut);
    send_response(seq, lsp::JsonValue::make_object({}));
}

void DapAdapter::handle_threads(int seq) {
    send_response(seq, lsp::JsonValue::make_object({
                        {"threads",
                         lsp::JsonValue::make_array({
                             lsp::JsonValue::make_object({
                                 {"id", lsp::JsonValue::make_number(1)},
                                 {"name", lsp::JsonValue::make_string("PScript")},
                             }),
                         })},
                    }));
}

lsp::JsonValue DapAdapter::stack_frame_json(const debug::DebugStackFrame& frame) const {
    return lsp::JsonValue::make_object({
        {"id", lsp::JsonValue::make_number(static_cast<int>(frame.index + 1))},
        {"name", lsp::JsonValue::make_string(frame.function_name.empty() ? "<module>" : frame.function_name)},
        {"source",
         lsp::JsonValue::make_object({
             {"path", lsp::JsonValue::make_string(frame.source_file)},
         })},
        {"line", lsp::JsonValue::make_number(static_cast<int>(frame.location.line))},
        {"column", lsp::JsonValue::make_number(static_cast<int>(frame.location.column))},
    });
}

void DapAdapter::handle_stack_trace(int seq, const lsp::JsonValue& args) {
    std::vector<lsp::JsonValue> frames;
    if (vm_ != nullptr) {
        debug::VmInspector inspector(*vm_);
        for (const debug::DebugStackFrame& frame : inspector.stack_frames()) {
            frames.push_back(stack_frame_json(frame));
        }
    }
    send_response(seq, lsp::JsonValue::make_object({
                        {"stackFrames", lsp::JsonValue::make_array(frames)},
                        {"totalFrames", lsp::JsonValue::make_number(static_cast<int>(frames.size()))},
                    }));
}

void DapAdapter::handle_scopes(int seq, const lsp::JsonValue& args) {
    const int frame_id = json_int_field(args, "frameId", 1);
    std::vector<lsp::JsonValue> scopes;
    scopes.push_back(lsp::JsonValue::make_object({
        {"name", lsp::JsonValue::make_string("Locals")},
        {"variablesReference", lsp::JsonValue::make_number(scope_ref(frame_id, 1))},
        {"expensive", lsp::JsonValue::make_bool(false)},
    }));
    scopes.push_back(lsp::JsonValue::make_object({
        {"name", lsp::JsonValue::make_string("Upvalues")},
        {"variablesReference", lsp::JsonValue::make_number(scope_ref(frame_id, 2))},
        {"expensive", lsp::JsonValue::make_bool(false)},
    }));
    scopes.push_back(lsp::JsonValue::make_object({
        {"name", lsp::JsonValue::make_string("Globals")},
        {"variablesReference", lsp::JsonValue::make_number(kGlobalsScopeBase + frame_id)},
        {"expensive", lsp::JsonValue::make_bool(true)},
    }));
    send_response(seq, lsp::JsonValue::make_object({{"scopes", lsp::JsonValue::make_array(scopes)}}));
}

std::int64_t DapAdapter::allocate_var_ref(std::uint32_t child_id) {
    const std::int64_t ref = next_var_ref_++;
    var_ref_to_child_id_[ref] = child_id;
    return ref;
}

void DapAdapter::handle_variables(int seq, const lsp::JsonValue& args) {
    const std::int64_t ref = static_cast<std::int64_t>(json_int_field(args, "variablesReference", 0));
    std::vector<lsp::JsonValue> variables;
    if (vm_ == nullptr) {
        send_response(seq, lsp::JsonValue::make_object({{"variables", lsp::JsonValue::make_array({})}}));
        return;
    }
    debug::VmInspector inspector(*vm_);
    const auto frames = inspector.stack_frames();

    if (ref >= kGlobalsScopeBase) {
        const int frame_id = static_cast<int>(ref - kGlobalsScopeBase);
        if (!bytecode_) {
            send_response(seq, lsp::JsonValue::make_object({{"variables", lsp::JsonValue::make_array({})}}));
            return;
        }
        for (const auto& global : bytecode_->variables) {
            const std::optional<Value> value = inspector.global_variable(global.name);
            if (!value.has_value()) {
                continue;
            }
            const debug::DebugValue inspected = inspector_.inspect(*value);
            if (!inspected.properties.empty()) {
                std::vector<lsp::JsonValue> children;
                for (const debug::DebugProperty& prop : inspected.properties) {
                    std::int64_t child_ref = 0;
                    if (prop.expandable && prop.child_id != 0) {
                        child_ref = allocate_var_ref(prop.child_id);
                    }
                    children.push_back(lsp::JsonValue::make_object({
                        {"name", lsp::JsonValue::make_string(prop.name)},
                        {"value", lsp::JsonValue::make_string(prop.summary)},
                        {"variablesReference", lsp::JsonValue::make_number(static_cast<double>(child_ref))},
                    }));
                }
                variables.push_back(lsp::JsonValue::make_object({
                    {"name", lsp::JsonValue::make_string(global.name)},
                    {"value", lsp::JsonValue::make_string(inspected.summary)},
                    {"variablesReference", lsp::JsonValue::make_number(static_cast<double>(next_var_ref_++))},
                    {"indexedVariables", lsp::JsonValue::make_number(0)},
                }));
            } else {
                variables.push_back(lsp::JsonValue::make_object({
                    {"name", lsp::JsonValue::make_string(global.name)},
                    {"value", lsp::JsonValue::make_string(inspected.summary)},
                    {"variablesReference", lsp::JsonValue::make_number(0)},
                }));
            }
        }
        (void)frame_id;
        send_response(seq, lsp::JsonValue::make_object({{"variables", lsp::JsonValue::make_array(variables)}}));
        return;
    }

    const auto child_it = var_ref_to_child_id_.find(ref);
    if (child_it != var_ref_to_child_id_.end()) {
        const Value* stored = inspector_.stored_value(child_it->second);
        if (stored != nullptr) {
            const debug::DebugValue inspected = inspector_.inspect(*stored);
            for (const debug::DebugProperty& prop : inspected.properties) {
                std::int64_t child_ref = 0;
                if (prop.expandable && prop.child_id != 0) {
                    child_ref = allocate_var_ref(prop.child_id);
                }
                variables.push_back(lsp::JsonValue::make_object({
                    {"name", lsp::JsonValue::make_string(prop.name)},
                    {"value", lsp::JsonValue::make_string(prop.summary)},
                    {"variablesReference", lsp::JsonValue::make_number(static_cast<double>(child_ref))},
                }));
            }
        }
        send_response(seq, lsp::JsonValue::make_object({{"variables", lsp::JsonValue::make_array(variables)}}));
        return;
    }

    int scope_tag = 0;
    const int frame_id = frame_id_from_scope_ref(ref, scope_tag);
    if (frame_id < 1 || static_cast<std::size_t>(frame_id - 1) >= frames.size()) {
        send_response(seq, lsp::JsonValue::make_object({{"variables", lsp::JsonValue::make_array({})}}));
        return;
    }
    const debug::DebugStackFrame& frame = frames[static_cast<std::size_t>(frame_id - 1)];
    const auto emit_vars = [&](const std::vector<debug::DebugVariable>& locals) {
        for (const debug::DebugVariable& local : locals) {
            if (!local.in_scope) {
                continue;
            }
            if (scope_tag == 1 && local.kind == DebugLocalKind::Upvalue) {
                continue;
            }
            if (scope_tag == 2 && local.kind != DebugLocalKind::Upvalue) {
                continue;
            }
            const debug::DebugValue inspected = inspector_.inspect(local.value);
            std::int64_t var_ref = 0;
            if (!inspected.properties.empty()) {
                var_ref = next_var_ref_++;
                for (const debug::DebugProperty& prop : inspected.properties) {
                    if (prop.expandable && prop.child_id != 0) {
                        var_ref_to_child_id_[var_ref] = prop.child_id;
                        break;
                    }
                }
            }
            variables.push_back(lsp::JsonValue::make_object({
                {"name", lsp::JsonValue::make_string(local.name)},
                {"value", lsp::JsonValue::make_string(inspected.summary)},
                {"variablesReference", lsp::JsonValue::make_number(static_cast<double>(var_ref))},
            }));
        }
    };
    if (scope_tag == 1 || scope_tag == 2) {
        emit_vars(frame.locals);
        emit_vars(frame.upvalues);
    }
    send_response(seq, lsp::JsonValue::make_object({{"variables", lsp::JsonValue::make_array(variables)}}));
}

void DapAdapter::handle_evaluate(int seq, const lsp::JsonValue& args) {
    const std::string expression = json_string_field(args, "expression");
    debug::EvalError error;
    std::string result;
    std::int64_t var_ref = 0;
    if (controller_ == nullptr) {
        send_error_response(seq, "evaluate", "no active debug session");
        return;
    }
    const std::optional<Value> value = controller_->evaluate_watch(expression, error);
    if (!value.has_value()) {
        send_response(seq, lsp::JsonValue::make_object({
                            {"result", lsp::JsonValue::make_string(error.message.empty() ? "error" : error.message)},
                            {"variablesReference", lsp::JsonValue::make_number(0)},
                        }));
        return;
    }
    const debug::DebugValue inspected = inspector_.inspect(*value);
    result = inspected.summary;
    if (!inspected.properties.empty()) {
        var_ref = next_var_ref_++;
        for (const debug::DebugProperty& prop : inspected.properties) {
            if (prop.expandable && prop.child_id != 0) {
                var_ref_to_child_id_[var_ref] = prop.child_id;
                break;
            }
        }
    }
    send_response(seq, lsp::JsonValue::make_object({
                        {"result", lsp::JsonValue::make_string(result)},
                        {"variablesReference", lsp::JsonValue::make_number(static_cast<double>(var_ref))},
                    }));
}

void DapAdapter::handle_source(int seq, const lsp::JsonValue& args) {
    const lsp::JsonValue* source = field(args, "source");
    std::string path;
    if (source != nullptr) {
        path = json_string_field(*source, "path");
    }
    if (path.empty()) {
        path = program_path_;
    }
    std::string content = program_source_;
    if (normalize_path(path) != program_path_) {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
    }
    send_response(seq, lsp::JsonValue::make_object({
                        {"content", lsp::JsonValue::make_string(content)},
                        {"mimeType", lsp::JsonValue::make_string("text/x-pscript")},
                    }));
}

void DapAdapter::handle_request(const lsp::JsonValue& request) {
    const int seq = json_int_field(request, "seq", 0);
    const std::string command = json_string_field(request, "command");
    const lsp::JsonValue* args = field(request, "arguments");
    const lsp::JsonValue empty_args = lsp::JsonValue::make_object({});
    const lsp::JsonValue& arg_ref = args != nullptr ? *args : empty_args;

    if (command == "initialize") {
        handle_initialize(seq, arg_ref);
    } else if (command == "launch") {
        handle_launch(seq, arg_ref);
    } else if (command == "disconnect") {
        handle_disconnect(seq, arg_ref);
    } else if (command == "terminate") {
        handle_terminate(seq);
    } else if (command == "configurationDone") {
        handle_configuration_done(seq);
    } else if (command == "setBreakpoints") {
        handle_set_breakpoints(seq, arg_ref);
    } else if (command == "setFunctionBreakpoints") {
        handle_set_function_breakpoints(seq, arg_ref);
    } else if (command == "setExceptionBreakpoints") {
        handle_set_exception_breakpoints(seq, arg_ref);
    } else if (command == "continue") {
        handle_continue(seq, arg_ref);
    } else if (command == "pause") {
        handle_pause(seq);
    } else if (command == "next") {
        handle_next(seq);
    } else if (command == "stepIn") {
        handle_step_in(seq);
    } else if (command == "stepOut") {
        handle_step_out(seq);
    } else if (command == "stackTrace") {
        handle_stack_trace(seq, arg_ref);
    } else if (command == "scopes") {
        handle_scopes(seq, arg_ref);
    } else if (command == "variables") {
        handle_variables(seq, arg_ref);
    } else if (command == "evaluate") {
        handle_evaluate(seq, arg_ref);
    } else if (command == "threads") {
        handle_threads(seq);
    } else if (command == "source") {
        handle_source(seq, arg_ref);
    } else if (command == "attach") {
        send_error_response(seq, command, "attach not supported; use launch", 1002);
    } else {
        send_response(seq, lsp::JsonValue::make_object({}));
    }
}

int DapAdapter::run(std::istream& in, std::ostream& out) {
    in_ = &in;
    out_ = &out;
    std::string body;
    while (read_message(in, body)) {
        const std::optional<lsp::JsonValue> parsed = lsp::parse_json(body);
        if (!parsed.has_value()) {
            continue;
        }
        const lsp::JsonValue* type = field(*parsed, "type");
        if (type == nullptr || type->kind != lsp::JsonValue::Kind::String) {
            continue;
        }
        if (type->string_value == "request") {
            handle_request(*parsed);
        }
    }
    terminated_ = true;
    vm_cv_.notify_all();
    if (vm_thread_.joinable()) {
        vm_thread_.join();
    }
    return 0;
}

}  // namespace pscript::dap
