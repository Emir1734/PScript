#include "package/git_clone.h"

#include <algorithm>
#include <cctype>
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
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace pscript::package {
namespace {

bool contains_unsafe_shell_characters(std::string_view text) {
    for (char c : text) {
        switch (c) {
            case '\0':
            case '\n':
            case '\r':
            case '\t':
            case ';':
            case '|':
            case '&':
            case '$':
            case '`':
            case '"':
            case '\'':
            case '<':
            case '>':
            case '(':
            case ')':
            case '{':
            case '}':
            case '*':
            case '?':
            case '!':
            case '#':
            case '%':
            case '~':
                return true;
            default:
                break;
        }
    }
    return false;
}

bool looks_like_git_remote(const std::string& url) {
    if (url.empty() || url.size() > 4096) {
        return false;
    }
    if (url.find("://") != std::string::npos) {
        return true;
    }
    if (url.find('@') != std::string::npos) {
        return true;
    }
    std::filesystem::path as_path(url);
    std::error_code ec;
    if (as_path.is_absolute() || std::filesystem::exists(as_path, ec)) {
        return true;
    }
    if (url.find('/') != std::string::npos || url.find('\\') != std::string::npos) {
        return true;
    }
    return false;
}

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

std::string quote_windows_arg(const std::wstring& arg) {
    std::wstring quoted = L"\"";
    for (wchar_t c : arg) {
        if (c == L'"') {
            quoted += L'\\';
        }
        quoted += c;
    }
    quoted += L'"';
    const int needed = WideCharToMultiByte(CP_UTF8, 0, quoted.data(), static_cast<int>(quoted.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) {
        return std::string();
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, quoted.data(), static_cast<int>(quoted.size()), out.data(), needed, nullptr,
                        nullptr);
    return out;
}

int run_git_argv(const std::vector<std::wstring>& argv) {
    if (argv.empty()) {
        return -1;
    }
    std::wstring command_line;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            command_line += L' ';
        }
        command_line += L'"';
        for (wchar_t c : argv[i]) {
            if (c == L'"') {
                command_line += L'\\';
            }
            command_line += c;
        }
        command_line += L'"';
    }
    std::vector<wchar_t> mutable_cmd(command_line.begin(), command_line.end());
    mutable_cmd.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    const BOOL created =
        CreateProcessW(L"git.exe", mutable_cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                     &startup, &process);
    if (!created) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

#else

int run_git_argv(const std::vector<std::string>& argv) {
    if (argv.empty()) {
        return -1;
    }
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const std::string& arg : argv) {
        cargv.push_back(const_cast<char*>(arg.c_str()));
    }
    cargv.push_back(nullptr);
    pid_t pid = 0;
    const int spawn_result = posix_spawnp(&pid, "git", nullptr, nullptr, cargv.data(), environ);
    if (spawn_result != 0) {
        return -1;
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

#endif

}  // namespace

bool validate_git_remote_url(const std::string& url, std::string* error) {
    if (contains_unsafe_shell_characters(url)) {
        if (error) {
            *error = "git URL contains unsupported characters";
        }
        return false;
    }
    if (!looks_like_git_remote(url)) {
        if (error) {
            *error = "invalid git URL";
        }
        return false;
    }
    return true;
}

bool validate_git_ref(const std::string& ref, std::string* error) {
    if (ref.empty()) {
        return true;
    }
    if (contains_unsafe_shell_characters(ref)) {
        if (error) {
            *error = "git ref contains unsupported characters";
        }
        return false;
    }
    if (!ref.empty() && ref.front() == '-') {
        if (error) {
            *error = "git ref cannot start with '-'";
        }
        return false;
    }
    if (ref.size() > 256) {
        if (error) {
            *error = "git ref is too long";
        }
        return false;
    }
    return true;
}

std::string redact_git_url_for_display(const std::string& url) {
    const std::string https_prefix = "https://";
    const std::string http_prefix = "http://";
    std::string_view rest = url;
    std::string scheme;
    if (url.rfind(https_prefix, 0) == 0) {
        scheme = https_prefix;
        rest = std::string_view(url).substr(https_prefix.size());
    } else if (url.rfind(http_prefix, 0) == 0) {
        scheme = http_prefix;
        rest = std::string_view(url).substr(http_prefix.size());
    } else {
        return url;
    }
    const std::size_t at = rest.find('@');
    if (at == std::string_view::npos) {
        return url;
    }
    return scheme + "***@" + std::string(rest.substr(at + 1));
}

bool run_git_clone(const std::string& url, const std::string& ref, const std::filesystem::path& destination,
                   std::string* error) {
    if (!validate_git_remote_url(url, error)) {
        return false;
    }
    if (!validate_git_ref(ref, error)) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::filesystem::remove_all(destination, ec);

#ifdef _WIN32
    std::vector<std::wstring> argv;
    argv.push_back(L"git");
    argv.push_back(L"clone");
    argv.push_back(L"--depth");
    argv.push_back(L"1");
    if (!ref.empty()) {
        argv.push_back(L"--branch");
        argv.push_back(utf8_to_wide(ref));
    }
    argv.push_back(utf8_to_wide(url));
    argv.push_back(utf8_to_wide(destination.lexically_normal().string()));
#else
    std::vector<std::string> argv;
    argv.push_back("git");
    argv.push_back("clone");
    argv.push_back("--depth");
    argv.push_back("1");
    if (!ref.empty()) {
        argv.push_back("--branch");
        argv.push_back(ref);
    }
    argv.push_back(url);
    argv.push_back(destination.lexically_normal().string());
#endif

    const int code = run_git_argv(argv);
    if (code != 0) {
        if (error) {
            *error = "git clone failed for " + redact_git_url_for_display(url);
        }
        std::filesystem::remove_all(destination, ec);
        return false;
    }
    if (!std::filesystem::is_directory(destination)) {
        if (error) {
            *error = "git clone did not create destination directory";
        }
        return false;
    }
    return true;
}

}  // namespace pscript::package
