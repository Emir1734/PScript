#include "lsp/json_util.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace {

int g_failed = 0;

void fail(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

std::string frame(std::string_view body) {
    return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + std::string(body);
}

std::string json_escape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c == '\\' || c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

class LspStdoutReader {
public:
    void append_bytes(const char* data, std::size_t size) { buffer_.append(data, size); }

    bool wait_for_response(int id, int timeout_ms, const std::function<void()>& pump) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            if (saw_id_.count(id) > 0) {
                return true;
            }
        }
        return false;
    }

    void parse_frames() {
        for (;;) {
            const std::size_t header_end = buffer_.find("\r\n\r\n");
            if (header_end == std::string::npos) {
                break;
            }
            const std::string_view header(buffer_.data(), header_end);
            const std::string_view prefix = "Content-Length: ";
            const std::size_t pos = header.find(prefix);
            if (pos == std::string_view::npos) {
                break;
            }
            const int content_length = std::atoi(std::string(header.substr(pos + prefix.size())).c_str());
            const std::size_t body_start = header_end + 4;
            if (buffer_.size() < body_start + static_cast<std::size_t>(content_length)) {
                break;
            }
            const std::string_view body(buffer_.data() + body_start, static_cast<std::size_t>(content_length));
            const std::optional<pscript::lsp::JsonValue> message = pscript::lsp::parse_json(body);
            if (message.has_value()) {
                const std::optional<const pscript::lsp::JsonValue*> id_value = message->get("id");
                if (id_value.has_value() && (*id_value)->kind == pscript::lsp::JsonValue::Kind::Number) {
                    saw_id_.insert(static_cast<int>((*id_value)->number_value));
                }
            }
            buffer_.erase(0, body_start + static_cast<std::size_t>(content_length));
        }
    }

private:
    std::string buffer_;
    std::unordered_set<int> saw_id_;
};

bool exercise_unicode_keystroke(const std::function<bool(std::string_view)>& write,
                                LspStdoutReader& reader, const std::function<void()>& pump_stdout,
                                const std::function<bool()>& process_alive, std::string_view ch,
                                std::string_view label, int init_id) {
    const std::string uri = "file:///tmp/pscript_stdio_unicode.phs";
    const std::string init = frame(std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(init_id) +
                                   R"(,"method":"initialize","params":{"capabilities":{}}})");
    const std::string initialized = frame(R"({"jsonrpc":"2.0","method":"initialized","params":{}})");
    const std::string did_open = frame(std::string(R"({"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":")") +
                                      uri + R"(","languageId":"pscript","version":1,"text":""}}})");
    const std::string did_change = frame(
        std::string(R"({"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":")") +
        uri + R"(","version":2},"contentChanges":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":0}},"rangeLength":0,"text":")" +
        json_escape(ch) + R"("}]}})");
    const int completion_id = init_id + 1;
    const int semantic_id = init_id + 2;
    const std::string completion = frame(
        std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(completion_id) +
        R"(,"method":"textDocument/completion","params":{"textDocument":{"uri":")" + uri +
        R"(","position":{"line":0,"character":1}}})");
    const std::string semantic = frame(
        std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(semantic_id) +
        R"(,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":")" + uri + R"("}}})");

    if (!write(init)) {
        fail(label, "failed to write initialize");
        return false;
    }
    if (!reader.wait_for_response(init_id, 3000, pump_stdout)) {
        fail(label, "missing initialize response");
        return false;
    }
    if (!write(initialized) || !write(did_open) || !write(did_change) || !write(completion) || !write(semantic)) {
        fail(label, "failed to write LSP frames");
        return false;
    }
    (void)reader.wait_for_response(completion_id, 5000, pump_stdout);
    (void)reader.wait_for_response(semantic_id, 5000, pump_stdout);
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    if (!process_alive()) {
        fail(label, "language server process exited");
        return false;
    }
    pass(label);
    return true;
}

#ifdef _WIN32

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

struct PipeProcess {
    HANDLE process = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;

    PipeProcess() = default;
    PipeProcess(const PipeProcess&) = delete;
    PipeProcess& operator=(const PipeProcess&) = delete;

    PipeProcess(PipeProcess&& other) noexcept { *this = std::move(other); }

