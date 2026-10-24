#include "stdlib/native_common.h"

#include <cstdlib>
#include <cwchar>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
extern char** environ;
#endif

namespace pscript {
namespace {

[[noreturn]] void os_fail(std::string_view operation, const std::string& detail) {
    native_runtime_error(std::string(operation) + " failed: " + detail);
}

std::string path_to_string(const std::filesystem::path& path) {
    return path.generic_string();
}

const std::string& require_env_name(const Value& value, std::string_view function) {
    const std::string& name = native_require_string(value, function, "name");
    if (name.empty()) {
        native_value_error(std::string("'") + std::string(function) + "' expects a non-empty name");
    }
    return name;
}

#ifdef _WIN32

std::string wide_to_utf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

bool set_env_var(const std::string& name, const std::string& value) {
    return SetEnvironmentVariableA(name.c_str(), value.c_str()) != 0;
}

bool remove_env_var(const std::string& name) {
    return SetEnvironmentVariableA(name.c_str(), nullptr) != 0;
}

bool has_env_var(const std::string& name) {
    const DWORD size = GetEnvironmentVariableA(name.c_str(), nullptr, 0);
    return size > 0;
}

Value collect_env() {
    std::vector<std::pair<std::string, Value>> entries;
    wchar_t* block = GetEnvironmentStringsW();
    if (block == nullptr) {
        os_fail("env", "unable to read environment");
    }
    for (const wchar_t* cursor = block; *cursor != L'\0';) {
        const std::string entry = wide_to_utf8(cursor);
        const std::size_t equal = entry.find('=');
        if (equal != std::string::npos) {
            entries.emplace_back(entry.substr(0, equal), Runtime::make_string(entry.substr(equal + 1)));
        }
        cursor += std::wcslen(cursor) + 1;
    }
    FreeEnvironmentStringsW(block);
    return Runtime::make_dict(std::move(entries));
}

std::string home_directory() {
    if (const char* profile = std::getenv("USERPROFILE")) {
        return profile;
    }
    os_fail("home_dir", "USERPROFILE is not set");
}

#else

bool set_env_var(const std::string& name, const std::string& value) {
    return setenv(name.c_str(), value.c_str(), 1) == 0;
}

bool remove_env_var(const std::string& name) {
    return unsetenv(name.c_str()) == 0;
}

bool has_env_var(const std::string& name) {
    return std::getenv(name.c_str()) != nullptr;
}

Value collect_env() {
    std::vector<std::pair<std::string, Value>> entries;
    if (environ != nullptr) {
        for (char** cursor = environ; *cursor != nullptr; ++cursor) {
            const std::string entry = *cursor;
            const std::size_t equal = entry.find('=');
            if (equal != std::string::npos) {
                entries.emplace_back(entry.substr(0, equal), Runtime::make_string(entry.substr(equal + 1)));
            }
        }
    }
    return Runtime::make_dict(std::move(entries));
}

std::string home_directory() {
    if (const char* home = std::getenv("HOME")) {
        return home;
    }
    os_fail("home_dir", "HOME is not set");
}

#endif

}  // namespace

Value native_os_name(const std::vector<Value>& args) {
    native_require_argc("name", args, 0);
#ifdef _WIN32
    return Runtime::make_string("Windows");
#elif defined(__linux__)
    return Runtime::make_string("Linux");
#else
    return Runtime::make_string("Unix");
#endif
}

Value native_os_platform(const std::vector<Value>& args) {
    native_require_argc("platform", args, 0);
#ifdef _WIN32
    return Runtime::make_string("win32");
#elif defined(__linux__)
    return Runtime::make_string("linux");
#else
    return Runtime::make_string("unix");
#endif
}

Value native_os_arch(const std::vector<Value>& args) {
    native_require_argc("arch", args, 0);
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
    return Runtime::make_string("x86_64");
#elif defined(__aarch64__) || defined(_M_ARM64)
    return Runtime::make_string("arm64");
#elif defined(__i386__) || defined(_M_IX86)
    return Runtime::make_string("x86");
#else
    return Runtime::make_string("unknown");
#endif
}

Value native_os_cwd(const std::vector<Value>& args) {
    native_require_argc("cwd", args, 0);
    try {
        return Runtime::make_string(path_to_string(std::filesystem::current_path()));
    } catch (const std::exception& ex) {
        os_fail("cwd", ex.what());
    }
}

Value native_os_chdir(const std::vector<Value>& args) {
    native_require_argc("chdir", args, 1);
    const std::filesystem::path path(native_require_string_arg(args, 0, "chdir", "path"));
    try {
        std::filesystem::current_path(path);
    } catch (const std::exception& ex) {
        os_fail("chdir", native_filesystem_message("chdir", path.generic_string(), ex));
    }
    return Runtime::make_null();
}

Value native_os_env(const std::vector<Value>& args) {
    native_require_argc("env", args, 0);
    return collect_env();
}

Value native_os_set_env(const std::vector<Value>& args) {
    native_require_argc("set_env", args, 2);
    const std::string& name = require_env_name(args[0], "set_env");
    const std::string& value = native_require_string(args[1], "set_env", "value");
    if (!set_env_var(name, value)) {
        os_fail("set_env", "unable to set environment variable '" + name + "'");
    }
    return Runtime::make_null();
}

Value native_os_remove_env(const std::vector<Value>& args) {
    native_require_argc("remove_env", args, 1);
    const std::string& name = require_env_name(args[0], "remove_env");
    if (!has_env_var(name)) {
        return Runtime::make_null();
    }
    if (!remove_env_var(name)) {
        os_fail("remove_env", "unable to remove environment variable '" + name + "'");
    }
    return Runtime::make_null();
}

Value native_os_has_env(const std::vector<Value>& args) {
    native_require_argc("has_env", args, 1);
    const std::string& name = require_env_name(args[0], "has_env");
    return Runtime::make_bool(has_env_var(name));
}

Value native_os_home_dir(const std::vector<Value>& args) {
    native_require_argc("home_dir", args, 0);
    return Runtime::make_string(home_directory());
}

Value native_os_temp_dir(const std::vector<Value>& args) {
    native_require_argc("temp_dir", args, 0);
    try {
        return Runtime::make_string(path_to_string(std::filesystem::temp_directory_path()));
    } catch (const std::exception& ex) {
        os_fail("temp_dir", ex.what());
    }
}

}  // namespace pscript
