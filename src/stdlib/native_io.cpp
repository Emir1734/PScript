#include "stdlib/native_common.h"
#include "stdlib/stdlib.h"
#include "runtime/async_io.h"

#include <fstream>
#include <sstream>

namespace pscript {
namespace {

constexpr std::uint32_t kIoFileHandleClassIndex = 0;

[[noreturn]] void io_fail(const std::string& message) {
    native_runtime_error(message);
}

[[noreturn]] void io_fail_path(std::string_view operation, const std::string& path, const std::exception& ex) {
    io_fail(native_filesystem_message(operation, path, ex));
}

NativeFileHandleData& require_file_handle(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_file) {
        io_fail("invalid file handle operation");
    }
    return *object.class_value->native_file;
}

const NativeFileHandleData& require_open_file_handle(const Value& object) {
    NativeFileHandleData& data = require_file_handle(object);
    if (data.closed || !data.stream || !data.stream->is_open()) {
        io_fail("file handle is closed");
    }
    return data;
}

struct OpenMode {
    std::ios::openmode mode = std::ios::in;
    bool can_read = false;
    bool can_write = false;
};

OpenMode parse_mode(std::string_view mode_text, const std::string& path) {
    OpenMode parsed;
    if (mode_text == "r") {
        parsed.mode = std::ios::in;
        parsed.can_read = true;
    } else if (mode_text == "w") {
        parsed.mode = std::ios::out | std::ios::trunc;
        parsed.can_write = true;
    } else if (mode_text == "a") {
        parsed.mode = std::ios::out | std::ios::app;
        parsed.can_write = true;
    } else if (mode_text == "r+") {
        parsed.mode = std::ios::in | std::ios::out;
        parsed.can_read = true;
        parsed.can_write = true;
    } else if (mode_text == "w+") {
        parsed.mode = std::ios::in | std::ios::out | std::ios::trunc;
        parsed.can_read = true;
        parsed.can_write = true;
    } else if (mode_text == "a+") {
        parsed.mode = std::ios::in | std::ios::out | std::ios::app;
        parsed.can_read = true;
        parsed.can_write = true;
    } else {
        native_value_error("unsupported file open mode '" + std::string(mode_text) + "'");
    }
    if (path.empty()) {
        native_value_error("file path must not be empty");
    }
    return parsed;
}

std::shared_ptr<std::fstream> open_stream(const std::string& path, const OpenMode& open_mode) {
    auto stream = std::make_shared<std::fstream>(path, open_mode.mode | std::ios::binary);
    if (!stream->is_open()) {
        io_fail("open failed for '" + path + "': file not found or not accessible");
    }
    return stream;
}

}  // namespace

Value make_native_file_handle(std::uint32_t class_index, std::string path, std::string mode) {
    const OpenMode open_mode = parse_mode(mode, path);
    auto data = std::make_shared<NativeFileHandleData>();
    data->kind = static_cast<std::uint32_t>(NativeClassKind::FileHandle);
    data->path = path;
    data->mode = std::move(mode);
    try {
        data->stream = open_stream(data->path, open_mode);
    } catch (const RuntimeError&) {
        throw;
    } catch (const std::exception& ex) {
        io_fail_path("open", data->path, ex);
    }
    data->closed = false;
    Value value = Runtime::make_class(class_index, "FileHandle", std::vector<std::string>{});
    value.class_value->native_file = std::move(data);
    return value;
}

Value native_io_open(const std::vector<Value>& args) {
    native_require_argc("open", args, 2);
    const std::string& path = native_require_string_arg(args, 0, "open", "path");
    const std::string& mode = native_require_string_arg(args, 1, "open", "mode");
    return make_native_file_handle(kIoFileHandleClassIndex, path, mode);
}

Value call_native_file_handle_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                     Runtime& runtime) {
    NativeFileHandleData& data = require_file_handle(object);

    if (name == "is_open") {
        native_require_argc("is_open", args, 0);
        const bool open = !data.closed && data.stream && data.stream->is_open();
        return Runtime::make_bool(open);
    }
    if (name == "close") {
        native_require_argc("close", args, 0);
        close_native_file_handle(data);
        return Runtime::make_null();
    }
    if (name == "flush") {
        native_require_argc("flush", args, 0);
        require_open_file_handle(object);
        if (!data.stream->flush()) {
            io_fail("flush failed for '" + data.path + "'");
        }
        return Runtime::make_null();
    }
    if (name == "read") {
        native_require_argc("read", args, 0);
        const NativeFileHandleData& open_data = require_open_file_handle(object);
        const OpenMode open_mode = parse_mode(open_data.mode, open_data.path);
        if (!open_mode.can_read) {
            io_fail("file is not open for reading");
        }
        std::ostringstream buffer;
        buffer << open_data.stream->rdbuf();
        return Runtime::make_string(buffer.str());
    }
    if (name == "write") {
        native_require_argc("write", args, 1);
        const NativeFileHandleData& open_data = require_open_file_handle(object);
        const OpenMode open_mode = parse_mode(open_data.mode, open_data.path);
        if (!open_mode.can_write) {
            io_fail("file is not open for writing");
        }
        const std::string& content = native_require_string(args[0], "write", "content");
        if (!open_data.stream->write(content.data(), static_cast<std::streamsize>(content.size()))) {
            io_fail("write failed for '" + open_data.path + "'");
        }
        return Runtime::make_null();
    }
    if (name == "async_read") {
        native_require_argc("async_read", args, 0);
        require_open_file_handle(object);
        return runtime.async_io().submit_handle_read(object.class_value->native_file);
    }
    if (name == "async_write") {
        native_require_argc("async_write", args, 1);
        require_open_file_handle(object);
        const std::string& content = native_require_string(args[0], "async_write", "content");
        return runtime.async_io().submit_handle_write(object.class_value->native_file, content);
    }

    io_fail(std::string("unknown file handle method '") + std::string(name) + "'");
}

}  // namespace pscript
