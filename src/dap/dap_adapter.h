#pragma once

#include "debug/debug_session.h"
#include "debug/debug_value.h"
#include "lsp/json_util.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pscript {

namespace dap {

class DapAdapter : public debug::IDebugEventListener {
public:
    DapAdapter();
    ~DapAdapter();
    int run(std::istream& in, std::ostream& out);

    void on_event(const debug::DebugEvent& event) override;

private:
    struct PendingCommand {
        enum class Kind { Continue, Pause, Next, StepIn, StepOut, Disconnect, Terminate } kind = Kind::Continue;
    };

    std::istream* in_ = nullptr;
    std::ostream* out_ = nullptr;
    std::mutex io_mutex_;
    int seq_ = 1;
    bool initialized_ = false;
    bool configuration_done_ = false;
    bool terminated_ = false;

    std::unique_ptr<Bytecode> bytecode_;
    std::ostringstream program_output_;
    std::unique_ptr<Runtime> runtime_;
    std::unique_ptr<Vm> vm_;
    std::unique_ptr<debug::ExecutionController> controller_;
    std::string program_path_;
    std::string program_source_;

    std::thread vm_thread_;
    std::mutex vm_mutex_;
    std::condition_variable vm_cv_;
    std::optional<PendingCommand> pending_command_;
    std::atomic<bool> vm_thread_running_{false};

    std::unordered_map<std::uint32_t, std::uint32_t> dap_to_internal_bp_;
    std::uint32_t next_dap_bp_id_ = 1;

    debug::ValueInspector inspector_;
    std::unordered_map<std::int64_t, std::uint32_t> var_ref_to_child_id_;
    std::int64_t next_var_ref_ = 1;

    int send_event(const std::string& event, const lsp::JsonValue& body);
    int send_response(int request_seq, const lsp::JsonValue& body);
    int send_error_response(int request_seq, const std::string& command, const std::string& message, int error_code = 1000);

    void handle_request(const lsp::JsonValue& request);
    void handle_initialize(int seq, const lsp::JsonValue& args);
    void handle_launch(int seq, const lsp::JsonValue& args);
    void handle_disconnect(int seq, const lsp::JsonValue& args);
    void handle_terminate(int seq);
    void handle_configuration_done(int seq);
    void handle_set_breakpoints(int seq, const lsp::JsonValue& args);
    void handle_set_function_breakpoints(int seq, const lsp::JsonValue& args);
    void handle_set_exception_breakpoints(int seq, const lsp::JsonValue& args);
    void handle_continue(int seq, const lsp::JsonValue& args);
    void handle_pause(int seq);
    void handle_next(int seq);
    void handle_step_in(int seq);
    void handle_step_out(int seq);
    void handle_stack_trace(int seq, const lsp::JsonValue& args);
    void handle_scopes(int seq, const lsp::JsonValue& args);
    void handle_variables(int seq, const lsp::JsonValue& args);
    void handle_evaluate(int seq, const lsp::JsonValue& args);
    void handle_threads(int seq);
    void handle_source(int seq, const lsp::JsonValue& args);

    void start_debug_session();
    void vm_thread_main();
    void post_command(PendingCommand::Kind kind);
    void wait_for_vm_command();
    void send_stopped(const std::string& reason, const std::string& text = {});
    void send_terminated();
    void send_exited(int exit_code);

    static std::string normalize_path(std::string path);
    std::string resolve_source_path(const std::string& path) const;
    bool compile_program(const std::string& path, std::string& error);
    std::int64_t allocate_var_ref(std::uint32_t child_id);

    lsp::JsonValue capabilities() const;
    lsp::JsonValue stack_frame_json(const debug::DebugStackFrame& frame) const;
};

}  // namespace dap
}  // namespace pscript
