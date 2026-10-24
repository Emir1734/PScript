#include "cli/cli.h"
#include "cli/pipeline.h"
#include "package/install.h"
#include "package/manifest.h"
#include "package/package_identity.h"
#include "package/project.h"
#include "package/registry.h"
#include "package/semver.h"

#include <iostream>
#include <filesystem>
#include <sstream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <cstdlib>

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
#include <cerrno>
#include <cstring>
#endif

#ifndef PSCRIPT_EXECUTABLE
#error PSCRIPT_EXECUTABLE must be defined
#endif

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

struct CommandResult {
    int exit_code = -1;
    std::string output;
};

struct RunOptions {
    std::optional<std::filesystem::path> working_directory;
    std::vector<std::pair<std::string, std::string>> env;
};

#ifdef _WIN32

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) {
        return std::wstring();
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        return std::wstring();
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), needed);
    return wide;
}

std::string quote_windows(const std::string& arg) {
    std::string quoted = "\"";
    for (char c : arg) {
        if (c == '"') {
            quoted += "\\\"";
        } else {
            quoted += c;
        }
    }
    quoted += '"';
    return quoted;
}

std::vector<wchar_t> merge_environment_block(const std::vector<std::pair<std::string, std::string>>& overrides) {
    std::wstring merged;
    wchar_t* system_env = GetEnvironmentStringsW();
    if (system_env != nullptr) {
        for (const wchar_t* entry = system_env; *entry != L'\0';) {
            const std::wstring line(entry);
            entry += line.size() + 1;
            bool replaced = false;
            for (const auto& [key, value] : overrides) {
                const std::wstring prefix = utf8_to_wide(key) + L"=";
                if (line.rfind(prefix, 0) == 0) {
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                merged.append(line);
                merged.push_back(L'\0');
            }
        }
        FreeEnvironmentStringsW(system_env);
    }
    for (const auto& [key, value] : overrides) {
        merged.append(utf8_to_wide(key + "=" + value));
        merged.push_back(L'\0');
    }
    merged.push_back(L'\0');
    return std::vector<wchar_t>(merged.begin(), merged.end());
}

CommandResult run_command(const std::vector<std::string>& args, const RunOptions& options = {}) {
    CommandResult result;
    if (args.empty()) {
        return result;
    }

    std::string command_line;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            command_line += ' ';
        }
        command_line += quote_windows(args[i]);
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &security, 0)) {
        result.output = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;

    PROCESS_INFORMATION process{};
    const std::wstring wide_app = utf8_to_wide(args[0]);
    std::wstring wide_cmd = utf8_to_wide(command_line);
    std::vector<wchar_t> mutable_cmd(wide_cmd.begin(), wide_cmd.end());
    mutable_cmd.push_back(L'\0');
    std::wstring wide_cwd;
    const wchar_t* cwd_ptr = nullptr;
    if (options.working_directory.has_value()) {
        wide_cwd = utf8_to_wide(options.working_directory->string());
        cwd_ptr = wide_cwd.c_str();
    }
    std::vector<wchar_t> environment_block;
    void* environment_ptr = nullptr;
    DWORD creation_flags = 0;
    if (!options.env.empty()) {
        environment_block = merge_environment_block(options.env);
        environment_ptr = environment_block.data();
        creation_flags = CREATE_UNICODE_ENVIRONMENT;
    }
    const BOOL created =
        CreateProcessW(wide_app.c_str(), mutable_cmd.data(), nullptr, nullptr, TRUE, creation_flags, environment_ptr,
                       cwd_ptr, &startup, &process);
    CloseHandle(write_pipe);

    if (!created) {
        CloseHandle(read_pipe);
        result.output = "CreateProcessW failed";
        return result;
    }

    std::string output;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        output.append(buffer, buffer + read);
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(read_pipe);

    result.exit_code = static_cast<int>(exit_code);
    result.output = std::move(output);
    return result;
}

#else

CommandResult run_command(const std::vector<std::string>& args, const RunOptions& options = {}) {
    CommandResult result;
    if (args.empty()) {
        return result;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        result.output = std::strerror(errno);
        return result;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        result.output = std::strerror(errno);
        close(fds[0]);
        close(fds[1]);
        return result;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        if (options.working_directory.has_value()) {
            std::filesystem::current_path(*options.working_directory);
        }
        for (const auto& [key, value] : options.env) {
            setenv(key.c_str(), value.c_str(), 1);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);
        execv(args[0].c_str(), argv.data());
        _exit(127);
    }

    close(fds[1]);
    std::string output;
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(fds[0], buffer, sizeof(buffer))) > 0) {
        output.append(buffer, buffer + n);
    }
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else {
        result.exit_code = 1;
    }
    result.output = std::move(output);
    return result;
}

