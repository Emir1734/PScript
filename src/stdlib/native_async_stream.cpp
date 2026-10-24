#include "stdlib/native_async_stream.h"
#include "stdlib/native_impl.h"

#include "runtime/runtime.h"

namespace pscript {

bool is_native_async_stream(const Value& value) {
    return value.kind == ValueKind::Class && value.class_value && value.class_value->native_async_stream != nullptr;
}

Value make_native_async_stream(std::uint32_t class_index, std::vector<std::string> chunks) {
    Value object = Runtime::make_class(class_index, "AsyncStream", {});
    auto data = std::make_shared<NativeAsyncStreamData>();
    data->chunks = std::move(chunks);
    object.class_value->native_async_stream = data;
    return object;
}

Value call_native_async_stream_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                      Runtime& runtime) {
    if (!is_native_async_stream(object)) {
        throw RuntimeError("invalid AsyncStream", RuntimeErrorCategory::Type);
    }
    const std::shared_ptr<NativeAsyncStreamData>& data = object.class_value->native_async_stream;
    if (name == "iter") {
        if (!args.empty()) {
            throw RuntimeError("'iter' expects 0 arguments", RuntimeErrorCategory::Type);
        }
        return object;
    }
    if (name == "anext" || name == "next") {
        if (!args.empty()) {
            throw RuntimeError("'" + std::string(name) + "' expects 0 arguments", RuntimeErrorCategory::Type);
        }
        if (data->closed || data->read_index >= data->chunks.size()) {
            throw RuntimeError("StopIteration", RuntimeErrorCategory::Runtime);
        }
        const std::string chunk = data->chunks[data->read_index++];
        if (runtime.async_hooks().track_task) {
            auto task = std::make_shared<TaskData>();
            runtime.async_hooks().track_task(task);
            task->state = TaskState::Completed;
            task->result = Runtime::make_string(chunk);
            return Runtime::make_task(task);
        }
        return Runtime::make_string(chunk);
    }
    if (name == "async_read") {
        if (!args.empty()) {
            throw RuntimeError("'async_read' expects 0 arguments", RuntimeErrorCategory::Type);
        }
        if (data->closed || data->read_index >= data->chunks.size()) {
            return Runtime::make_string("");
        }
        const std::string chunk = data->chunks[data->read_index++];
        if (runtime.async_hooks().track_task) {
            auto task = std::make_shared<TaskData>();
            runtime.async_hooks().track_task(task);
            task->state = TaskState::Completed;
            task->result = Runtime::make_string(chunk);
            return Runtime::make_task(task);
        }
        return Runtime::make_string(chunk);
    }
    if (name == "async_write") {
        if (args.size() != 1) {
            throw RuntimeError("'async_write' expects 1 argument", RuntimeErrorCategory::Type);
        }
        if (args[0].kind != ValueKind::String) {
            throw RuntimeError("'async_write' expects a string", RuntimeErrorCategory::Type);
        }
        data->chunks.push_back(args[0].string);
        if (runtime.async_hooks().track_task) {
            auto task = std::make_shared<TaskData>();
            runtime.async_hooks().track_task(task);
            task->state = TaskState::Completed;
            task->result = Runtime::make_null();
            return Runtime::make_task(task);
        }
        return Runtime::make_null();
    }
    if (name == "close") {
        if (!args.empty()) {
            throw RuntimeError("'close' expects 0 arguments", RuntimeErrorCategory::Type);
        }
        data->closed = true;
        return Runtime::make_null();
    }
    throw RuntimeError("AsyncStream has no method '" + std::string(name) + "'", RuntimeErrorCategory::Type);
}

}  // namespace pscript
