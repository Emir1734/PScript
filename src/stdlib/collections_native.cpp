#include "stdlib/stdlib.h"

#include "runtime/runtime.h"

namespace pscript {
namespace {

const char* collection_kind_name(NativeClassKind kind) {
    switch (kind) {
        case NativeClassKind::Stack:
            return "Stack";
        case NativeClassKind::Queue:
            return "Queue";
        case NativeClassKind::Deque:
            return "Deque";
        default:
            return "Collection";
    }
}

[[noreturn]] void collection_fail(const std::string& message) {
    throw RuntimeError(message, RuntimeErrorCategory::Value);
}

NativeCollectionData& require_native_collection(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_collection) {
        collection_fail("invalid collection operation");
    }
    return *object.class_value->native_collection;
}

}  // namespace

bool is_native_collection(const Value& object) {
    return object.kind == ValueKind::Class && object.class_value && object.class_value->native_collection;
}

NativeClassKind native_collection_kind(const Value& object) {
    if (!is_native_collection(object)) {
        return NativeClassKind::None;
    }
    return static_cast<NativeClassKind>(object.class_value->native_collection->kind);
}

Value make_native_collection(NativeClassKind kind, std::uint32_t class_index) {
    auto data = std::make_shared<NativeCollectionData>();
    data->kind = static_cast<std::uint32_t>(kind);
    Value value = Runtime::make_class(class_index, collection_kind_name(kind), std::vector<std::string>{});
    value.class_value->native_collection = std::move(data);
    return value;
}

Value call_native_collection_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    NativeCollectionData& data = require_native_collection(object);
    const NativeClassKind kind = static_cast<NativeClassKind>(data.kind);

    if (name == "is_empty") {
        if (args.size() != 0) {
            collection_fail("'is_empty' expects no arguments");
        }
        return Runtime::make_bool(data.items.empty());
    }
    if (name == "length") {
        if (args.size() != 0) {
            collection_fail("'length' expects no arguments");
        }
        return Runtime::make_integer(static_cast<std::int64_t>(data.items.size()));
    }

    if (kind == NativeClassKind::Stack) {
        if (name == "push") {
            if (args.size() != 1) {
                collection_fail("'push' expects 1 argument");
            }
            data.items.push_back(args[0]);
            return Runtime::make_null();
        }
        if (name == "pop") {
            if (args.size() != 0) {
                collection_fail("'pop' expects no arguments");
            }
            if (data.items.empty()) {
                collection_fail("stack is empty");
            }
            Value result = std::move(data.items.back());
            data.items.pop_back();
            return result;
        }
        if (name == "peek") {
            if (args.size() != 0) {
                collection_fail("'peek' expects no arguments");
            }
            if (data.items.empty()) {
                collection_fail("stack is empty");
            }
            return data.items.back();
        }
    }

    if (kind == NativeClassKind::Queue) {
        if (name == "enqueue") {
            if (args.size() != 1) {
                collection_fail("'enqueue' expects 1 argument");
            }
            data.items.push_back(args[0]);
            return Runtime::make_null();
        }
        if (name == "dequeue") {
            if (args.size() != 0) {
                collection_fail("'dequeue' expects no arguments");
            }
            if (data.items.empty()) {
                collection_fail("queue is empty");
            }
            Value result = std::move(data.items.front());
            data.items.erase(data.items.begin());
            return result;
        }
        if (name == "peek") {
            if (args.size() != 0) {
                collection_fail("'peek' expects no arguments");
            }
            if (data.items.empty()) {
                collection_fail("queue is empty");
            }
            return data.items.front();
        }
    }

    if (kind == NativeClassKind::Deque) {
        if (name == "push_front") {
            if (args.size() != 1) {
                collection_fail("'push_front' expects 1 argument");
            }
            data.items.insert(data.items.begin(), args[0]);
            return Runtime::make_null();
        }
        if (name == "push_back") {
            if (args.size() != 1) {
                collection_fail("'push_back' expects 1 argument");
            }
            data.items.push_back(args[0]);
            return Runtime::make_null();
        }
        if (name == "pop_front") {
            if (args.size() != 0) {
                collection_fail("'pop_front' expects no arguments");
            }
            if (data.items.empty()) {
                collection_fail("deque is empty");
            }
            Value result = std::move(data.items.front());
            data.items.erase(data.items.begin());
            return result;
        }
        if (name == "pop_back") {
            if (args.size() != 0) {
                collection_fail("'pop_back' expects no arguments");
            }
            if (data.items.empty()) {
                collection_fail("deque is empty");
            }
            Value result = std::move(data.items.back());
            data.items.pop_back();
            return result;
        }
        if (name == "front") {
            if (args.size() != 0) {
                collection_fail("'front' expects no arguments");
            }
            if (data.items.empty()) {
                collection_fail("deque is empty");
            }
            return data.items.front();
        }
        if (name == "back") {
            if (args.size() != 0) {
                collection_fail("'back' expects no arguments");
            }
            if (data.items.empty()) {
                collection_fail("deque is empty");
            }
            return data.items.back();
        }
    }

    collection_fail(std::string("unknown collection method '") + std::string(name) + "'");
}

}  // namespace pscript