#endif

std::string pscript_exe() {
    return PSCRIPT_EXECUTABLE;
}

class TempDir {
public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() / "pscript_cli_tests";
        std::filesystem::create_directories(path_);
        path_ /= std::filesystem::path("run-" + std::to_string(std::hash<std::string>{}(path_.string())));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::filesystem::path write_file(const std::string& name, std::string_view contents) const {
        const auto file = path_ / name;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << contents;
        return file;
    }

private:
    std::filesystem::path path_;
};

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

std::string normalize_newlines(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            out.push_back('\n');
            ++i;
            continue;
        }
        out.push_back(text[i]);
    }
    return out;
}

}  // namespace

int main() {
    const std::string exe = pscript_exe();
    TempDir temp;

    {
        const auto file = temp.write_file("add.phs", "x = 10\ny = 20\nprint(x + y)\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "30\n") {
            fail_test("valid program", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("valid program");
        }
    }

    {
        const auto file = temp.write_file("hello.phs", "print(\"Hello\")\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "Hello\n") {
            fail_test("string print", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("string print");
        }
    }

    {
        const std::string missing = (temp.write_file("keep.txt", "")).parent_path().string() + "/missing.phs";
        const CommandResult result = run_command({exe, missing});
        if (result.exit_code == 0 || !contains(result.output, "cannot open file")) {
            fail_test("missing file", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("missing file");
        }
    }

    {
        const auto file = temp.write_file("syntax.phs", "x =\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code == 0 || !contains(result.output, "Compile error:") || !contains(result.output, "line:") ||
            !contains(result.output, "column:")) {
            fail_test("syntax error", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("syntax error");
        }
    }

    {
        const auto file = temp.write_file("divzero.phs", "print(1 / 0)\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 3 || !contains(result.output, "Unhandled exception:") ||
            !contains(result.output, "division by zero")) {
            fail_test("runtime error", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("runtime error");
        }
    }

    {
        const auto file = temp.write_file("ok.phs", "print(1)\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0) {
            fail_test("success exit code", "exit=" + std::to_string(result.exit_code));
        } else {
            pass_test("success exit code");
        }
    }

    {
        const CommandResult result = run_command({exe});
        if (result.exit_code != 2 || !contains(result.output, "Usage: pscript")) {
            fail_test("error exit code", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("error exit code");
        }
    }

    {
        const auto file = temp.write_file("typed.phs",
                                         "int x = 10\n"
                                         "float score = 92.5\n"
                                         "string name = \"Emir\"\n"
                                         "bool active = true\n"
                                         "\n"
                                         "print(\"Name:\", name, \"Score:\", score, \"Active:\", active)\n"
                                         "print(\"Sum:\", x + 20)\n");
        const CommandResult result = run_command({exe, file.string()});
        const std::string expected = "Name: Emir Score: 92.5 Active: true\nSum: 30\n";
        if (result.exit_code != 0 || normalize_newlines(result.output) != expected) {
            fail_test("typed program", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("typed program");
        }
    }

    {
        const auto file = temp.write_file("type_error.phs", "int x = \"hello\"\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code == 0 || !contains(result.output, "Type error: expected int, got string") ||
            !contains(result.output, "line:")) {
            fail_test("type error", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("type error");
        }
    }

    {
        const auto file = temp.write_file("if.phs", "x = 15\n\nif x > 10:\n    print(\"big\")\nelse:\n    print(\"small\")\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "big\n") {
            fail_test("if program", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("if program");
        }
    }

    {
        const auto file = temp.write_file("loops.phs",
                                         "for i in range(10):\n"
                                         "    if i == 3:\n"
                                         "        continue\n"
                                         "    if i == 7:\n"
                                         "        break\n"
                                         "    print(i)\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "0\n1\n2\n4\n5\n6\n") {
            fail_test("loops program",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("loops program");
        }
    }

    {
        const auto file = temp.write_file("unicode.phs",
                                         "print(\"Merhaba D\xC3\xBCnya\")\n"
                                         "print(\"\xC3\xA7 \xC4\x9F \xC4\xB1 \xC4\xB0 \xC3\xB6 \xC5\x9F \xC3\xBC\")\n"
                                         "print(\"\xF0\x9F\x98\x80\")\n"
                                         "print(\"\xF0\x9F\x9A\x80\")\n"
                                         "print(\"\xE2\x9D\xA4\xEF\xB8\x8F\")\n"
                                         "print(\"T\xC3\xBCrk\xC3\xA7" "e: \xC3\xA7\xC4\x9F\xC4\xB1\xC5\x9F\xC4\xB0\xC3\xB6\xC3\xBC\")\n"
                                         "print(\"Emoji: \xF0\x9F\x98\x80 \xF0\x9F\x9A\x80 \xF0\x9F\x94\xA5\")\n"
                                         "print(\"Mixed: PScript \xC3\xA7" "al\xC4\xB1\xC5\x9F\xC4\xB1yor \xF0\x9F\x9A\x80\")\n"
                                         "name = \"Emir Y\xC4\xB1ld\xC4\xB1z\"\n"
                                         "print(name)\n");
        const CommandResult result = run_command({exe, file.string()});
        const std::string expected =
            "Merhaba D\xC3\xBCnya\n"
            "\xC3\xA7 \xC4\x9F \xC4\xB1 \xC4\xB0 \xC3\xB6 \xC5\x9F \xC3\xBC\n"
            "\xF0\x9F\x98\x80\n"
            "\xF0\x9F\x9A\x80\n"
            "\xE2\x9D\xA4\xEF\xB8\x8F\n"
            "T\xC3\xBCrk\xC3\xA7" "e: \xC3\xA7\xC4\x9F\xC4\xB1\xC5\x9F\xC4\xB0\xC3\xB6\xC3\xBC\n"
            "Emoji: \xF0\x9F\x98\x80 \xF0\x9F\x9A\x80 \xF0\x9F\x94\xA5\n"
            "Mixed: PScript \xC3\xA7" "al\xC4\xB1\xC5\x9F\xC4\xB1yor \xF0\x9F\x9A\x80\n"
            "Emir Y\xC4\xB1ld\xC4\xB1z\n";
        if (result.exit_code != 0 || normalize_newlines(result.output) != expected) {
            fail_test("utf-8 program",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("utf-8 program");
        }
    }

    {
        const std::string bom_source = "\xEF\xBB\xBFprint(\"\xC3\xA7\")\n";
        const auto file = temp.write_file("bom.phs", bom_source);
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "\xC3\xA7\n") {
            fail_test("utf-8 bom program",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("utf-8 bom program");
        }
    }

    {
        temp.write_file("math.phs", "func add(a, b):\n    return a + b\n");
        const auto file = temp.write_file("main_import.phs", "import math\nprint(math.add(1, 2))\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "3\n") {
            fail_test("module import via cli",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("module import via cli");
        }
    }

    {
        const auto file = temp.write_file("print_expr.phs",
                                         "x = 10\n"
                                         "y = 4\n"
                                         "print (x * y) / 2\n"
                                         "print((x * y) / 2)\n"
                                         "print x * y\n"
                                         "print x + y * 2\n"
                                         "print (x + y) * 2\n"
                                         "print x + 10, y * 2, (x + y) / 2\n");
        const CommandResult result = run_command({exe, file.string()});
        const std::string expected = "20\n20\n40\n18\n28\n20 8 7\n";
        if (result.exit_code != 0 || normalize_newlines(result.output) != expected) {
            fail_test("print expressions",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("print expressions");
        }
    }

    {
        const auto file = temp.write_file("async_main.phs", "async func main():\n    await delay(0ms)\n    print(42)\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "42\n") {
            fail_test("async main cli", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("async main cli");
        }
    }

    {
        const auto file = temp.write_file("async_fail.phs",
                                          "async func fail():\n    throw \"boom\"\nasync func main():\n    await fail()\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 3 || !contains(result.output, "Unhandled exception:") ||
            !contains(result.output, "boom")) {
            fail_test("async unhandled cli", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("async unhandled cli");
        }
    }

    {
        const auto file = temp.write_file("async_bg_fail.phs",
                                          "async func fail():\n    throw \"bg\"\nasync func main():\n    fail()\n    "
                                          "print(\"done\")\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 3 || !contains(result.output, "done") ||
            !contains(result.output, "Unhandled exception:") || !contains(result.output, "bg")) {
            fail_test("async background failure cli",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("async background failure cli");
        }
    }

    {
        const auto file = temp.write_file("sync_regression.phs", "print(99)\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "99\n") {
            fail_test("sync program regression cli",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("sync program regression cli");
        }
    }

    {
        const auto file = temp.write_file("sys_args.phs", "import sys\nprint(sys.args)\n");
        const CommandResult result = run_command({exe, file.string(), "hello", "world"});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "[hello, world]\n") {
            fail_test("sys args cli",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("sys args cli");
        }
    }

    {
        const auto file = temp.write_file("sys_version.phs", "import sys\nprint(sys.version)\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "1.0.0\n") {
            fail_test("sys version cli",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("sys version cli");
        }
    }

    {
        const auto file = temp.write_file("process_exit.phs", "import process\nprocess.exit(42)\nprint(\"skip\")\n");
        const CommandResult result = run_command({exe, file.string()});
        if (result.exit_code != 42 || contains(result.output, "skip")) {
            fail_test("process exit cli",
                      "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("process exit cli");
        }
    }

    {
        const CommandResult result = run_command({exe, "--help"});
        if (result.exit_code != 0 || !contains(result.output, "Commands:") || !contains(result.output, "repl") ||
            !contains(result.output, "test") || !contains(result.output, "doctor") ||
            !contains(result.output, "explain") || !contains(result.output, "pack") ||
            !contains(result.output, "lib new")) {
            fail_test("global help", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("global help");
        }
    }

    {
        const CommandResult result = run_command({exe, "--version"});
        if (result.exit_code != 0 || !contains(result.output, "1.0.0")) {
            fail_test("global version", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("global version");
        }
    }

    {
        const CommandResult result = run_command({exe, "--not-a-real-flag"});
        if (result.exit_code != 2 || !contains(result.output, "unknown option")) {
            fail_test("unknown option", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("unknown option");
        }
    }

    {
        const CommandResult result = run_command({exe, "run"});
        if (result.exit_code != 2 || !contains(result.output, "run requires")) {
            fail_test("run missing file", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("run missing file");
        }
    }

    {
        const auto file = temp.write_file("ok_check.phs", "print(1)\n");
        const CommandResult result = run_command({exe, "check", file.string()});
        if (result.exit_code != 0) {
            fail_test("check valid", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("check valid");
        }
    }

    {
        const auto file = temp.write_file("bad_check.phs", "x =\n");
        const CommandResult result = run_command({exe, "check", file.string()});
        if (result.exit_code != 1 || !contains(result.output, "error [parser]")) {
            fail_test("check syntax", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("check syntax");
        }
    }

    {
        const auto file = temp.write_file("runtime_check.phs", "print(1/0)\n");
        const CommandResult run_result = run_command({exe, file.string()});
        const CommandResult check_result = run_command({exe, "check", file.string()});
        if (check_result.exit_code != 0 || run_result.exit_code != 3) {
            fail_test("check does not run",
                      "check=" + std::to_string(check_result.exit_code) + " run=" + std::to_string(run_result.exit_code));
        } else {
            pass_test("check does not run");
        }
    }

    {
        const auto file = temp.write_file("run_sub.phs", "print(7)\n");
        const CommandResult result = run_command({exe, "run", file.string()});
        if (result.exit_code != 0 || normalize_newlines(result.output) != "7\n") {
            fail_test("run subcommand", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("run subcommand");
        }
    }

    {
        const auto temp_root = temp.write_file("keep.txt", "").parent_path();
        const std::filesystem::path project_dir = temp_root / "pscript_init_test";
        const CommandResult result = run_command({exe, "init", project_dir.string()});
        const std::filesystem::path manifest_path = project_dir / "pscript.toml";
        const std::filesystem::path main_path = project_dir / "src" / "main.phs";
        if (result.exit_code != 0 || !std::filesystem::exists(manifest_path) || !std::filesystem::exists(main_path)) {
            fail_test("init project", "exit=" + std::to_string(result.exit_code));
        } else {
            pass_test("init project");
        }
    }

    {
        const auto temp_root = temp.write_file("registry_e2e.txt", "").parent_path();
        const std::filesystem::path greet_src = temp_root / "greet_pkg_src";
        std::string pkg_error;
        if (!pscript::package::scaffold_project(greet_src, "greet_lib", &pkg_error)) {
            fail_test("registry install run e2e", pkg_error);
        } else {
            std::ofstream mod(greet_src / "src" / "greet_lib.phs", std::ios::binary | std::ios::trunc);
            mod << "pub func greet():\n    return \"hello-registry\"\n";
            mod.close();
            const std::filesystem::path registry_root = temp_root / "local_registry";
            std::filesystem::create_directories(registry_root);
            const std::filesystem::path greet_version_dir =
                pscript::package::registry_package_root(registry_root, "greet_lib") / "1.0.0";
            std::error_code remove_ec;
            std::filesystem::remove_all(greet_version_dir, remove_ec);
            if (!pscript::package::registry_publish_package(registry_root.string(), "greet_lib", pscript::package::SemVer{1, 0, 0},
                                                              greet_src.string(), "", &pkg_error)) {
                fail_test("registry install run e2e", pkg_error);
            } else {
                const std::filesystem::path version_dir =
                    pscript::package::registry_package_root(registry_root, "greet_lib") / "1.0.0";
                std::filesystem::remove_all(version_dir / "source");
                const std::string registry_abs =
                    std::filesystem::absolute(registry_root).lexically_normal().string();
#ifdef _WIN32
                SetEnvironmentVariableA("PSCRIPT_REGISTRY", registry_abs.c_str());
                _putenv_s("PSCRIPT_REGISTRY", registry_abs.c_str());
#else
                setenv("PSCRIPT_REGISTRY", registry_abs.c_str(), 1);
#endif
                const std::filesystem::path app_dir = temp_root / "registry_app";
                if (run_command({exe, "init", app_dir.string()}).exit_code != 0) {
                    fail_test("registry install run e2e", "init failed");
                } else {
                    RunOptions in_app;
                    in_app.working_directory = app_dir;
                    const CommandResult add = run_command({exe, "add", "greet_lib", "^1.0.0"}, in_app);
                    const CommandResult install = run_command({exe, "install"}, in_app);
                    const std::filesystem::path main_path = app_dir / "src" / "main.phs";
                    std::ofstream main_out(main_path, std::ios::binary | std::ios::trunc);
                    main_out << "import greet_lib\nprint(greet_lib.greet())\n";
                    main_out.close();
                    const CommandResult run = run_command({exe, main_path.string()}, in_app);
                    const std::filesystem::path deps_dir = app_dir / ".pscript" / "deps";
                    std::string deps_listing;
                    if (std::filesystem::exists(deps_dir)) {
                        for (const auto& entry : std::filesystem::directory_iterator(deps_dir)) {
                            deps_listing += entry.path().filename().string();
                            deps_listing += ';';
                        }
                    }
                    const std::filesystem::path installed_module =
                        app_dir / ".pscript" / "deps" / "greet_lib-1.0.0" / "src" / "greet_lib.phs";
                    if (add.exit_code != 0 || install.exit_code != 0 || run.exit_code != 0 ||
                        !std::filesystem::exists(installed_module) ||
                        normalize_newlines(run.output) != "hello-registry\n") {
                        fail_test("registry install run e2e",
                                  "add=" + std::to_string(add.exit_code) + " install=" +
                                      std::to_string(install.exit_code) + " run=" + std::to_string(run.exit_code) +
                                      " deps=" + deps_listing + " install_out=" + install.output +
                                      " run_out=" + run.output);
                    } else {
                        pass_test("registry install run e2e");
                    }
                }
#ifdef _WIN32
                SetEnvironmentVariableA("PSCRIPT_REGISTRY", nullptr);
#else
                unsetenv("PSCRIPT_REGISTRY");
#endif
            }
        }
    }

    {
        const auto temp_root = temp.write_file("keep2.txt", "").parent_path();
        const std::filesystem::path project_dir = temp_root / "pscript_add_test";
        if (run_command({exe, "init", project_dir.string()}).exit_code != 0) {
            fail_test("add dependency", "init failed");
        } else {
            const std::filesystem::path previous = std::filesystem::current_path();
            std::filesystem::current_path(project_dir);
            std::ostringstream out;
            std::ostringstream err;
            const pscript::CliRunResult add_result =
                pscript::run_cli_with_args({"add", "example_dep"}, out, err);
            std::filesystem::current_path(previous);
            std::ifstream manifest(project_dir / "pscript.toml");
            const std::string manifest_text((std::istreambuf_iterator<char>(manifest)),
                                            std::istreambuf_iterator<char>());
            if (add_result.exit_code != 0 || manifest_text.find("example_dep") == std::string::npos) {
                fail_test("add dependency",
                          "exit=" + std::to_string(add_result.exit_code) + " err=" + err.str());
            } else {
                pass_test("add dependency");
            }
        }
    }

    {
        const auto file = temp.write_file("fmt_me.phs", "func f():\n  print(1)\n");
        const CommandResult result = run_command({exe, "fmt", file.string()});
        if (result.exit_code != 0 || !contains(result.output, "print(1)")) {
            fail_test("fmt command", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("fmt command");
        }
    }

    {
        const auto file = temp.write_file("lint_me.phs", "func f():\n    x = 1\n    return 1\n");
        const CommandResult result = run_command({exe, "lint", file.string()});
        if (!contains(result.output, "unused")) {
            fail_test("lint command", "exit=" + std::to_string(result.exit_code) + " output=\"" + result.output + "\"");
        } else {
            pass_test("lint command");
        }
    }

    {
        const auto temp_root = temp.write_file("workspace_cmds.txt", "").parent_path();
        const std::filesystem::path project_dir = temp_root / "pscript_test_cmd";
        if (run_command({exe, "init", project_dir.string()}).exit_code != 0) {
            fail_test("test command success", "init failed");
        } else {
            std::filesystem::create_directories(project_dir / "tests");
            std::ofstream test_file(project_dir / "tests" / "smoke.phs", std::ios::binary | std::ios::trunc);
            test_file << "import testing\n\nfunc test_smoke():\n    testing.equal(1, 1)\n";
            test_file.close();
            RunOptions in_project;
            in_project.working_directory = project_dir;
            const CommandResult pass = run_command({exe, "test"}, in_project);
            if (pass.exit_code != 0 || !contains(pass.output, "all passed") || !contains(pass.output, "test_smoke")) {
                fail_test("test command success",
                          "exit=" + std::to_string(pass.exit_code) + " output=\"" + pass.output + "\"");
            } else {
                pass_test("test command success");
            }

            std::ofstream fail_file(project_dir / "tests" / "fail.phs", std::ios::binary | std::ios::trunc);
            fail_file << "import testing\n\nfunc test_bad():\n    testing.fail(\"nope\")\n";
            fail_file.close();
            const CommandResult fail = run_command({exe, "test"}, in_project);
            if (fail.exit_code != 3) {
                fail_test("test command failure", "exit=" + std::to_string(fail.exit_code) + " output=\"" + fail.output + "\"");
            } else {
                pass_test("test command failure");
            }

            std::filesystem::remove_all(project_dir / "tests");
            std::filesystem::create_directories(project_dir / "tests");
            const CommandResult empty = run_command({exe, "test"}, in_project);
            if (empty.exit_code != 2 || !contains(empty.output, "No test files")) {
                fail_test("test command no files",
                          "exit=" + std::to_string(empty.exit_code) + " output=\"" + empty.output + "\"");
            } else {
                pass_test("test command no files");
            }
        }
    }

    {
        const auto temp_root = temp.write_file("doctor_cmds.txt", "").parent_path();
        RunOptions outside;
        outside.working_directory = temp_root;
        const CommandResult no_project = run_command({exe, "doctor"}, outside);
        if (no_project.exit_code != 0 || !contains(no_project.output, "WARN") ||
            !contains(no_project.output, "Project")) {
            fail_test("doctor outside project",
                      "exit=" + std::to_string(no_project.exit_code) + " output=\"" + no_project.output + "\"");
        } else {
            pass_test("doctor outside project");
        }

        const std::filesystem::path project_dir = temp_root / "doctor_project";
        if (run_command({exe, "init", project_dir.string()}).exit_code != 0) {
            fail_test("doctor valid project", "init failed");
        } else {
            RunOptions in_project;
            in_project.working_directory = project_dir;
            const CommandResult ok = run_command({exe, "doctor"}, in_project);
            if (ok.exit_code != 0 || !contains(ok.output, "OK") || !contains(ok.output, "no issues")) {
                fail_test("doctor valid project",
                          "exit=" + std::to_string(ok.exit_code) + " output=\"" + ok.output + "\"");
            } else {
                pass_test("doctor valid project");
            }

            std::ofstream bad_manifest(project_dir / "pscript.toml", std::ios::binary | std::ios::trunc);
            bad_manifest << "not valid toml [[[\n";
            bad_manifest.close();
            const CommandResult bad = run_command({exe, "doctor"}, in_project);
            if (bad.exit_code != 3 || !contains(bad.output, "ERROR") || !contains(bad.output, "Manifest")) {
                fail_test("doctor invalid manifest",
                          "exit=" + std::to_string(bad.exit_code) + " output=\"" + bad.output + "\"");
            } else {
                pass_test("doctor invalid manifest");
            }
        }
    }

    {
        const CommandResult known = run_command({exe, "explain", "parser"});
        if (known.exit_code != 0 || !contains(known.output, "Parser error")) {
            fail_test("explain known code",
                      "exit=" + std::to_string(known.exit_code) + " output=\"" + known.output + "\"");
        } else {
            pass_test("explain known code");
        }
        const CommandResult unknown = run_command({exe, "explain", "not-a-code"});
        if (unknown.exit_code != 2 || !contains(unknown.output, "unknown diagnostic code")) {
            fail_test("explain unknown code",
                      "exit=" + std::to_string(unknown.exit_code) + " output=\"" + unknown.output + "\"");
        } else {
            pass_test("explain unknown code");
        }
        const CommandResult missing = run_command({exe, "explain"});
        if (missing.exit_code != 2 || !contains(missing.output, "Usage: pscript explain")) {
            fail_test("explain missing code",
                      "exit=" + std::to_string(missing.exit_code) + " output=\"" + missing.output + "\"");
        } else {
            pass_test("explain missing code");
        }
    }

    {
        const auto temp_root = temp.write_file("phase2_pack.txt", "").parent_path();
        const std::filesystem::path lib_dir = temp_root / "phase2_lib";
        const CommandResult lib_new = run_command({exe, "lib", "new", "phase2_lib", lib_dir.string()});
        const std::filesystem::path module_path = lib_dir / "src" / "phase2_lib.phs";
        if (lib_new.exit_code != 0 || !std::filesystem::exists(module_path)) {
            fail_test("lib new", "exit=" + std::to_string(lib_new.exit_code) + " output=\"" + lib_new.output + "\"");
        } else {
            pass_test("lib new");
        }

        RunOptions in_lib;
        in_lib.working_directory = lib_dir;
        const CommandResult pack = run_command({exe, "pack"}, in_lib);
        const std::filesystem::path archive = lib_dir / "dist" / "phase2_lib-0.1.0" / "package.tar.gz";
        if (pack.exit_code != 0 || !std::filesystem::exists(archive) || !contains(pack.output, "package.tar.gz")) {
            fail_test("pack command", "exit=" + std::to_string(pack.exit_code) + " output=\"" + pack.output + "\"");
        } else {
            pass_test("pack command");
        }
        const CommandResult pack_again = run_command({exe, "pack"}, in_lib);
        if (pack_again.exit_code != 1 || !contains(pack_again.output, "already exists")) {
            fail_test("pack overwrite protection",
                      "exit=" + std::to_string(pack_again.exit_code) + " output=\"" + pack_again.output + "\"");
        } else {
            pass_test("pack overwrite protection");
        }
        const CommandResult dry = run_command({exe, "publish", "--dry-run"}, in_lib);
        if (dry.exit_code != 0 || !contains(dry.output, "Dry run only") ||
            !contains(dry.output, "no registry upload")) {
            fail_test("publish dry-run",
                      "exit=" + std::to_string(dry.exit_code) + " output=\"" + dry.output + "\"");
        } else {
            pass_test("publish dry-run");
        }
        const CommandResult meta = run_command({exe, "metadata"}, in_lib);
        if (meta.exit_code != 0 || !contains(meta.output, "phase2_lib") || contains(meta.output, "PSCRIPT_PUBLISH")) {
            fail_test("metadata command", "exit=" + std::to_string(meta.exit_code) + " output=\"" + meta.output + "\"");
        } else {
            pass_test("metadata command");
        }
        const CommandResult list_cmd = run_command({exe, "list"}, in_lib);
        if (list_cmd.exit_code != 0 || !contains(list_cmd.output, "No pscript.lock")) {
            fail_test("list without lockfile",
                      "exit=" + std::to_string(list_cmd.exit_code) + " output=\"" + list_cmd.output + "\"");
        } else {
            pass_test("list without lockfile");
        }
        const CommandResult tree_cmd = run_command({exe, "tree"}, in_lib);
        if (tree_cmd.exit_code != 0 || !contains(tree_cmd.output, "no dependencies")) {
            fail_test("tree empty", "exit=" + std::to_string(tree_cmd.exit_code) + " output=\"" + tree_cmd.output + "\"");
        } else {
            pass_test("tree empty");
        }

        const std::filesystem::path bad_dir = temp_root / "phase2_bad_pack";
        std::filesystem::create_directories(bad_dir);
        std::ofstream bad_manifest(bad_dir / "pscript.toml", std::ios::binary | std::ios::trunc);
        bad_manifest << "name = \"bad_pkg\"\nversion = \"not-semver\"\n";
        bad_manifest.close();
        std::filesystem::create_directories(bad_dir / "src");
        RunOptions in_bad;
        in_bad.working_directory = bad_dir;
        const CommandResult bad_pack = run_command({exe, "pack"}, in_bad);
        if (bad_pack.exit_code != 1) {
            fail_test("pack invalid manifest", "exit=" + std::to_string(bad_pack.exit_code));
        } else {
            pass_test("pack invalid manifest");
        }
    }

    {
        const auto temp_root = temp.write_file("phase3_deps.txt", "").parent_path();
        const std::filesystem::path fixture = temp_root / "phase3_greet_src";
        std::string pkg_error;
        if (!pscript::package::scaffold_project(fixture, "phase3_greet", &pkg_error)) {
            fail_test("phase3 dependency commands", pkg_error);
        } else {
            std::ofstream mod(fixture / "src" / "phase3_greet.phs", std::ios::binary | std::ios::trunc);
            mod << "pub func greet():\n    return \"hi\"\n";
            mod.close();
            const std::filesystem::path app_dir = temp_root / "phase3_app";
            if (run_command({exe, "init", app_dir.string()}).exit_code != 0) {
                fail_test("phase3 dependency commands", "init failed");
            } else {
                RunOptions in_app;
                in_app.working_directory = app_dir;
                    pscript::package::ProjectManifest manifest;
                    std::string error;
                    const pscript::package::ProjectPaths paths =
                        pscript::package::project_paths_from_root(app_dir);
                    const auto loaded = pscript::package::load_manifest_file(paths.manifest.string(), &error);
                    if (!loaded) {
                        fail_test("phase3 dependency commands", error);
                    } else {
                        manifest = *loaded;
                        const std::string fixture_path =
                            std::filesystem::absolute(fixture).lexically_normal().string();
                        pscript::package::DependencySpec greet_dep;
                        greet_dep.name = "phase3_greet";
                        greet_dep.source = pscript::package::DependencySourceKind::Path;
                        greet_dep.path = fixture_path;
                        greet_dep.constraint.raw = "1.0.0";
                        manifest.dependencies["phase3_greet"] = greet_dep;
                        pscript::package::DependencySpec keep_dep;
                        keep_dep.name = "phase3_keep";
                        keep_dep.source = pscript::package::DependencySourceKind::Path;
                        keep_dep.path = fixture_path;
                        keep_dep.constraint.raw = "1.0.0";
                        manifest.dependencies["phase3_keep"] = keep_dep;
                        pscript::package::write_manifest_file(paths.manifest.string(), manifest, &error);
                        const CommandResult install = run_command({exe, "install"}, in_app);
                        const CommandResult sync_ok = run_command({exe, "sync"}, in_app);
                        const CommandResult audit_ok = run_command({exe, "audit"}, in_app);
                        const CommandResult update_all = run_command({exe, "update"}, in_app);
                        const CommandResult remove_missing =
                            run_command({exe, "remove", "not_a_dep"}, in_app);
                        const CommandResult remove_greet =
                            run_command({exe, "remove", "phase3_greet"}, in_app);
                        const CommandResult list_after = run_command({exe, "list"}, in_app);
                        const std::filesystem::path greet_install =
                            app_dir / ".pscript" / "deps" / "phase3_greet-1.0.0";
                        if (install.exit_code != 0 || sync_ok.exit_code != 0 || !contains(sync_ok.output, "match") ||
                            audit_ok.exit_code != 0 || !contains(audit_ok.output, "INFO") ||
                            update_all.exit_code != 0 || remove_missing.exit_code != 2 ||
                            remove_greet.exit_code != 0 || !contains(remove_greet.output, "Removed") ||
                            std::filesystem::exists(greet_install) || !contains(list_after.output, "phase3_keep")) {
                            fail_test("phase3 dependency commands",
                                      "install=" + std::to_string(install.exit_code) + " sync=" +
                                          std::to_string(sync_ok.exit_code) + " audit=" +
                                          std::to_string(audit_ok.exit_code) + " remove=" +
                                          std::to_string(remove_greet.exit_code));
                        } else {
                            pass_test("phase3 dependency commands");
                        }
                    }
            }
        }
    }

    {
        std::istringstream in("x = 2\nprint(x)\n.exit\n");
        std::ostringstream out;
        std::ostringstream err;
        const pscript::CliRunResult result = pscript::run_cli_with_args({"repl"}, out, err, in);
        if (result.exit_code != 0 || normalize_newlines(out.str()) != ">>> >>> 2\n>>> ") {
            fail_test("repl state", "exit=" + std::to_string(result.exit_code) + " out=\"" + out.str() + "\"");
        } else {
            pass_test("repl state");
        }
    }

    {
        std::istringstream in("print(1/0)\n.exit\n");
        std::ostringstream out;
        std::ostringstream err;
        const pscript::CliRunResult result = pscript::run_cli_with_args({"repl"}, out, err, in);
        if (result.exit_code != 0 || !contains(err.str(), "Unhandled exception:")) {
            fail_test("repl runtime error", "exit=" + std::to_string(result.exit_code) + " err=\"" + err.str() + "\"");
        } else {
            pass_test("repl runtime error");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All CLI tests passed\n";
    return 0;
}