    PipeProcess& operator=(PipeProcess&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        close_handles();
        process = other.process;
        stdin_write = other.stdin_write;
        stdout_read = other.stdout_read;
        other.process = nullptr;
        other.stdin_write = nullptr;
        other.stdout_read = nullptr;
        return *this;
    }

    ~PipeProcess() { close_handles(); }

    void close_handles() {
        if (stdin_write) {
            CloseHandle(stdin_write);
            stdin_write = nullptr;
        }
        if (stdout_read) {
            CloseHandle(stdout_read);
            stdout_read = nullptr;
        }
        if (process) {
            CloseHandle(process);
            process = nullptr;
        }
    }
};

std::optional<PipeProcess> spawn_lsp(const char* exe_path) {
    SECURITY_ATTRIBUTES attrs{};
    attrs.nLength = sizeof(attrs);
    attrs.bInheritHandle = TRUE;

    HANDLE stdin_read = nullptr;
    HANDLE stdout_write = nullptr;
    PipeProcess proc;
    if (!CreatePipe(&stdin_read, &proc.stdin_write, &attrs, 0)) {
        return std::nullopt;
    }
    if (!CreatePipe(&proc.stdout_read, &stdout_write, &attrs, 0)) {
        return std::nullopt;
    }
    SetHandleInformation(proc.stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(proc.stdout_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    PROCESS_INFORMATION info{};
    const std::wstring exe_wide = utf8_to_wide(exe_path);
    if (exe_wide.empty()) {
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        return std::nullopt;
    }
    std::wstring cwd = exe_wide;
    const std::size_t slash = cwd.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        cwd.resize(slash);
    }
    if (!CreateProcessW(exe_wide.c_str(), nullptr, nullptr, nullptr, TRUE, 0, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &startup, &info)) {
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        return std::nullopt;
    }
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(info.hThread);
    proc.process = info.hProcess;
    return proc;
}

bool write_all_handle(HANDLE handle, std::string_view data) {
    std::size_t written_total = 0;
    while (written_total < data.size()) {
        DWORD written = 0;
        if (!WriteFile(handle, data.data() + written_total,
                       static_cast<DWORD>(data.size() - written_total), &written, nullptr)) {
            return false;
        }
        written_total += written;
    }
    return true;
}

bool process_alive_handle(HANDLE process) { return WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }

void terminate_process_handle(HANDLE process) { TerminateProcess(process, 0); }

void pump_stdout_handle(HANDLE stdout_read, LspStdoutReader& reader) {
    char chunk[4096];
    DWORD available = 0;
    while (PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
        DWORD read = 0;
        if (!ReadFile(stdout_read, chunk, sizeof(chunk), &read, nullptr)) {
            return;
        }
        reader.append_bytes(chunk, read);
        reader.parse_frames();
    }
}

bool run_stdio_tests(const char* exe) {
    std::optional<PipeProcess> proc = spawn_lsp(exe);
    if (!proc.has_value()) {
        fail("lsp_server_stdio", "failed to spawn pscript_lsp");
        return false;
    }
    LspStdoutReader reader;
    const auto write = [&](std::string_view data) { return write_all_handle(proc->stdin_write, data); };
    const auto pump = [&]() { pump_stdout_handle(proc->stdout_read, reader); };
    const auto alive = [&]() { return process_alive_handle(proc->process); };
    if (!exercise_unicode_keystroke(write, reader, pump, alive, "ç", "lsp stdio didChange ç", 1)) {
        terminate_process_handle(proc->process);
        return false;
    }
    terminate_process_handle(proc->process);
    proc = std::nullopt;

    proc = spawn_lsp(exe);
    if (!proc.has_value()) {
        fail("lsp_server_stdio", "failed to spawn pscript_lsp for ö");
        return false;
    }
    LspStdoutReader reader_o;
    const auto write_o = [&](std::string_view data) { return write_all_handle(proc->stdin_write, data); };
    const auto pump_o = [&]() { pump_stdout_handle(proc->stdout_read, reader_o); };
    const auto alive_o = [&]() { return process_alive_handle(proc->process); };
    if (!exercise_unicode_keystroke(write_o, reader_o, pump_o, alive_o, "ö", "lsp stdio didChange ö", 1)) {
        terminate_process_handle(proc->process);
        return false;
    }
    terminate_process_handle(proc->process);
    return true;
}

#else

struct PipeProcess {
    pid_t pid = -1;
    int stdin_write = -1;
    int stdout_read = -1;

    ~PipeProcess() { close_fds(); }

    void close_fds() {
        if (stdin_write >= 0) {
            close(stdin_write);
            stdin_write = -1;
        }
        if (stdout_read >= 0) {
            close(stdout_read);
            stdout_read = -1;
        }
    }
};

std::optional<PipeProcess> spawn_lsp(const char* exe_path) {
    PipeProcess proc;
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
        return std::nullopt;
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return std::nullopt;
    }
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
    posix_spawn_file_actions_adddup2(&actions, stdin_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, stdin_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, stdout_pipe[1]);
    char* const argv[] = {const_cast<char*>(exe_path), nullptr};
    pid_t pid = -1;
    const int spawn_status =
        posix_spawn(&pid, exe_path, &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    if (spawn_status != 0 || pid < 0) {
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        return std::nullopt;
    }
    proc.pid = pid;
    proc.stdin_write = stdin_pipe[1];
    proc.stdout_read = stdout_pipe[0];
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) != 0) {
        return std::nullopt;
    }
    return proc;
}

