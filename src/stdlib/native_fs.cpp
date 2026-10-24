#include "stdlib/native_common.h"
#include "runtime/async_io.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace pscript {
namespace {

std::filesystem::path fs_path_arg(const std::vector<Value>& args, std::size_t index, std::string_view function) {
    return std::filesystem::path(native_require_string_arg(args, index, function, "path"));
}

[[noreturn]] void fs_fail(std::string_view operation, const std::filesystem::path& path, const std::exception& ex) {
    native_runtime_error(native_filesystem_message(operation, path.generic_string(), ex));
}

[[noreturn]] void fs_fail(std::string_view operation, const std::filesystem::path& path, const std::string& message) {
    native_runtime_error(std::string(operation) + " failed for '" + path.generic_string() + "': " + message);
}

std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fs_fail("read", path, "file not found or not readable");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write_file_text(const std::filesystem::path& path, std::string_view content, bool append) {
    const auto mode = append ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc);
    std::ofstream out(path, mode);
    if (!out) {
        fs_fail(append ? "append" : "write", path, "file not writable");
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
        fs_fail(append ? "append" : "write", path, "write failed");
    }
}

bool fs_exists_impl(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return false;
        }
        fs_fail("exists", path, std::system_error(ec));
    }
    return status.type() != std::filesystem::file_type::not_found;
}

}  // namespace

Value native_fs_exists(const std::vector<Value>& args) {
    native_require_argc("exists", args, 1);
    return Runtime::make_bool(fs_exists_impl(fs_path_arg(args, 0, "exists")));
}

Value native_fs_is_file(const std::vector<Value>& args) {
    native_require_argc("is_file", args, 1);
    const std::filesystem::path path = fs_path_arg(args, 0, "is_file");
    std::error_code ec;
    const bool result = std::filesystem::is_regular_file(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return Runtime::make_bool(false);
        }
        fs_fail("is_file", path, std::system_error(ec));
    }
    return Runtime::make_bool(result);
}

Value native_fs_is_dir(const std::vector<Value>& args) {
    native_require_argc("is_dir", args, 1);
    const std::filesystem::path path = fs_path_arg(args, 0, "is_dir");
    std::error_code ec;
    const bool result = std::filesystem::is_directory(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return Runtime::make_bool(false);
        }
        fs_fail("is_dir", path, std::system_error(ec));
    }
    return Runtime::make_bool(result);
}

Value native_fs_read(const std::vector<Value>& args) {
    native_require_argc("read", args, 1);
    const std::filesystem::path path = fs_path_arg(args, 0, "read");
    if (!std::filesystem::is_regular_file(path)) {
        if (std::filesystem::is_directory(path)) {
            fs_fail("read", path, "path is a directory");
        }
        fs_fail("read", path, "file not found");
    }
    try {
        return Runtime::make_string(read_file_text(path));
    } catch (const RuntimeError&) {
        throw;
    } catch (const std::exception& ex) {
        fs_fail("read", path, ex);
    }
}

Value native_fs_write(const std::vector<Value>& args) {
    native_require_argc("write", args, 2);
    const std::filesystem::path path = fs_path_arg(args, 0, "write");
    const std::string& content = native_require_string(args[1], "write", "content");
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        fs_fail("write", path, "path is a directory");
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        fs_fail("write", path, "parent directory does not exist");
    }
    try {
        write_file_text(path, content, false);
    } catch (const RuntimeError&) {
        throw;
    } catch (const std::exception& ex) {
        fs_fail("write", path, ex);
    }
    return Runtime::make_null();
}

Value native_fs_append(const std::vector<Value>& args) {
    native_require_argc("append", args, 2);
    const std::filesystem::path path = fs_path_arg(args, 0, "append");
    const std::string& content = native_require_string(args[1], "append", "content");
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        fs_fail("append", path, "path is a directory");
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        fs_fail("append", path, "parent directory does not exist");
    }
    try {
        write_file_text(path, content, true);
    } catch (const RuntimeError&) {
        throw;
    } catch (const std::exception& ex) {
        fs_fail("append", path, ex);
    }
    return Runtime::make_null();
}

Value native_fs_copy(const std::vector<Value>& args) {
    native_require_argc("copy", args, 2);
    const std::filesystem::path source = fs_path_arg(args, 0, "copy");
    const std::filesystem::path destination = fs_path_arg(args, 1, "copy");
    if (!std::filesystem::exists(source)) {
        fs_fail("copy", source, "source not found");
    }
    try {
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
    } catch (const std::exception& ex) {
        fs_fail("copy", source, ex);
    }
    return Runtime::make_null();
}

