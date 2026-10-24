#include "stdlib/native_common.h"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pscript {
namespace {

constexpr std::uint32_t kRunResultClassIndex = 0;

[[noreturn]] void process_fail(std::string_view operation, const std::string& detail) {
    native_runtime_error(std::string(operation) + " failed: " + detail);
}

Value make_run_result(int exit_code, std::string stdout_text, std::string stderr_text) {
    static const std::vector<std::string> field_names = {"exit_code", "stdout", "stderr"};
    Value value = Runtime::make_class(kRunResultClassIndex, "RunResult", field_names);
    value.class_value->fields[0] = Runtime::make_integer(static_cast<std::int64_t>(exit_code));
    value.class_value->fields[1] = Runtime::make_string(std::move(stdout_text));
    value.class_value->fields[2] = Runtime::make_string(std::move(stderr_text));
    return value;
}

#ifdef _WIN32

std::string read_handle_all(HANDLE handle) {
    std::string out;
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0) {
            break;
        }
        out.append(buffer.data(), read);
    }
    return out;
}

struct RunOutput {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

RunOutput run_command(const std::string& command) {
    RunOutput result;
    SECURITY_ATTRIBUTES attrs{};
    attrs.nLength = sizeof(attrs);
    attrs.bInheritHandle = TRUE;

    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &attrs, 0) || !CreatePipe(&stderr_read, &stderr_write, &attrs, 0)) {
        process_fail("run", "unable to create output pipes");
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process{};
    std::string cmdline = "cmd.exe /c " + command;
    std::vector<char> cmdline_buffer(cmdline.begin(), cmdline.end());
    cmdline_buffer.push_back('\0');
    if (!CreateProcessA(nullptr, cmdline_buffer.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &process)) {
        CloseHandle(stdout_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_read);
        CloseHandle(stderr_write);
        process_fail("run", "unable to start process");
    }

    CloseHandle(stdout_write);
    CloseHandle(stderr_write);

    result.stdout_text = read_handle_all(stdout_read);
    result.stderr_text = read_handle_all(stderr_read);
    CloseHandle(stdout_read);
    CloseHandle(stderr_read);

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    result.exit_code = static_cast<int>(exit_code);
    return result;
}

std::int64_t current_process_id() {
    return static_cast<std::int64_t>(GetCurrentProcessId());
}

#else

std::string read_pipe_all(int fd) {
    std::string out;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = read(fd, buffer.data(), buffer.size());
        if (count <= 0) {
            break;
        }
        out.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return out;
}

struct RunOutput {
    int exit_code = -1;
    std::string stdout_text;
    std::string stderr_text;
};

RunOutput run_command(const std::string& command) {
    RunOutput result;
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        process_fail("run", "unable to create output pipes");
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        process_fail("run", "unable to fork process");
    }
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    result.stdout_text = read_pipe_all(stdout_pipe[0]);
    result.stderr_text = read_pipe_all(stderr_pipe[0]);
    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        process_fail("run", "unable to wait for process");
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = -1;
    }
    return result;
}

std::int64_t current_process_id() {
    return static_cast<std::int64_t>(getpid());
}

#endif

}  // namespace

Value native_process_id(const std::vector<Value>& args) {
    native_require_argc("id", args, 0);
    return Runtime::make_integer(current_process_id());
}

Value native_process_exit(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("exit", args, 1);
    if (args[0].kind != ValueKind::Integer) {
        native_type_error("'exit' expects an integer exit code");
    }
    runtime.request_process_exit(static_cast<int>(args[0].integer));
}

Value native_process_run(const std::vector<Value>& args) {
    native_require_argc("run", args, 1);
    const std::string& command = native_require_string(args[0], "run", "command");
    if (command.empty()) {
        native_value_error("'run' expects a non-empty command");
    }
    const RunOutput output = run_command(command);
    return make_run_result(output.exit_code, output.stdout_text, output.stderr_text);
}

}  // namespace pscript