bool write_all_fd(int fd, std::string_view data) {
    std::size_t written_total = 0;
    while (written_total < data.size()) {
        const ssize_t written = write(fd, data.data() + written_total, data.size() - written_total);
        if (written <= 0) {
            return false;
        }
        written_total += static_cast<std::size_t>(written);
    }
    return true;
}

bool process_alive_pid(pid_t pid) {
    const pid_t result = waitpid(pid, nullptr, WNOHANG);
    return result == 0;
}

void terminate_process_pid(pid_t pid) { kill(pid, SIGTERM); }

void pump_stdout_fd(int stdout_read, LspStdoutReader& reader) {
    char chunk[4096];
    pollfd descriptor{};
    descriptor.fd = stdout_read;
    descriptor.events = POLLIN;
    while (poll(&descriptor, 1, 0) > 0 && (descriptor.revents & POLLIN) != 0) {
        const ssize_t read_count = read(stdout_read, chunk, sizeof(chunk));
        if (read_count <= 0) {
            return;
        }
        reader.append_bytes(chunk, static_cast<std::size_t>(read_count));
        reader.parse_frames();
    }
}

bool run_stdio_tests(const char* exe) {
    std::optional<PipeProcess> proc = spawn_lsp(exe);
    if (!proc.has_value()) {
        fail("lsp_server_stdio", "failed to spawn pscript_lsp");
        return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    LspStdoutReader reader;
    const auto write = [&](std::string_view data) { return write_all_fd(proc->stdin_write, data); };
    const auto pump = [&]() { pump_stdout_fd(proc->stdout_read, reader); };
    const auto alive = [&]() { return process_alive_pid(proc->pid); };
    if (!exercise_unicode_keystroke(write, reader, pump, alive, "ç", "lsp stdio didChange ç", 1)) {
        terminate_process_pid(proc->pid);
        return false;
    }
    terminate_process_pid(proc->pid);
    proc = std::nullopt;

    proc = spawn_lsp(exe);
    if (!proc.has_value()) {
        fail("lsp_server_stdio", "failed to spawn pscript_lsp for ö");
        return false;
    }
    LspStdoutReader reader_o;
    const auto write_o = [&](std::string_view data) { return write_all_fd(proc->stdin_write, data); };
    const auto pump_o = [&]() { pump_stdout_fd(proc->stdout_read, reader_o); };
    const auto alive_o = [&]() { return process_alive_pid(proc->pid); };
    if (!exercise_unicode_keystroke(write_o, reader_o, pump_o, alive_o, "ö", "lsp stdio didChange ö", 1)) {
        terminate_process_pid(proc->pid);
        return false;
    }
    terminate_process_pid(proc->pid);
    return true;
}

#endif

}  // namespace

int main() {
#ifndef PSCRIPT_LSP_EXE
    fail("lsp_server_stdio", "PSCRIPT_LSP_EXE not defined");
    return 1;
#else
    if (!run_stdio_tests(PSCRIPT_LSP_EXE)) {
        return 1;
    }
    if (g_failed != 0) {
        return 1;
    }
    std::cout << "All lsp server process tests passed\n";
    return 0;
#endif
}