Value native_fs_move(const std::vector<Value>& args) {
    native_require_argc("move", args, 2);
    const std::filesystem::path source = fs_path_arg(args, 0, "move");
    const std::filesystem::path destination = fs_path_arg(args, 1, "move");
    if (!std::filesystem::exists(source)) {
        fs_fail("move", source, "source not found");
    }
    try {
        std::filesystem::rename(source, destination);
    } catch (const std::exception& ex) {
        fs_fail("move", source, ex);
    }
    return Runtime::make_null();
}

Value native_fs_remove(const std::vector<Value>& args) {
    native_require_argc("remove", args, 1);
    const std::filesystem::path path = fs_path_arg(args, 0, "remove");
    if (!std::filesystem::exists(path)) {
        fs_fail("remove", path, "path not found");
    }
    if (std::filesystem::is_directory(path)) {
        fs_fail("remove", path, "path is a directory");
    }
    std::error_code ec;
    if (!std::filesystem::remove(path, ec) || ec) {
        fs_fail("remove", path, ec ? std::system_error(ec) : std::runtime_error("remove failed"));
    }
    return Runtime::make_null();
}

Value native_fs_create_dir(const std::vector<Value>& args) {
    native_require_argc("create_dir", args, 1);
    const std::filesystem::path path = fs_path_arg(args, 0, "create_dir");
    if (std::filesystem::exists(path)) {
        if (std::filesystem::is_directory(path)) {
            return Runtime::make_null();
        }
        fs_fail("create_dir", path, "path already exists and is not a directory");
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        fs_fail("create_dir", path, "parent directory does not exist");
    }
    std::error_code ec;
    if (!std::filesystem::create_directory(path, ec) || ec) {
        fs_fail("create_dir", path, ec ? std::system_error(ec) : std::runtime_error("create_dir failed"));
    }
    return Runtime::make_null();
}

Value native_fs_remove_dir(const std::vector<Value>& args) {
    native_require_argc("remove_dir", args, 1);
    const std::filesystem::path path = fs_path_arg(args, 0, "remove_dir");
    if (!std::filesystem::exists(path)) {
        fs_fail("remove_dir", path, "directory not found");
    }
    if (!std::filesystem::is_directory(path)) {
        fs_fail("remove_dir", path, "path is not a directory");
    }
    std::error_code ec;
    if (!std::filesystem::remove(path, ec) || ec) {
        fs_fail("remove_dir", path, ec ? std::system_error(ec) : std::runtime_error("directory not empty"));
    }
    return Runtime::make_null();
}

Value native_fs_list_dir(const std::vector<Value>& args) {
    native_require_argc("list_dir", args, 1);
    const std::filesystem::path path = fs_path_arg(args, 0, "list_dir");
    if (!std::filesystem::exists(path)) {
        fs_fail("list_dir", path, "directory not found");
    }
    if (!std::filesystem::is_directory(path)) {
        fs_fail("list_dir", path, "path is not a directory");
    }
    try {
        std::vector<Value> entries;
        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(path)) {
            entries.push_back(Runtime::make_string(entry.path().filename().generic_string()));
        }
        std::sort(entries.begin(), entries.end(), [](const Value& left, const Value& right) {
            return left.string < right.string;
        });
        return Runtime::make_list(std::move(entries));
    } catch (const std::exception& ex) {
        fs_fail("list_dir", path, ex);
    }
}

Value native_fs_async_read(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("async_read", args, 1);
    const std::string& path = native_require_string_arg(args, 0, "async_read", "path");
    return runtime.async_io().submit_fs_read(path);
}

Value native_fs_async_write(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("async_write", args, 2);
    const std::string& path = native_require_string_arg(args, 0, "async_write", "path");
    const std::string& content = native_require_string(args[1], "async_write", "content");
    return runtime.async_io().submit_fs_write(path, content, false);
}

Value native_fs_async_append(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("async_append", args, 2);
    const std::string& path = native_require_string_arg(args, 0, "async_append", "path");
    const std::string& content = native_require_string(args[1], "async_append", "content");
    return runtime.async_io().submit_fs_write(path, content, true);
}

}  // namespace pscript
