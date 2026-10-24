#include "lsp/json_util.h"

#include <chrono>
#include <cstdlib>
#include <functional>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef PSCRIPT_DAP_EXE
#define PSCRIPT_DAP_EXE "pscript_dap"
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

class DapReader {
public:
    void append(const char* data, std::size_t size) { buffer_.append(data, size); }

    bool wait_for_event(const std::string& event_name, int timeout_ms, const std::function<void()>& pump) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            parse_frames();
            if (saw_events_.count(event_name) > 0) {
                return true;
            }
        }
        return false;
    }

    bool wait_for_response(int seq, int timeout_ms, const std::function<void()>& pump) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            pump();
            parse_frames();
            if (saw_response_seq_.count(seq) > 0) {
                return true;
            }
        }
        return false;
    }

private:
    std::string buffer_;
    std::unordered_set<std::string> saw_events_;
    std::unordered_set<int> saw_response_seq_;

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
                if (const std::optional<const pscript::lsp::JsonValue*> type = message->get("type");
                    type.has_value() && (*type)->kind == pscript::lsp::JsonValue::Kind::String) {
                    if ((*type)->string_value == "event") {
                        if (const std::optional<const pscript::lsp::JsonValue*> event = message->get("event");
                            event.has_value() && (*event)->kind == pscript::lsp::JsonValue::Kind::String) {
                            saw_events_.insert((*event)->string_value);
                        }
                    } else if ((*type)->string_value == "response") {
                        if (const std::optional<const pscript::lsp::JsonValue*> seq = message->get("request_seq");
                            seq.has_value() && (*seq)->kind == pscript::lsp::JsonValue::Kind::Number) {
                            saw_response_seq_.insert(static_cast<int>((*seq)->number_value));
                        }
                    }
                }
            }
            buffer_.erase(0, body_start + static_cast<std::size_t>(content_length));
        }
    }
};

#ifdef _WIN32

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

struct PipeProcess {
    HANDLE process = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;

    ~PipeProcess() {
        if (stdin_write) {
            CloseHandle(stdin_write);
        }
        if (stdout_read) {
            CloseHandle(stdout_read);
        }
        if (process) {
            CloseHandle(process);
        }
    }

    bool spawn(const std::wstring& command_line) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE stdout_write = nullptr;
        HANDLE stdin_read = nullptr;
        if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
            return false;
        }
        if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0)) {
            return false;
        }
        if (!CreatePipe(&stdin_read, &stdin_write, &sa, 0)) {
            return false;
        }
        if (!SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0)) {
            return false;
        }
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdin_read;
        si.hStdOutput = stdout_write;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + command_line + L"\"";
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                            &pi)) {
            return false;
        }
        CloseHandle(stdout_write);
        CloseHandle(stdin_read);
        process = pi.hProcess;
        CloseHandle(pi.hThread);
        return true;
    }

    bool write(std::string_view data) {
        DWORD written = 0;
        return WriteFile(stdin_write, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) != 0;
    }

    bool pump_stdout(DapReader& reader) {
        char buffer[4096];
        DWORD available = 0;
        if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr)) {
            return false;
        }
        if (available == 0) {
            return true;
        }
        DWORD read = 0;
        if (!ReadFile(stdout_read, buffer, sizeof(buffer), &read, nullptr)) {
            return false;
        }
        reader.append(buffer, read);
        return true;
    }

    bool alive() const {
        if (!process) {
            return false;
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(process, &code)) {
            return false;
        }
        return code == STILL_ACTIVE;
    }
};

#endif

}  // namespace

int main() {
#ifndef _WIN32
    pass("dap stdio e2e");
    pass("dap skipped on non-windows");
    return 0;
#else
    const std::wstring exe = utf8_to_wide(PSCRIPT_DAP_EXE);
    PipeProcess proc;
    if (!proc.spawn(exe)) {
        fail("dap spawn", "CreateProcess failed");
        return 1;
    }
    DapReader reader;
    const auto pump = [&] { proc.pump_stdout(reader); };

    const std::string script_path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : ".") + "\\pscript_dap_test.phs";
    {
        std::ofstream out(script_path, std::ios::binary);
        out << "x = 0\nwhile x < 2:\n    x = x + 1\n";
    }
    std::string path_json = script_path;
    for (char& ch : path_json) {
        if (ch == '\\') {
            ch = '/';
        }
    }

    const std::string init = frame(
        R"({"seq":1,"type":"request","command":"initialize","arguments":{"clientID":"test","adapterID":"pscript"}})");
    const std::string launch = frame(std::string(R"({"seq":2,"type":"request","command":"launch","arguments":{"program":")") +
                                         path_json + R"("}})");
    const std::string breakpoints = frame(
        std::string(R"({"seq":3,"type":"request","command":"setBreakpoints","arguments":{"source":{"path":")") +
        path_json + R"("},"breakpoints":[{"line":3}]}})");
    const std::string config_done = frame(R"({"seq":4,"type":"request","command":"configurationDone","arguments":{}})");
    const std::string disconnect = frame(R"({"seq":5,"type":"request","command":"disconnect","arguments":{}})");

    if (!proc.write(init)) {
        fail("dap initialize", "write failed");
        return 1;
    }
    if (!reader.wait_for_response(1, 5000, pump)) {
        fail("dap initialize", "no response");
        return 1;
    }
    pass("dap initialize");

    proc.write(launch);
    if (!reader.wait_for_response(2, 5000, pump)) {
        fail("dap launch", "no response");
        return 1;
    }
    pass("dap launch");

    proc.write(breakpoints);
    if (!reader.wait_for_response(3, 5000, pump)) {
        fail("dap setBreakpoints", "no response");
        return 1;
    }
    pass("dap setBreakpoints");

    proc.write(config_done);
    if (!reader.wait_for_response(4, 5000, pump)) {
        fail("dap configurationDone", "no response");
        return 1;
    }
    pass("dap configurationDone");

    if (!reader.wait_for_event("stopped", 8000, pump)) {
        fail("dap stopped event", "breakpoint did not pause");
        return 1;
    }
    pass("dap stopped event");

    proc.write(disconnect);
    (void)reader.wait_for_response(5, 3000, pump);
    pass("dap disconnect");

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All DAP stdio tests passed\n";
    return 0;
#endif
}
