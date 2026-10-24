#include "runtime/runtime.h"

#include "runtime/collection_ops.h"

#include "runtime/alloc_guard.h"
#include "runtime/cli_state.h"
#include "runtime/iterator.h"
#include "runtime/async_io.h"

#include "version.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>
#include <utility>

namespace pscript {
namespace {

void dict_rebuild_index(DictData& data) {
    data.key_index.clear();
    for (std::size_t i = 0; i < data.entries.size(); ++i) {
        data.key_index[data.entries[i].first] = i;
    }
}

void dict_emplace(DictData& data, const std::string& key, Value value) {
    if (Value* found = Runtime::dict_lookup_mut(data, key)) {
        *found = std::move(value);
        return;
    }
    if (data.entries.size() == data.entries.capacity()) {
        const std::size_t next = data.entries.capacity() == 0 ? 8 : data.entries.capacity() * 2;
        alloc_guard::throw_if_blocked(next);
        data.entries.reserve(next);
        data.key_index.reserve(next);
    }
    const std::size_t index = data.entries.size();
    data.key_index.emplace(key, index);
    data.entries.emplace_back(key, std::move(value));
}

std::uint64_t value_bucket_key(const Value& value) {
    switch (value.kind) {
        case ValueKind::Null:
            return 0x9E3779B97F4A7C15ULL;
        case ValueKind::Bool:
            return value.boolean ? 1ULL : 2ULL;
        case ValueKind::Integer:
            return static_cast<std::uint64_t>(value.integer) * 0xD6E8FEB86659FD93ULL;
        case ValueKind::Float:
            return std::hash<double>{}(value.floating);
        case ValueKind::String:
            return std::hash<std::string>{}(value.string);
        default: {
            const void* identity = &value;
            switch (value.kind) {
                case ValueKind::Bytes:
                    identity = value.bytes.get();
                    break;
                case ValueKind::List:
                    identity = value.list.get();
                    break;
                case ValueKind::Dict:
                    identity = value.dict.get();
                    break;
                case ValueKind::Tuple:
                    identity = value.tuple.get();
                    break;
                case ValueKind::Set:
                    identity = value.set.get();
                    break;
                case ValueKind::Struct:
                    identity = value.struct_value.get();
                    break;
                case ValueKind::Class:
                    identity = value.class_value.get();
                    break;
                case ValueKind::Enum:
                    identity = value.enum_value.get();
                    break;
                case ValueKind::Iterator:
                    identity = value.iterator_data.get();
                    break;
                case ValueKind::Function:
                    identity = value.function.get();
                    break;
                default:
                    break;
            }
            return (static_cast<std::uint64_t>(value.kind) << 56) ^
                   std::hash<const void*>{}(identity);
        }
    }
}

void set_rebuild_buckets(SetData& data) {
    data.buckets.clear();
    for (std::size_t i = 0; i < data.items.size(); ++i) {
        data.buckets[value_bucket_key(data.items[i])].push_back(i);
    }
}

bool set_contains_indexed(const SetData& data, const Value& value) {
    const auto it = data.buckets.find(value_bucket_key(value));
    if (it == data.buckets.end()) {
        return false;
    }
    for (const std::size_t index : it->second) {
        if (index < data.items.size() && Runtime::equals(data.items[index], value)) {
            return true;
        }
    }
    return false;
}

bool contains_ptr(const std::vector<const void*>& seen, const void* ptr) {
    return std::find(seen.begin(), seen.end(), ptr) != seen.end();
}

}  // namespace

RuntimeError::RuntimeError(std::string message)
    : std::runtime_error(message), message_(std::move(message)), category_(infer_category(message_)) {}

RuntimeError::RuntimeError(std::string message, RuntimeErrorCategory category)
    : std::runtime_error(message), message_(std::move(message)), category_(category) {}

ProcessExit::ProcessExit(int code) : std::runtime_error("process exit"), code_(code) {}

RuntimeErrorCategory RuntimeError::infer_category(const std::string& message) {
    if (message.rfind("cannot convert", 0) == 0 || message.rfind("Type error:", 0) == 0 ||
        message.find("expects ") != std::string::npos || message.find("expects a ") != std::string::npos ||
        message.find("expects an ") != std::string::npos || message.find("expects integer") != std::string::npos ||
        message.find("expects string") != std::string::npos || message.find("must be a ") != std::string::npos ||
        message.find("must be an ") != std::string::npos || message.rfind("not a ", 0) == 0 ||
        message.rfind("cannot call method", 0) == 0 || message.rfind("cannot iterate", 0) == 0 ||
        message.rfind("object is not iterable", 0) == 0 || message.rfind("object has no method", 0) == 0 ||
        message.rfind("cannot use 'in'", 0) == 0 || message.rfind("cannot index", 0) == 0 ||
        message.rfind("cannot access field", 0) == 0 || message.rfind("incompatible struct", 0) == 0) {
        return RuntimeErrorCategory::Type;
    }
    if (message.rfind("undefined variable", 0) == 0 || message.rfind("unknown function", 0) == 0 ||
        message.rfind("undefined dictionary key", 0) == 0 || message.rfind("unknown field", 0) == 0 ||
        message.rfind("unknown ", 0) == 0) {
        return RuntimeErrorCategory::Name;
    }
    if (message.find("index out of range") != std::string::npos ||
        message.rfind("list index", 0) == 0 || message.rfind("tuple index", 0) == 0 ||
        message.rfind("range index", 0) == 0) {
        return RuntimeErrorCategory::Index;
    }
    if (message.rfind("unsupported operand", 0) == 0 || message.rfind("division by zero", 0) == 0 ||
        message.rfind("modulo by zero", 0) == 0 || message.rfind("invalid shift", 0) == 0 ||
        message.rfind("destructuring", 0) == 0 || message.rfind("value not found", 0) == 0 ||
        message.rfind("undefined set", 0) == 0 || message.rfind("cannot sort", 0) == 0 ||
        message.rfind("invalid replace", 0) == 0 || message.rfind("is empty", 0) != std::string::npos ||
        message.rfind("is immutable", 0) != std::string::npos || message.rfind("invalid function", 0) == 0) {
        return RuntimeErrorCategory::Value;
    }
    return RuntimeErrorCategory::Runtime;
}

Runtime::Runtime(std::ostream& output) : Runtime(output, std::cin) {}

Runtime::Runtime(std::ostream& output, std::istream& input) : Runtime(output, input, std::cerr) {}

Runtime::Runtime(std::ostream& output, std::istream& input, std::ostream& error_output)
    : output_(&output), error_output_(&error_output), input_(&input),
      async_io_(std::make_unique<AsyncIoService>(*this)), cli_state_(std::make_unique<CliState>()) {
    register_builtin("print", builtin_print);
    register_builtin("range", builtin_range);
    register_builtin("input", builtin_input);
    register_builtin("int", builtin_int);
    register_builtin("float", builtin_float);
    register_builtin("str", builtin_str);
    register_builtin("bool", builtin_bool);
    register_builtin("type", builtin_type);
    register_builtin("abs", builtin_abs);
    register_builtin("min", builtin_min);
    register_builtin("max", builtin_max);
    register_builtin("round", builtin_round);
    register_builtin("clamp", builtin_clamp);
    register_builtin("filter", builtin_filter);
    register_builtin("map", builtin_map);
    register_builtin("sort_by", builtin_sort_by);
    register_builtin("group_by", builtin_group_by);
    register_builtin("take", builtin_take);
    register_builtin("parallel_map", builtin_parallel_map);
}

Runtime::~Runtime() {
    close_all_sql_databases();
}

void Runtime::register_sql_database(const std::shared_ptr<NativeDatabaseData>& database) {
    if (!database) {
        return;
    }
    std::lock_guard<std::mutex> lock(sql_databases_mutex_);
    sql_databases_.push_back(database);
}

void Runtime::close_all_sql_databases() {
    std::lock_guard<std::mutex> lock(sql_databases_mutex_);
    for (const std::shared_ptr<NativeDatabaseData>& database : sql_databases_) {
        if (!database) {
            continue;
        }
        database->finalize_sql_resources();
        if (database->connection) {
            database->connection->close();
        }
        database->closed = true;
    }
    sql_databases_.clear();
}

Value Runtime::make_integer(std::int64_t value) {
    Value result;
    result.kind = ValueKind::Integer;
    result.integer = value;
    return result;
}

Value Runtime::make_float(double value) {
    Value result;
    result.kind = ValueKind::Float;
    result.floating = value;
    return result;
}

Value Runtime::make_string(std::string value) {
    Value result;
    result.kind = ValueKind::String;
    result.string = std::move(value);
    return result;
}

Value Runtime::concat_strings(const std::string& left, const std::string& right) {
    alloc_guard::throw_if_blocked(left.size() + right.size());
    std::string out;
    out.reserve(left.size() + right.size());
    out.append(left);
    out.append(right);
    return make_string(std::move(out));
}

Value Runtime::concat_strings(std::string&& left, const std::string& right) {
    alloc_guard::throw_if_blocked(left.size() + right.size());
    const std::size_t needed = left.size() + right.size();
    if (left.capacity() < needed) {
        left.reserve(needed > left.capacity() * 2 ? needed : left.capacity() * 2);
    }
    left.append(right);
    return make_string(std::move(left));
}

Value Runtime::make_bool(bool value) {
    Value result;
    result.kind = ValueKind::Bool;
    result.boolean = value;
    return result;
}

Value Runtime::make_null() {
    return Value{};
}

Value Runtime::make_bytes(std::vector<std::uint8_t> data) {
    alloc_guard::throw_if_blocked(data.size());
    Value result;
    result.kind = ValueKind::Bytes;
    result.bytes = std::make_shared<BytesData>();
    result.bytes->data = std::move(data);
    return result;
}

Value Runtime::make_list(std::vector<Value> items) {
    alloc_guard::throw_if_blocked(items.size());
    Value result;
    result.kind = ValueKind::List;
    result.list = std::make_shared<ListData>();
    result.list->items.reserve(items.size());
    result.list->items = std::move(items);
    return result;
}

Value Runtime::make_dict(std::vector<std::pair<std::string, Value>> entries) {
    alloc_guard::throw_if_blocked(entries.size());
    Value result;
    result.kind = ValueKind::Dict;
    result.dict = std::make_shared<DictData>();
    result.dict->entries.reserve(entries.size());
    result.dict->key_index.reserve(entries.size());
    for (auto& entry : entries) {
        dict_emplace(*result.dict, entry.first, std::move(entry.second));
    }
    return result;
}

Value Runtime::make_tuple(std::vector<Value> items) {
    alloc_guard::throw_if_blocked(items.size());
    Value result;
    result.kind = ValueKind::Tuple;
    result.tuple = std::make_shared<TupleData>();
    result.tuple->items.reserve(items.size());
    result.tuple->items = std::move(items);
    return result;
}

Value Runtime::make_struct(std::uint32_t type_index, std::string type_name, std::vector<std::string> field_names) {
    Value result;
    result.kind = ValueKind::Struct;
    result.struct_value = std::make_shared<StructData>();
    result.struct_value->type_index = type_index;
    result.struct_value->type_name = std::move(type_name);
    result.struct_value->field_names = std::move(field_names);
    result.struct_value->fields.assign(result.struct_value->field_names.size(), make_null());
    return result;
}

Value Runtime::make_class(std::uint32_t type_index, std::string type_name, std::vector<std::string> field_names) {
    Value result;
    result.kind = ValueKind::Class;
    result.class_value = std::make_shared<ClassData>();
    result.class_value->type_index = type_index;
    result.class_value->type_name = std::move(type_name);
    result.class_value->field_names = std::move(field_names);
    result.class_value->fields.assign(result.class_value->field_names.size(), make_null());
    for (std::uint32_t i = 0; i < result.class_value->field_names.size(); ++i) {
        result.class_value->field_name_index[result.class_value->field_names[i]] = i;
    }
    return result;
}

Value Runtime::make_enum(std::uint32_t enum_index, std::uint32_t member_index, std::string enum_name,
                         std::string member_name) {
    return make_enum(enum_index, member_index, std::move(enum_name), std::move(member_name), make_null());
}

Value Runtime::make_enum(std::uint32_t enum_index, std::uint32_t member_index, std::string enum_name,
                         std::string member_name, Value payload) {
    Value result;
    result.kind = ValueKind::Enum;
    result.enum_value = std::make_shared<EnumData>();
    result.enum_value->enum_index = enum_index;
    result.enum_value->member_index = member_index;
    result.enum_value->enum_name = std::move(enum_name);
    result.enum_value->member_name = std::move(member_name);
    result.enum_value->payload = std::move(payload);
    return result;
}

Value Runtime::get_field(const Value& object, std::uint32_t field_index) {
    if (object.kind == ValueKind::Struct && object.struct_value) {
        if (field_index >= object.struct_value->fields.size()) {
            fail("unknown field");
        }
        return object.struct_value->fields[field_index];
    }
    if (object.kind == ValueKind::Class && object.class_value) {
        if (field_index >= object.class_value->fields.size()) {
            fail("unknown field");
        }
        return object.class_value->fields[field_index];
    }
    fail("cannot access field on non-object value");
}

void Runtime::set_field(const Value& object, std::uint32_t field_index, Value value) {
    if (object.kind == ValueKind::Struct && object.struct_value) {
        if (field_index >= object.struct_value->fields.size()) {
            fail("unknown field");
        }
        object.struct_value->fields[field_index] = std::move(value);
        return;
    }
    if (object.kind == ValueKind::Class && object.class_value) {
        if (field_index >= object.class_value->fields.size()) {
            fail("unknown field");
        }
        object.class_value->fields[field_index] = std::move(value);
        return;
    }
    fail("cannot access field on non-object value");
}

Value Runtime::make_set(std::vector<Value> items) {
    alloc_guard::throw_if_blocked(items.size());
    Value result;
    result.kind = ValueKind::Set;
    result.set = std::make_shared<SetData>();
    result.set->items.reserve(items.size());
    for (auto& item : items) {
        if (set_contains_indexed(*result.set, item)) {
            continue;
        }
        const std::size_t index = result.set->items.size();
        result.set->buckets[value_bucket_key(item)].push_back(index);
        result.set->items.push_back(std::move(item));
    }
    return result;
}

Value Runtime::make_range(std::int64_t start, std::int64_t stop, std::int64_t step) {
    Value result;
    result.kind = ValueKind::Range;
    result.range = std::make_shared<RangeData>();
    result.range->start = start;
    result.range->stop = stop;
    result.range->step = step;
    return result;
}

Value Runtime::make_function(std::shared_ptr<ClosureData> closure) {
    Value result;
    result.kind = ValueKind::Function;
    result.function = std::move(closure);
    return result;
}

Value Runtime::clone_closure_value(const Value& value) {
    if (value.kind != ValueKind::Function || !value.function) {
        return value;
    }
    const ClosureData& src = *value.function;
    auto data = std::make_shared<ClosureData>();
    data->function_index = src.function_index;
    data->name = src.name;
    data->module_index = src.module_index;
    data->module_class_ctor = src.module_class_ctor;
    data->module_struct_ctor = src.module_struct_ctor;
    data->class_index = src.class_index;
    data->struct_index = src.struct_index;
    data->captures.reserve(src.captures.size());
    for (const std::shared_ptr<Cell>& cell : src.captures) {
        auto copied = std::make_shared<Cell>();
        if (cell && cell->value) {
            copied->value = *cell->value;
        }
        data->captures.push_back(copied);
    }
    return make_function(std::move(data));
}

Value Runtime::make_task(std::shared_ptr<TaskData> task) {
    Value result;
    result.kind = ValueKind::Task;
    result.task = std::move(task);
    return result;
}

Value Runtime::make_module(std::uint32_t module_index) {
    Value result;
    result.kind = ValueKind::Module;
    result.module_namespace = std::make_shared<ModuleNamespaceData>();
    result.module_namespace->module_index = module_index;
    return result;
}

std::string Runtime::format(const Value& value) {
    std::vector<const void*> seen;
    return format_impl(value, seen);
}

std::string Runtime::format_impl(const Value& value, std::vector<const void*>& seen) {
    switch (value.kind) {
        case ValueKind::Integer:
            return std::to_string(value.integer);
        case ValueKind::Float: {
            std::ostringstream out;
            out << value.floating;
            return out.str();
        }
        case ValueKind::String:
            return value.string;
        case ValueKind::Bool:
            return value.boolean ? "true" : "false";
        case ValueKind::Null:
            return "null";
        case ValueKind::Bytes: {
            if (!value.bytes) {
                fail("invalid bytes");
            }
            return "bytes[" + std::to_string(value.bytes->data.size()) + "]";
        }
        case ValueKind::List: {
            if (!value.list) {
                fail("invalid list");
            }
            const void* ptr = value.list.get();
            if (contains_ptr(seen, ptr)) {
                return "[...]";
            }
            seen.push_back(ptr);
            std::string out = "[";
            for (std::size_t i = 0; i < value.list->items.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                out += format_impl(value.list->items[i], seen);
            }
            out += "]";
            seen.pop_back();
            return out;
        }
        case ValueKind::Dict: {
            if (!value.dict) {
                fail("invalid dictionary");
            }
            const void* ptr = value.dict.get();
            if (contains_ptr(seen, ptr)) {
                return "{...}";
            }
            seen.push_back(ptr);
            std::string out = "{";
            for (std::size_t i = 0; i < value.dict->entries.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                out += '"';
                out += value.dict->entries[i].first;
                out += "\": ";
                out += format_impl(value.dict->entries[i].second, seen);
            }
            out += "}";
            seen.pop_back();
            return out;
        }
        case ValueKind::Tuple: {
            if (!value.tuple) {
                fail("invalid tuple");
            }
            const void* ptr = value.tuple.get();
            if (contains_ptr(seen, ptr)) {
                return "(...)";
            }
            seen.push_back(ptr);
            if (value.tuple->items.empty()) {
                seen.pop_back();
                return "()";
            }
            std::string out = "(";
            for (std::size_t i = 0; i < value.tuple->items.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                out += format_impl(value.tuple->items[i], seen);
            }
            if (value.tuple->items.size() == 1) {
                out += ",";
            }
            out += ")";
            seen.pop_back();
            return out;
        }
        case ValueKind::Set: {
            if (!value.set) {
                fail("invalid set");
            }
            const void* ptr = value.set.get();
            if (contains_ptr(seen, ptr)) {
                return "{...}";
            }
            seen.push_back(ptr);
            std::string out = "{";
            for (std::size_t i = 0; i < value.set->items.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                out += format_impl(value.set->items[i], seen);
            }
            out += "}";
            seen.pop_back();
            return out;
        }
        case ValueKind::Struct: {
            if (!value.struct_value) {
                fail("invalid struct");
            }
            const void* ptr = value.struct_value.get();
            if (contains_ptr(seen, ptr)) {
                return value.struct_value->type_name + "{...}";
            }
            seen.push_back(ptr);
            std::string out = value.struct_value->type_name + "{";
            for (std::size_t i = 0; i < value.struct_value->fields.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                if (i < value.struct_value->field_names.size()) {
                    out += value.struct_value->field_names[i];
                    out += ": ";
                }
                out += format_impl(value.struct_value->fields[i], seen);
            }
            out += "}";
            seen.pop_back();
            return out;
        }
        case ValueKind::Class: {
            if (!value.class_value) {
                fail("invalid class instance");
            }
            const void* ptr = value.class_value.get();
            if (contains_ptr(seen, ptr)) {
                return value.class_value->type_name + "{...}";
            }
            seen.push_back(ptr);
            std::string out = value.class_value->type_name + "{";
            for (std::size_t i = 0; i < value.class_value->fields.size(); ++i) {
                if (i != 0) {
                    out += ", ";
                }
                if (i < value.class_value->field_names.size()) {
                    out += value.class_value->field_names[i];
                    out += ": ";
                }
                out += format_impl(value.class_value->fields[i], seen);
            }
            out += "}";
            seen.pop_back();
            return out;
        }
        case ValueKind::Enum: {
            if (!value.enum_value) {
                fail("invalid enum");
            }
            return value.enum_value->enum_name + "." + value.enum_value->member_name;
        }
        case ValueKind::Range: {
            if (!value.range) {
                fail("invalid range");
            }
            std::string out = "range(";
            out += std::to_string(value.range->start);
            out += ", ";
            out += std::to_string(value.range->stop);
            out += ", ";
            out += std::to_string(value.range->step);
            out += ")";
            return out;
        }
        case ValueKind::Iterator:
            return "<iterator>";
        case ValueKind::Function: {
            std::string out = "<function";
            if (value.function && !value.function->name.empty()) {
                out += " ";
                out += value.function->name;
            }
            out += ">";
            return out;
        }
        case ValueKind::Module:
            return "<module>";
        case ValueKind::Task:
            return "<task>";
    }
    fail("unsupported operand type");
}

bool Runtime::is_truthy(const Value& value) {
    switch (value.kind) {
        case ValueKind::Bool:
            return value.boolean;
        case ValueKind::Integer:
            return value.integer != 0;
        case ValueKind::Float:
            return value.floating != 0.0;
        case ValueKind::String:
            return !value.string.empty();
        case ValueKind::Null:
            return false;
        case ValueKind::Bytes:
            return value.bytes && !value.bytes->data.empty();
        case ValueKind::List:
            return value.list && !value.list->items.empty();
        case ValueKind::Dict:
            return value.dict && !value.dict->entries.empty();
        case ValueKind::Tuple:
            return value.tuple && !value.tuple->items.empty();
        case ValueKind::Set:
            return value.set && !value.set->items.empty();
        case ValueKind::Struct:
            return value.struct_value && !value.struct_value->fields.empty();
        case ValueKind::Class:
            return value.class_value && !value.class_value->fields.empty();
        case ValueKind::Enum:
            return true;
        case ValueKind::Range:
            return value.range && range_length(*value.range) != 0;
        case ValueKind::Iterator:
            return value.iterator_data && !value.iterator_data->exhausted;
        case ValueKind::Function:
        case ValueKind::Module:
        case ValueKind::Task:
            return true;
    }
    return false;
}

Value Runtime::get_index(const Value& object, const Value& index) {
    if (object.kind == ValueKind::Bytes) {
        if (!object.bytes) {
            fail("invalid bytes");
        }
        if (index.kind != ValueKind::Integer) {
            fail("bytes index must be an integer");
        }
        if (index.integer < 0 || static_cast<std::uint64_t>(index.integer) >= object.bytes->data.size()) {
            fail("bytes index out of range");
        }
        return make_integer(static_cast<std::int64_t>(object.bytes->data[static_cast<std::size_t>(index.integer)]));
    }
    if (object.kind == ValueKind::String) {
        if (index.kind != ValueKind::Integer) {
            fail("string index must be an integer");
        }
        if (index.integer < 0 || static_cast<std::uint64_t>(index.integer) >= object.string.size()) {
            fail("string index out of range");
        }
        return make_integer(static_cast<std::int64_t>(
            static_cast<unsigned char>(object.string[static_cast<std::size_t>(index.integer)])));
    }
    if (object.kind == ValueKind::List) {
        if (!object.list) {
            fail("invalid list");
        }
        if (index.kind != ValueKind::Integer) {
            fail("list index must be an integer");
        }
        if (index.integer < 0 || static_cast<std::uint64_t>(index.integer) >= object.list->items.size()) {
            fail("list index out of range");
        }
        return object.list->items[static_cast<std::size_t>(index.integer)];
    }
    if (object.kind == ValueKind::Tuple) {
        if (!object.tuple) {
            fail("invalid tuple");
        }
        if (index.kind != ValueKind::Integer) {
            fail("tuple index must be an integer");
        }
        if (index.integer < 0 || static_cast<std::uint64_t>(index.integer) >= object.tuple->items.size()) {
            fail("tuple index out of range");
        }
        return object.tuple->items[static_cast<std::size_t>(index.integer)];
    }
    if (object.kind == ValueKind::Set) {
        fail("cannot index set");
    }
    if (object.kind == ValueKind::Dict) {
        if (!object.dict) {
            fail("invalid dictionary");
        }
        if (index.kind != ValueKind::String) {
            fail("dictionary key must be a string");
        }
        const Value* found = dict_lookup(*object.dict, index.string);
        if (!found) {
            fail("undefined dictionary key");
        }
        return *found;
    }
    if (object.kind == ValueKind::Class && object.class_value) {
        if (index.kind != ValueKind::String) {
            fail("class field name must be a string");
        }
        const auto field_it = object.class_value->field_name_index.find(index.string);
        if (field_it != object.class_value->field_name_index.end() &&
            field_it->second < object.class_value->fields.size()) {
            return object.class_value->fields[field_it->second];
        }
        fail("unknown class field '" + index.string + "'");
    }
    if (index.kind == ValueKind::String) {
        fail("not a dictionary");
    }
    fail("not a list");
}

bool Runtime::test_destructure_shape(const Value& value, std::uint32_t expected_count, std::uint32_t kind) {
    try {
        check_destructure(value, expected_count, kind);
        return true;
    } catch (const RuntimeError&) {
        return false;
    }
}

void Runtime::check_destructure(const Value& value, std::uint32_t expected_count, std::uint32_t kind) {
    const bool minimum = kind >= 2;
    const std::uint32_t collection_kind = minimum ? kind - 2 : kind;
    if (collection_kind == 0) {
        if (value.kind != ValueKind::List) {
            fail("cannot destructure value");
        }
        if (!value.list) {
            fail("invalid list");
        }
        const std::size_t size = value.list->items.size();
        if (minimum) {
            if (size < expected_count) {
                fail("destructuring mismatch");
            }
        } else if (size != expected_count) {
            fail("destructuring mismatch");
        }
        return;
    }
    if (value.kind != ValueKind::Tuple) {
        fail("cannot destructure value");
    }
    if (!value.tuple) {
        fail("invalid tuple");
    }
    const std::size_t size = value.tuple->items.size();
    if (minimum) {
        if (size < expected_count) {
            fail("destructuring mismatch");
        }
    } else if (size != expected_count) {
        fail("destructuring mismatch");
    }
}

Value Runtime::destructure_rest(const Value& value, std::uint32_t start_index) {
    std::vector<Value> items;
    if (value.kind == ValueKind::List) {
        if (!value.list) {
            fail("invalid list");
        }
        if (start_index > value.list->items.size()) {
            fail("destructuring mismatch");
        }
        items.reserve(value.list->items.size() - start_index);
        for (std::size_t i = start_index; i < value.list->items.size(); ++i) {
            items.push_back(value.list->items[i]);
        }
        return make_list(std::move(items));
    }
    if (value.kind == ValueKind::Tuple) {
        if (!value.tuple) {
            fail("invalid tuple");
        }
        if (start_index > value.tuple->items.size()) {
            fail("destructuring mismatch");
        }
        items.reserve(value.tuple->items.size() - start_index);
        for (std::size_t i = start_index; i < value.tuple->items.size(); ++i) {
            items.push_back(value.tuple->items[i]);
        }
        return make_list(std::move(items));
    }
    fail("cannot destructure value");
}

Value Runtime::destructure_dict_rest(const Value& value, const std::vector<std::string>& excluded_keys) {
    if (value.kind != ValueKind::Dict) {
        fail("cannot destructure value");
    }
    if (!value.dict) {
        fail("invalid dictionary");
    }
    std::vector<std::pair<std::string, Value>> entries;
    for (const auto& entry : value.dict->entries) {
        bool excluded = false;
        for (const std::string& key : excluded_keys) {
            if (entry.first == key) {
                excluded = true;
                break;
            }
        }
        if (!excluded) {
            entries.emplace_back(entry.first, entry.second);
        }
    }
    return make_dict(std::move(entries));
}

void Runtime::set_index(const Value& object, const Value& index, Value value) {
    if (object.kind == ValueKind::Bytes) {
        fail("bytes object does not support item assignment");
    }
    if (object.kind == ValueKind::List) {
        if (!object.list) {
            fail("invalid list");
        }
        if (index.kind != ValueKind::Integer) {
            fail("list index must be an integer");
        }
        if (index.integer < 0 || static_cast<std::uint64_t>(index.integer) >= object.list->items.size()) {
            fail("list index out of range");
        }
        object.list->items[static_cast<std::size_t>(index.integer)] = std::move(value);
        return;
    }
    if (object.kind == ValueKind::Tuple) {
        fail("tuple is immutable");
    }
    if (object.kind == ValueKind::Set) {
        fail("cannot index set");
    }
    if (object.kind == ValueKind::Dict) {
        if (!object.dict) {
            fail("invalid dictionary");
        }
        if (index.kind != ValueKind::String) {
            fail("dictionary key must be a string");
        }
        dict_emplace(*object.dict, index.string, std::move(value));
        return;
    }
    if (index.kind == ValueKind::String) {
        fail("not a dictionary");
    }
    fail("not a list");
}

const char* Runtime::kind_name(ValueKind kind) {
    switch (kind) {
        case ValueKind::Integer:
            return "int";
        case ValueKind::Float:
            return "float";
        case ValueKind::String:
            return "string";
        case ValueKind::Bool:
            return "bool";
        case ValueKind::Null:
            return "null";
        case ValueKind::Bytes:
            return "bytes";
        case ValueKind::List:
            return "list";
        case ValueKind::Dict:
            return "dict";
        case ValueKind::Tuple:
            return "tuple";
        case ValueKind::Set:
            return "set";
        case ValueKind::Struct:
            return "struct";
        case ValueKind::Class:
            return "class";
        case ValueKind::Enum:
            return "enum";
        case ValueKind::Range:
            return "range";
        case ValueKind::Iterator:
            return "iterator";
        case ValueKind::Function:
            return "function";
        case ValueKind::Module:
            return "module";
        case ValueKind::Task:
            return "task";
    }
    return "unknown";
}

bool Runtime::strings_equal(const std::string& left, const std::string& right) {
    if (&left == &right) {
        return true;
    }
    const std::size_t size = left.size();
    if (size != right.size()) {
        return false;
    }
    if (size == 0) {
        return true;
    }
    const char* const left_data = left.data();
    const char* const right_data = right.data();
    if (left_data == right_data) {
        return true;
    }
    return std::memcmp(left_data, right_data, size) == 0;
}

bool Runtime::equals(const Value& left, const Value& right) {
    if (&left == &right) {
        return true;
    }
    if ((left.kind == ValueKind::Integer || left.kind == ValueKind::Float) &&
        (right.kind == ValueKind::Integer || right.kind == ValueKind::Float)) {
        const double a = left.kind == ValueKind::Integer ? static_cast<double>(left.integer) : left.floating;
        const double b = right.kind == ValueKind::Integer ? static_cast<double>(right.integer) : right.floating;
        return a == b;
    }
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
        case ValueKind::String:
            return strings_equal(left.string, right.string);
        case ValueKind::Bool:
            return left.boolean == right.boolean;
        case ValueKind::Null:
            return true;
        case ValueKind::Bytes: {
            if (!left.bytes || !right.bytes || left.bytes->data.size() != right.bytes->data.size()) {
                return false;
            }
            return left.bytes->data == right.bytes->data;
        }
        case ValueKind::List:
            return left.list == right.list;
        case ValueKind::Dict:
            return left.dict == right.dict;
        case ValueKind::Tuple: {
            if (!left.tuple || !right.tuple || left.tuple->items.size() != right.tuple->items.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.tuple->items.size(); ++i) {
                if (!equals(left.tuple->items[i], right.tuple->items[i])) {
                    return false;
                }
            }
            return true;
        }
        case ValueKind::Set:
            return left.set == right.set;
        case ValueKind::Struct: {
            if (!left.struct_value || !right.struct_value) {
                return false;
            }
            if (left.struct_value->type_index != right.struct_value->type_index) {
                return false;
            }
            if (left.struct_value->fields.size() != right.struct_value->fields.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.struct_value->fields.size(); ++i) {
                if (!equals(left.struct_value->fields[i], right.struct_value->fields[i])) {
                    return false;
                }
            }
            return true;
        }
        case ValueKind::Class: {
            if (!left.class_value || !right.class_value) {
                return false;
            }
            if (left.class_value->native_datetime && right.class_value->native_datetime) {
                return left.class_value->native_datetime->utc_micros == right.class_value->native_datetime->utc_micros;
            }
            if (left.class_value->native_duration && right.class_value->native_duration) {
                return left.class_value->native_duration->micros == right.class_value->native_duration->micros;
            }
            if (left.class_value->type_index != right.class_value->type_index) {
                return false;
            }
            if (left.class_value->fields.size() != right.class_value->fields.size()) {
                return false;
            }
            for (std::size_t i = 0; i < left.class_value->fields.size(); ++i) {
                if (!equals(left.class_value->fields[i], right.class_value->fields[i])) {
                    return false;
                }
            }
            return true;
        }
        case ValueKind::Enum:
            return left.enum_value && right.enum_value &&
                   left.enum_value->enum_index == right.enum_value->enum_index &&
                   left.enum_value->member_index == right.enum_value->member_index &&
                   equals(left.enum_value->payload, right.enum_value->payload);
        case ValueKind::Range:
            return left.range && right.range && left.range->start == right.range->start &&
                   left.range->stop == right.range->stop && left.range->step == right.range->step;
        case ValueKind::Iterator:
            return left.iterator_data == right.iterator_data;
        case ValueKind::Function:
            return left.function == right.function;
        case ValueKind::Module:
            return left.module_namespace == right.module_namespace;
        case ValueKind::Task:
            return left.task == right.task;
        default:
            return false;
    }
}

Value Runtime::contains(const Value& value, const Value& container) {
    switch (container.kind) {
        case ValueKind::List:
            return list_contains(container, value);
        case ValueKind::Tuple:
            return tuple_contains(container, value);
        case ValueKind::Set:
            return set_contains(container, value);
        case ValueKind::Dict:
            if (value.kind != ValueKind::String) {
                fail("'in' expects a string dictionary key");
            }
            return dict_contains_key(container, value);
        case ValueKind::String:
            if (value.kind != ValueKind::String) {
                fail("'in' expects a string");
            }
            return string_contains(container, value);
        case ValueKind::Range: {
            if (value.kind != ValueKind::Integer) {
                fail("'in' expects an integer range value");
            }
            if (!container.range) {
                fail("invalid range");
            }
            const RangeData& range = *container.range;
            if (range.step == 0) {
                fail("range step cannot be zero");
            }
            if (range.step > 0) {
                if (value.integer < range.start || value.integer >= range.stop) {
                    return make_bool(false);
                }
                return make_bool((value.integer - range.start) % range.step == 0);
            }
            if (value.integer > range.start || value.integer <= range.stop) {
                return make_bool(false);
            }
            return make_bool((range.start - value.integer) % (-range.step) == 0);
        }
        default:
            fail(std::string("cannot use 'in' with ") + kind_name(container.kind));
    }
}

void Runtime::expect_argc(std::string_view method, const std::vector<Value>& args, std::size_t count) {
    if (args.size() == count) {
        return;
    }
    const std::string name(method);
    if (count == 0) {
        fail("'" + name + "' expects no arguments");
    }
    fail("'" + name + "' expects " + std::to_string(count) + (count == 1 ? " argument" : " arguments"));
}

const Value& Runtime::expect_string_arg(std::string_view method, const Value& value, std::string_view what) {
    if (value.kind != ValueKind::String) {
        fail(std::string("'") + std::string(method) + "' expects a string " + std::string(what));
    }
    return value;
}

std::int64_t Runtime::expect_int_arg(std::string_view method, const Value& value, std::string_view what) {
    if (value.kind != ValueKind::Integer) {
        fail(std::string("'") + std::string(method) + "' expects an integer " + std::string(what));
    }
    return value.integer;
}

Value Runtime::call_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    switch (object.kind) {
        case ValueKind::List:
            return call_list_method(object, name, args);
        case ValueKind::Dict:
            return call_dict_method(object, name, args);
        case ValueKind::Tuple:
            return call_tuple_method(object, name, args);
        case ValueKind::Set:
            return call_set_method(object, name, args);
        case ValueKind::String:
            return call_string_method(object, name, args);
        case ValueKind::Bytes:
            if (name == "length") {
                expect_argc(name, args, 0);
                if (!object.bytes) {
                    fail("invalid bytes");
                }
                return make_integer(static_cast<std::int64_t>(object.bytes->data.size()));
            }
            fail(std::string("cannot call method '") + std::string(name) + "' on bytes");
        case ValueKind::Class:
            if (is_native_iterator(object)) {
                if (name == "next") {
                    expect_argc(name, args, 0);
                    return native_iterator_next(object);
                }
                if (name == "iter") {
                    expect_argc(name, args, 0);
                    return native_iterator_iter(object);
                }
                fail(std::string("Iterator has no method '") + std::string(name) + "'");
            }
            fail(std::string("cannot call method '") + std::string(name) + "' on class");
        case ValueKind::Iterator:
            if (name == "next") {
                expect_argc(name, args, 0);
                return native_iterator_next(object);
            }
            if (name == "iter") {
                expect_argc(name, args, 0);
                return native_iterator_iter(object);
            }
            fail(std::string("Iterator has no method '") + std::string(name) + "'");
        default:
            fail(std::string("cannot call method '") + std::string(name) + "' on " + kind_name(object.kind));
    }
}

Value Runtime::call_list_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    if (name == "append") {
        expect_argc(name, args, 1);
        return list_append(object, args[0]);
    }
    if (name == "prepend") {
        expect_argc(name, args, 1);
        return list_prepend(object, args[0]);
    }
    if (name == "first") {
        expect_argc(name, args, 0);
        return list_first(object);
    }
    if (name == "last") {
        expect_argc(name, args, 0);
        return list_last(object);
    }
    if (name == "find") {
        expect_argc(name, args, 1);
        return list_find(object, args[0]);
    }
    if (name == "contains") {
        expect_argc(name, args, 1);
        return list_contains(object, args[0]);
    }
    if (name == "count") {
        expect_argc(name, args, 1);
        return list_count(object, args[0]);
    }
    if (name == "remove") {
        expect_argc(name, args, 1);
        return list_remove(object, args[0]);
    }
    if (name == "remove_at") {
        expect_argc(name, args, 1);
        return list_remove_at(object, args[0]);
    }
    if (name == "clear") {
        expect_argc(name, args, 0);
        return list_clear(object);
    }
    if (name == "is_empty") {
        expect_argc(name, args, 0);
        return list_is_empty(object);
    }
    if (name == "length") {
        expect_argc(name, args, 0);
        return list_length(object);
    }
    if (name == "iter") {
        expect_argc(name, args, 0);
        return runtime_get_iter(object);
    }
    if (name == "sort") {
        expect_argc(name, args, 0);
        return list_sort(object);
    }
    if (name == "reverse") {
        expect_argc(name, args, 0);
        return list_reverse(object);
    }
    fail("unknown list method '" + std::string(name) + "'");
}

Value Runtime::call_dict_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    if (name == "get") {
        expect_argc(name, args, 1);
        return dict_get(object, args[0]);
    }
    if (name == "contains_key") {
        expect_argc(name, args, 1);
        return dict_contains_key(object, args[0]);
    }
    if (name == "remove") {
        expect_argc(name, args, 1);
        return dict_remove(object, args[0]);
    }
    if (name == "keys") {
        expect_argc(name, args, 0);
        return dict_keys(object);
    }
    if (name == "values") {
        expect_argc(name, args, 0);
        return dict_values(object);
    }
    if (name == "clear") {
        expect_argc(name, args, 0);
        return dict_clear(object);
    }
    if (name == "is_empty") {
        expect_argc(name, args, 0);
        return dict_is_empty(object);
    }
    if (name == "length") {
        expect_argc(name, args, 0);
        return dict_length(object);
    }
    if (name == "iter") {
        expect_argc(name, args, 0);
        return runtime_get_iter(object);
    }
    fail("unknown dictionary method '" + std::string(name) + "'");
}

Value Runtime::call_tuple_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    if (name == "first") {
        expect_argc(name, args, 0);
        return tuple_first(object);
    }
    if (name == "last") {
        expect_argc(name, args, 0);
        return tuple_last(object);
    }
    if (name == "find") {
        expect_argc(name, args, 1);
        return tuple_find(object, args[0]);
    }
    if (name == "contains") {
        expect_argc(name, args, 1);
        return tuple_contains(object, args[0]);
    }
    if (name == "length") {
        expect_argc(name, args, 0);
        return tuple_length(object);
    }
    if (name == "iter") {
        expect_argc(name, args, 0);
        return runtime_get_iter(object);
    }
    fail("unknown tuple method '" + std::string(name) + "'");
}

Value Runtime::call_set_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    if (name == "add") {
        expect_argc(name, args, 1);
        return set_add(object, args[0]);
    }
    if (name == "remove") {
        expect_argc(name, args, 1);
        return set_remove(object, args[0]);
    }
    if (name == "contains") {
        expect_argc(name, args, 1);
        return set_contains(object, args[0]);
    }
    if (name == "length") {
        expect_argc(name, args, 0);
        return set_length(object);
    }
    if (name == "is_empty") {
        expect_argc(name, args, 0);
        return set_is_empty(object);
    }
    if (name == "clear") {
        expect_argc(name, args, 0);
        return set_clear(object);
    }
    if (name == "union") {
        expect_argc(name, args, 1);
        return set_union(object, args[0]);
    }
    if (name == "intersection") {
        expect_argc(name, args, 1);
        return set_intersection(object, args[0]);
    }
    if (name == "difference") {
        expect_argc(name, args, 1);
        return set_difference(object, args[0]);
    }
    if (name == "iter") {
        expect_argc(name, args, 0);
        return runtime_get_iter(object);
    }
    fail("unknown set method '" + std::string(name) + "'");
}

Value Runtime::call_string_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    if (name == "find") {
        expect_argc(name, args, 1);
        return string_find(object, args[0]);
    }
    if (name == "contains") {
        expect_argc(name, args, 1);
        return string_contains(object, args[0]);
    }
    if (name == "replace") {
        expect_argc(name, args, 2);
        return string_replace(object, args[0], args[1]);
    }
    if (name == "starts_with") {
        expect_argc(name, args, 1);
        return string_starts_with(object, args[0]);
    }
    if (name == "ends_with") {
        expect_argc(name, args, 1);
        return string_ends_with(object, args[0]);
    }
    if (name == "first") {
        expect_argc(name, args, 0);
        return string_first(object);
    }
    if (name == "last") {
        expect_argc(name, args, 0);
        return string_last(object);
    }
    if (name == "is_empty") {
        expect_argc(name, args, 0);
        return string_is_empty(object);
    }
    if (name == "length") {
        expect_argc(name, args, 0);
        return string_length(object);
    }
    if (name == "trim") {
        expect_argc(name, args, 0);
        return string_trim(object);
    }
    if (name == "lower") {
        expect_argc(name, args, 0);
        return string_lower(object);
    }
    if (name == "upper") {
        expect_argc(name, args, 0);
        return string_upper(object);
    }
    if (name == "split") {
        expect_argc(name, args, 1);
        return string_split(object, args[0]);
    }
    fail("unknown string method '" + std::string(name) + "'");
}

namespace {

ListData& require_list(const Value& value, std::string_view method) {
    if (value.kind != ValueKind::List || !value.list) {
        RuntimeError err("'" + std::string(method) + "' requires a list");
        throw err;
    }
    return *value.list;
}

DictData& require_dict(const Value& value, std::string_view method) {
    if (value.kind != ValueKind::Dict || !value.dict) {
        throw RuntimeError("'" + std::string(method) + "' requires a dictionary");
    }
    return *value.dict;
}

TupleData& require_tuple(const Value& value, std::string_view method) {
    if (value.kind != ValueKind::Tuple || !value.tuple) {
        throw RuntimeError("'" + std::string(method) + "' requires a tuple");
    }
    return *value.tuple;
}

SetData& require_set(const Value& value, std::string_view method) {
    if (value.kind != ValueKind::Set || !value.set) {
        throw RuntimeError("'" + std::string(method) + "' requires a set");
    }
    return *value.set;
}

bool set_has(const SetData& data, const Value& value) {
    if (!data.buckets.empty()) {
        return set_contains_indexed(data, value);
    }
    for (const auto& item : data.items) {
        if (Runtime::equals(item, value)) {
            return true;
        }
    }
    return false;
}

const std::string& require_string(const Value& value, std::string_view method) {
    if (value.kind != ValueKind::String) {
        throw RuntimeError("'" + std::string(method) + "' requires a string");
    }
    return value.string;
}

double as_sort_number(const Value& value) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    return value.floating;
}

}  // namespace

Value Runtime::list_append(const Value& list, Value value) {
    auto& items = require_list(list, "append").items;
    if (items.size() == items.capacity()) {
        const std::size_t next = items.capacity() == 0 ? 8 : items.capacity() * 2;
        alloc_guard::throw_if_blocked(next);
        items.reserve(next);
    }
    items.push_back(std::move(value));
    return make_null();
}

Value Runtime::list_prepend(const Value& list, Value value) {
    auto& items = require_list(list, "prepend").items;
    items.insert(items.begin(), std::move(value));
    return make_null();
}

Value Runtime::list_first(const Value& list) {
    const auto& items = require_list(list, "first").items;
    if (items.empty()) {
        fail("list is empty");
    }
    return items.front();
}

Value Runtime::list_last(const Value& list) {
    const auto& items = require_list(list, "last").items;
    if (items.empty()) {
        fail("list is empty");
    }
    return items.back();
}

Value Runtime::list_find(const Value& list, const Value& value) {
    const auto& items = require_list(list, "find").items;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (equals(items[i], value)) {
            return make_integer(static_cast<std::int64_t>(i));
        }
    }
    return make_integer(-1);
}

Value Runtime::list_contains(const Value& list, const Value& value) {
    return make_bool(list_find(list, value).integer >= 0);
}

Value Runtime::list_count(const Value& list, const Value& value) {
    const auto& items = require_list(list, "count").items;
    std::int64_t n = 0;
    for (const auto& item : items) {
        if (equals(item, value)) {
            n += 1;
        }
    }
    return make_integer(n);
}

Value Runtime::list_remove(const Value& list, const Value& value) {
    auto& items = require_list(list, "remove").items;
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (equals(*it, value)) {
            items.erase(it);
            return make_null();
        }
    }
    fail("value not found");
}

Value Runtime::list_remove_at(const Value& list, const Value& index) {
    auto& items = require_list(list, "remove_at").items;
    const std::int64_t i = expect_int_arg("remove_at", index, "index");
    if (i < 0 || static_cast<std::uint64_t>(i) >= items.size()) {
        fail("list index out of range");
    }
    items.erase(items.begin() + static_cast<std::ptrdiff_t>(i));
    return make_null();
}

Value Runtime::list_clear(const Value& list) {
    require_list(list, "clear").items.clear();
    return make_null();
}

Value Runtime::list_is_empty(const Value& list) {
    return make_bool(require_list(list, "is_empty").items.empty());
}

Value Runtime::list_length(const Value& list) {
    return make_integer(static_cast<std::int64_t>(require_list(list, "length").items.size()));
}

Value Runtime::tuple_first(const Value& tuple) {
    const auto& items = require_tuple(tuple, "first").items;
    if (items.empty()) {
        fail("tuple is empty");
    }
    return items.front();
}

Value Runtime::tuple_last(const Value& tuple) {
    const auto& items = require_tuple(tuple, "last").items;
    if (items.empty()) {
        fail("tuple is empty");
    }
    return items.back();
}

Value Runtime::tuple_find(const Value& tuple, const Value& value) {
    const auto& items = require_tuple(tuple, "find").items;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (equals(items[i], value)) {
            return make_integer(static_cast<std::int64_t>(i));
        }
    }
    return make_integer(-1);
}

Value Runtime::tuple_contains(const Value& tuple, const Value& value) {
    return make_bool(tuple_find(tuple, value).integer >= 0);
}

Value Runtime::tuple_length(const Value& tuple) {
    return make_integer(static_cast<std::int64_t>(require_tuple(tuple, "length").items.size()));
}

Value Runtime::set_add(const Value& set, Value value) {
    SetData& data = require_set(set, "add");
    if (set_has(data, value)) {
        return make_null();
    }
    if (data.items.size() == data.items.capacity()) {
        const std::size_t next = data.items.capacity() == 0 ? 8 : data.items.capacity() * 2;
        alloc_guard::throw_if_blocked(next);
        data.items.reserve(next);
    }
    const std::size_t index = data.items.size();
    data.buckets[value_bucket_key(value)].push_back(index);
    data.items.push_back(std::move(value));
    return make_null();
}

Value Runtime::set_remove(const Value& set, const Value& value) {
    SetData& data = require_set(set, "remove");
    for (auto it = data.items.begin(); it != data.items.end(); ++it) {
        if (equals(*it, value)) {
            data.items.erase(it);
            set_rebuild_buckets(data);
            return make_null();
        }
    }
    fail("undefined set value");
}

Value Runtime::set_contains(const Value& set, const Value& value) {
    return make_bool(set_has(require_set(set, "contains"), value));
}

Value Runtime::set_length(const Value& set) {
    return make_integer(static_cast<std::int64_t>(require_set(set, "length").items.size()));
}

Value Runtime::set_is_empty(const Value& set) {
    return make_bool(require_set(set, "is_empty").items.empty());
}

Value Runtime::set_clear(const Value& set) {
    SetData& data = require_set(set, "clear");
    data.items.clear();
    data.buckets.clear();
    return make_null();
}

Value Runtime::set_union(const Value& set, const Value& other) {
    if (other.kind != ValueKind::Set || !other.set) {
        fail("'union' expects a set");
    }
    std::vector<Value> items = require_set(set, "union").items;
    for (const auto& item : other.set->items) {
        bool found = false;
        for (const auto& existing : items) {
            if (equals(existing, item)) {
                found = true;
                break;
            }
        }
        if (!found) {
            items.push_back(item);
        }
    }
    return make_set(std::move(items));
}

Value Runtime::set_intersection(const Value& set, const Value& other) {
    if (other.kind != ValueKind::Set || !other.set) {
        fail("'intersection' expects a set");
    }
    std::vector<Value> items;
    for (const auto& item : require_set(set, "intersection").items) {
        if (set_has(*other.set, item)) {
            items.push_back(item);
        }
    }
    return make_set(std::move(items));
}

Value Runtime::set_difference(const Value& set, const Value& other) {
    if (other.kind != ValueKind::Set || !other.set) {
        fail("'difference' expects a set");
    }
    std::vector<Value> items;
    for (const auto& item : require_set(set, "difference").items) {
        if (!set_has(*other.set, item)) {
            items.push_back(item);
        }
    }
    return make_set(std::move(items));
}

Value Runtime::list_sort(const Value& list) {
    auto& items = require_list(list, "sort").items;
    if (items.empty()) {
        return make_null();
    }
    bool numbers = true;
    bool strings = true;
    for (const auto& item : items) {
        if (item.kind != ValueKind::Integer && item.kind != ValueKind::Float) {
            numbers = false;
        }
        if (item.kind != ValueKind::String) {
            strings = false;
        }
    }
    if (numbers) {
        std::sort(items.begin(), items.end(), [](const Value& a, const Value& b) {
            return as_sort_number(a) < as_sort_number(b);
        });
        return make_null();
    }
    if (strings) {
        std::sort(items.begin(), items.end(), [](const Value& a, const Value& b) { return a.string < b.string; });
        return make_null();
    }
    fail("cannot sort incomparable values");
}

Value Runtime::list_reverse(const Value& list) {
    auto& items = require_list(list, "reverse").items;
    std::reverse(items.begin(), items.end());
    return make_null();
}

const Value* Runtime::dict_lookup(const DictData& dict, std::string_view key) {
    const auto it = dict.key_index.find(key);
    if (it == dict.key_index.end()) {
        return nullptr;
    }
    return &dict.entries[it->second].second;
}

Value* Runtime::dict_lookup_mut(DictData& dict, std::string_view key) {
    const auto it = dict.key_index.find(key);
    if (it == dict.key_index.end()) {
        return nullptr;
    }
    return &dict.entries[it->second].second;
}

Value Runtime::dict_get(const Value& dict, const Value& key) {
    const DictData& data = require_dict(dict, "get");
    const std::string& name = expect_string_arg("get", key, "key").string;
    const Value* found = dict_lookup(data, name);
    if (!found) {
        fail("undefined dictionary key");
    }
    return *found;
}

Value Runtime::dict_contains_key(const Value& dict, const Value& key) {
    const DictData& data = require_dict(dict, "contains_key");
    const std::string& name = expect_string_arg("contains_key", key, "key").string;
    return make_bool(dict_lookup(data, name) != nullptr);
}

Value Runtime::dict_remove(const Value& dict, const Value& key) {
    DictData& data = require_dict(dict, "remove");
    const std::string& name = expect_string_arg("remove", key, "key").string;
    const auto index_it = data.key_index.find(name);
    if (index_it == data.key_index.end()) {
        fail("undefined dictionary key");
    }
    data.entries.erase(data.entries.begin() + static_cast<std::ptrdiff_t>(index_it->second));
    dict_rebuild_index(data);
    return make_null();
}

Value Runtime::dict_keys(const Value& dict) {
    const DictData& data = require_dict(dict, "keys");
    std::vector<Value> keys;
    keys.reserve(data.entries.size());
    for (const auto& entry : data.entries) {
        keys.push_back(make_string(entry.first));
    }
    return make_list(std::move(keys));
}

Value Runtime::dict_values(const Value& dict) {
    const DictData& data = require_dict(dict, "values");
    std::vector<Value> values;
    values.reserve(data.entries.size());
    for (const auto& entry : data.entries) {
        values.push_back(entry.second);
    }
    return make_list(std::move(values));
}

Value Runtime::dict_clear(const Value& dict) {
    DictData& data = require_dict(dict, "clear");
    data.entries.clear();
    data.key_index.clear();
    return make_null();
}

Value Runtime::dict_is_empty(const Value& dict) {
    return make_bool(require_dict(dict, "is_empty").entries.empty());
}

Value Runtime::dict_length(const Value& dict) {
    return make_integer(static_cast<std::int64_t>(require_dict(dict, "length").entries.size()));
}

Value Runtime::string_find(const Value& text, const Value& needle) {
    const std::string& haystack = require_string(text, "find");
    const std::string& n = expect_string_arg("find", needle, "argument").string;
    const auto pos = haystack.find(n);
    if (pos == std::string::npos) {
        return make_integer(-1);
    }
    return make_integer(static_cast<std::int64_t>(pos));
}

Value Runtime::string_contains(const Value& text, const Value& needle) {
    return make_bool(string_find(text, needle).integer >= 0);
}

Value Runtime::string_replace(const Value& text, const Value& from, const Value& to) {
    const std::string& haystack = require_string(text, "replace");
    const std::string& a = expect_string_arg("replace", from, "argument").string;
    const std::string& b = expect_string_arg("replace", to, "argument").string;
    if (a.empty()) {
        fail("invalid replace");
    }
    std::string out;
    std::size_t start = 0;
    for (;;) {
        const auto pos = haystack.find(a, start);
        if (pos == std::string::npos) {
            out.append(haystack, start, std::string::npos);
            break;
        }
        out.append(haystack, start, pos - start);
        out += b;
        start = pos + a.size();
    }
    return make_string(std::move(out));
}

Value Runtime::string_starts_with(const Value& text, const Value& prefix) {
    const std::string& haystack = require_string(text, "starts_with");
    const std::string& p = expect_string_arg("starts_with", prefix, "argument").string;
    if (p.size() > haystack.size()) {
        return make_bool(false);
    }
    return make_bool(haystack.compare(0, p.size(), p) == 0);
}

Value Runtime::string_ends_with(const Value& text, const Value& suffix) {
    const std::string& haystack = require_string(text, "ends_with");
    const std::string& s = expect_string_arg("ends_with", suffix, "argument").string;
    if (s.size() > haystack.size()) {
        return make_bool(false);
    }
    return make_bool(haystack.compare(haystack.size() - s.size(), s.size(), s) == 0);
}

Value Runtime::string_first(const Value& text) {
    const std::string& s = require_string(text, "first");
    if (s.empty()) {
        fail("string is empty");
    }
    return make_string(s.substr(0, 1));
}

Value Runtime::string_last(const Value& text) {
    const std::string& s = require_string(text, "last");
    if (s.empty()) {
        fail("string is empty");
    }
    return make_string(s.substr(s.size() - 1, 1));
}

Value Runtime::string_is_empty(const Value& text) {
    return make_bool(require_string(text, "is_empty").empty());
}

Value Runtime::string_length(const Value& text) {
    return make_integer(static_cast<std::int64_t>(require_string(text, "length").size()));
}

Value Runtime::string_trim(const Value& text) {
    return make_string(trim_copy(require_string(text, "trim")));
}

Value Runtime::string_lower(const Value& text) {
    return make_string(ascii_lower(require_string(text, "lower")));
}

Value Runtime::string_upper(const Value& text) {
    return make_string(ascii_upper(require_string(text, "upper")));
}

Value Runtime::string_split(const Value& text, const Value& separator) {
    const std::string& haystack = require_string(text, "split");
    const std::string& sep = expect_string_arg("split", separator, "separator").string;
    if (sep.empty()) {
        fail("'split' separator cannot be empty");
    }
    auto list = std::make_shared<ListData>();
    if (haystack.empty()) {
        return make_list(list->items);
    }
    std::size_t start = 0;
    while (start <= haystack.size()) {
        const std::size_t pos = haystack.find(sep, start);
        if (pos == std::string::npos) {
            list->items.push_back(make_string(haystack.substr(start)));
            break;
        }
        list->items.push_back(make_string(haystack.substr(start, pos - start)));
        start = pos + sep.size();
    }
    return make_list(std::move(list->items));
}

void Runtime::register_builtin(std::string name, Builtin builtin) {
    if (name.empty() || !builtin) {
        fail("invalid built-in");
    }
    builtins_[std::move(name)] = std::move(builtin);
}

bool Runtime::has_builtin(std::string_view name) const {
    return builtins_.find(std::string(name)) != builtins_.end();
}

Value Runtime::call(std::string_view name, const std::vector<Value>& args) {
    const auto it = builtins_.find(std::string(name));
    if (it == builtins_.end()) {
        fail("unknown built-in '" + std::string(name) + "'");
    }
    return it->second(*this, args);
}

Value Runtime::print(const std::vector<Value>& args) {
    return call("print", args);
}

std::int64_t Runtime::range_length(const RangeData& range) {
    if (range.step == 0) {
        fail("range step cannot be zero");
    }
    if (range.step > 0) {
        if (range.start >= range.stop) {
            return 0;
        }
        const std::uint64_t diff = static_cast<std::uint64_t>(range.stop - range.start);
        const std::uint64_t step = static_cast<std::uint64_t>(range.step);
        return static_cast<std::int64_t>((diff + step - 1) / step);
    }
    if (range.start <= range.stop) {
        return 0;
    }
    const std::uint64_t diff = static_cast<std::uint64_t>(range.start - range.stop);
    const std::uint64_t step = static_cast<std::uint64_t>(-range.step);
    return static_cast<std::int64_t>((diff + step - 1) / step);
}

Value Runtime::iter_len(const Value& collection) {
    if (collection.kind == ValueKind::List) {
        if (!collection.list) {
            fail("invalid list");
        }
        return make_integer(static_cast<std::int64_t>(collection.list->items.size()));
    }
    if (collection.kind == ValueKind::Range) {
        if (!collection.range) {
            fail("invalid range");
        }
        return make_integer(range_length(*collection.range));
    }
    fail(std::string("cannot iterate ") + kind_name(collection.kind));
}

Value Runtime::iter_get(const Value& collection, const Value& index) {
    if (index.kind != ValueKind::Integer) {
        fail("iterator index must be an integer");
    }
    if (collection.kind == ValueKind::List) {
        return get_index(collection, index);
    }
    if (collection.kind == ValueKind::Range) {
        if (!collection.range) {
            fail("invalid range");
        }
        const std::int64_t length = range_length(*collection.range);
        if (index.integer < 0 || index.integer >= length) {
            fail("range index out of range");
        }
        return make_integer(collection.range->start + index.integer * collection.range->step);
    }
    fail(std::string("cannot iterate ") + kind_name(collection.kind));
}

Value Runtime::builtin_range(Runtime&, const std::vector<Value>& args) {
    if (args.empty() || args.size() > 3) {
        fail("'range' expects 1 to 3 arguments");
    }
    for (const Value& arg : args) {
        if (arg.kind != ValueKind::Integer) {
            fail("'range' expects integer arguments");
        }
    }

    std::int64_t start = 0;
    std::int64_t stop = 0;
    std::int64_t step = 1;
    if (args.size() == 1) {
        stop = args[0].integer;
    } else if (args.size() == 2) {
        start = args[0].integer;
        stop = args[1].integer;
    } else {
        start = args[0].integer;
        stop = args[1].integer;
        step = args[2].integer;
    }
    if (step == 0) {
        fail("range step cannot be zero");
    }
    return make_range(start, stop, step);
}

std::ostream& Runtime::output() {
    return *output_;
}

std::ostream& Runtime::error_output() {
    if (error_output_ == nullptr) {
        return std::cerr;
    }
    return *error_output_;
}

bool Runtime::parse_log_level(std::string_view text, LogLevel& out) {
    if (text == "DEBUG") {
        out = LogLevel::Debug;
        return true;
    }
    if (text == "INFO") {
        out = LogLevel::Info;
        return true;
    }
    if (text == "WARNING") {
        out = LogLevel::Warning;
        return true;
    }
    if (text == "ERROR") {
        out = LogLevel::Error;
        return true;
    }
    return false;
}

void Runtime::set_native_call_site(std::string_view file, std::uint32_t line, std::uint32_t column) {
    native_call_site_ = NativeCallSite{std::string(file), line, column};
}

void Runtime::clear_native_call_site() {
    native_call_site_.reset();
}

Value Runtime::builtin_print(Runtime& runtime, const std::vector<Value>& args) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i != 0) {
            runtime.output() << ' ';
        }
        runtime.output() << format(args[i]);
    }
    runtime.output() << '\n';
    return make_null();
}

std::string Runtime::trim_copy(std::string text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(start, end - start);
}

std::string Runtime::ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string Runtime::ascii_upper(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return text;
}

Value Runtime::builtin_input(Runtime& runtime, const std::vector<Value>& args) {
    if (args.size() > 1) {
        fail("'input' expects 0 or 1 argument");
    }
    if (args.size() == 1) {
        if (args[0].kind != ValueKind::String) {
            fail("'input' expects a string prompt");
        }
        runtime.output() << args[0].string;
        runtime.output().flush();
    }
    if (!runtime.input_) {
        fail("end of input");
    }
    std::string line;
    if (!std::getline(*runtime.input_, line)) {
        fail("end of input");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return make_string(std::move(line));
}

Value Runtime::convert_int(const Value& value) {
    switch (value.kind) {
        case ValueKind::Integer:
            return make_integer(value.integer);
        case ValueKind::Float:
            return make_integer(static_cast<std::int64_t>(value.floating));
        case ValueKind::Bool:
            return make_integer(value.boolean ? 1 : 0);
        case ValueKind::String: {
            const std::string text = trim_copy(value.string);
            if (text.empty()) {
                fail("cannot convert string to int");
            }
            try {
                std::size_t idx = 0;
                const long long parsed = std::stoll(text, &idx, 10);
                if (idx != text.size()) {
                    fail("cannot convert string to int");
                }
                return make_integer(static_cast<std::int64_t>(parsed));
            } catch (...) {
                fail("cannot convert string to int");
            }
        }
        default:
            fail(std::string("cannot convert ") + kind_name(value.kind) + " to int");
    }
}

Value Runtime::convert_float(const Value& value) {
    switch (value.kind) {
        case ValueKind::Integer:
            return make_float(static_cast<double>(value.integer));
        case ValueKind::Float:
            return make_float(value.floating);
        case ValueKind::Bool:
            return make_float(value.boolean ? 1.0 : 0.0);
        case ValueKind::String: {
            const std::string text = trim_copy(value.string);
            if (text.empty()) {
                fail("cannot convert string to float");
            }
            try {
                std::size_t idx = 0;
                const double parsed = std::stod(text, &idx);
                if (idx != text.size()) {
                    fail("cannot convert string to float");
                }
                return make_float(parsed);
            } catch (...) {
                fail("cannot convert string to float");
            }
        }
        default:
            fail(std::string("cannot convert ") + kind_name(value.kind) + " to float");
    }
}

Value Runtime::convert_bool(const Value& value) {
    switch (value.kind) {
        case ValueKind::Bool:
            return make_bool(value.boolean);
        case ValueKind::Integer:
            return make_bool(value.integer != 0);
        case ValueKind::Float:
            return make_bool(value.floating != 0.0);
        case ValueKind::Null:
            return make_bool(false);
        case ValueKind::String: {
            const std::string text = ascii_lower(trim_copy(value.string));
            if (text.empty() || text == "false" || text == "0") {
                return make_bool(false);
            }
            if (text == "true" || text == "1") {
                return make_bool(true);
            }
            fail("cannot convert string to bool");
        }
        case ValueKind::List:
        case ValueKind::Dict:
        case ValueKind::Tuple:
        case ValueKind::Set:
        case ValueKind::Range:
        case ValueKind::Iterator:
        case ValueKind::Function:
            return make_bool(is_truthy(value));
    }
    fail(std::string("cannot convert ") + kind_name(value.kind) + " to bool");
}

Value Runtime::builtin_int(Runtime&, const std::vector<Value>& args) {
    expect_argc("int", args, 1);
    return convert_int(args[0]);
}

Value Runtime::builtin_float(Runtime&, const std::vector<Value>& args) {
    expect_argc("float", args, 1);
    return convert_float(args[0]);
}

Value Runtime::builtin_str(Runtime&, const std::vector<Value>& args) {
    expect_argc("str", args, 1);
    return make_string(format(args[0]));
}

Value Runtime::builtin_bool(Runtime&, const std::vector<Value>& args) {
    expect_argc("bool", args, 1);
    return convert_bool(args[0]);
}

std::string Runtime::runtime_type_name(const Value& value) {
    if (value.kind == ValueKind::Bytes) {
        return "bytes";
    }
    if (value.kind == ValueKind::Class && value.class_value) {
        if (value.class_value->native_datetime) {
            return "datetime";
        }
        if (value.class_value->native_duration) {
            return "duration";
        }
    }
    return kind_name(value.kind);
}

Value Runtime::builtin_type(Runtime&, const std::vector<Value>& args) {
    expect_argc("type", args, 1);
    return make_string(runtime_type_name(args[0]));
}

namespace {

double numeric_to_double(const Value& value) {
    if (value.kind == ValueKind::Integer) {
        return static_cast<double>(value.integer);
    }
    if (value.kind == ValueKind::Float) {
        return value.floating;
    }
    throw RuntimeError("numeric function expects a numeric argument", RuntimeErrorCategory::Type);
}

bool all_integer(const std::vector<Value>& values) {
    for (const Value& value : values) {
        if (value.kind != ValueKind::Integer) {
            return false;
        }
    }
    return true;
}

Value numeric_from_double(double value, bool prefer_int) {
    if (prefer_int && std::floor(value) == value &&
        value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        value <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return Runtime::make_integer(static_cast<std::int64_t>(value));
    }
    return Runtime::make_float(value);
}

}  // namespace

Value Runtime::builtin_abs(Runtime&, const std::vector<Value>& args) {
    expect_argc("abs", args, 1);
    if (args[0].kind == ValueKind::Integer) {
        const std::int64_t value = args[0].integer;
        return make_integer(value < 0 ? -value : value);
    }
    if (args[0].kind == ValueKind::Float) {
        return make_float(std::fabs(args[0].floating));
    }
    throw RuntimeError("'abs' expects a numeric argument", RuntimeErrorCategory::Type);
}

Value Runtime::builtin_min(Runtime&, const std::vector<Value>& args) {
    if (args.size() < 2) {
        fail("'min' expects at least 2 arguments");
    }
    double best = numeric_to_double(args[0]);
    for (std::size_t i = 1; i < args.size(); ++i) {
        best = std::min(best, numeric_to_double(args[i]));
    }
    return numeric_from_double(best, all_integer(args));
}

Value Runtime::builtin_max(Runtime&, const std::vector<Value>& args) {
    if (args.size() < 2) {
        fail("'max' expects at least 2 arguments");
    }
    double best = numeric_to_double(args[0]);
    for (std::size_t i = 1; i < args.size(); ++i) {
        best = std::max(best, numeric_to_double(args[i]));
    }
    return numeric_from_double(best, all_integer(args));
}

Value Runtime::builtin_round(Runtime&, const std::vector<Value>& args) {
    expect_argc("round", args, 1);
    if (args[0].kind == ValueKind::Integer) {
        return make_integer(args[0].integer);
    }
    if (args[0].kind == ValueKind::Float) {
        const double value = args[0].floating;
        const double rounded = value >= 0.0 ? std::floor(value + 0.5) : std::ceil(value - 0.5);
        return numeric_from_double(rounded, false);
    }
    throw RuntimeError("'round' expects a numeric argument", RuntimeErrorCategory::Type);
}

Value Runtime::builtin_clamp(Runtime&, const std::vector<Value>& args) {
    expect_argc("clamp", args, 3);
    const double value = numeric_to_double(args[0]);
    const double lower = numeric_to_double(args[1]);
    const double upper = numeric_to_double(args[2]);
    if (lower > upper) {
        throw RuntimeError("clamp bounds are invalid", RuntimeErrorCategory::Value);
    }
    const double clamped = std::min(upper, std::max(lower, value));
    return numeric_from_double(clamped, all_integer(args));
}

void Runtime::set_program_args(std::vector<std::string> args) {
    program_args_ = std::move(args);
}

CliState& Runtime::cli_state() {
    return *cli_state_;
}

const CliState& Runtime::cli_state() const {
    return *cli_state_;
}

void Runtime::set_testing_runner_hooks(TestingRunnerHooks hooks) {
    testing_hooks_ = std::move(hooks);
}

void Runtime::clear_testing_runner_hooks() {
    testing_hooks_ = {};
}

int Runtime::run_tests(std::optional<std::string> filter) {
    if (!testing_hooks_.run_tests) {
        throw RuntimeError("testing runner is not available", RuntimeErrorCategory::Runtime);
    }
    return testing_hooks_.run_tests(std::move(filter));
}

const char* Runtime::version_string() {
    return kPscriptVersion;
}

void Runtime::signal_process_exit(int code) {
    process_exit_requested_ = true;
    process_exit_code_ = code;
}

void Runtime::request_process_exit(int code) {
    signal_process_exit(code);
    throw ProcessExit(code);
}

void Runtime::set_async_scheduler_hooks(AsyncSchedulerHooks hooks) {
    async_hooks_ = std::move(hooks);
    async_io_->set_hooks(async_hooks_);
}

void Runtime::clear_async_scheduler_hooks() {
    async_hooks_ = {};
    async_io_->clear_hooks();
}

AsyncIoService& Runtime::async_io() {
    return *async_io_;
}

const AsyncIoService& Runtime::async_io() const {
    return *async_io_;
}

bool Runtime::async_scheduler_active() const {
    return async_hooks_.scheduler_active && async_hooks_.scheduler_active();
}

void Runtime::set_callable_invoker(CallableInvoker invoker) {
    callable_invoker_ = std::move(invoker);
}

void Runtime::clear_callable_invoker() {
    callable_invoker_ = {};
    std::lock_guard lock(callable_mutex_);
    pending_callables_.clear();
}

void Runtime::set_parallel_thread_invoker_factory(ParallelThreadInvokerFactory factory) {
    parallel_thread_invoker_factory_ = std::move(factory);
}

void Runtime::clear_parallel_thread_invoker_factory() {
    parallel_thread_invoker_factory_ = {};
}

bool Runtime::has_parallel_thread_invoker_factory() const {
    return static_cast<bool>(parallel_thread_invoker_factory_);
}

Runtime::CallableInvoker Runtime::create_parallel_thread_invoker() {
    if (!parallel_thread_invoker_factory_) {
        throw RuntimeError("parallel worker invoker is not available", RuntimeErrorCategory::Runtime);
    }
    return parallel_thread_invoker_factory_();
}

Value Runtime::invoke_callable(const Value& callable, const std::vector<Value>& args) {
    if (!callable_invoker_) {
        throw RuntimeError("callable invocation is not available", RuntimeErrorCategory::Runtime);
    }
    return callable_invoker_(callable, args);
}

void Runtime::queue_callable_invoke(const Value& callable, std::vector<Value> args,
                                    const std::shared_ptr<std::promise<Value>>& promise) {
    if (!async_scheduler_active() && callable_invoker_) {
        try {
            promise->set_value(callable_invoker_(callable, args));
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
        return;
    }
    PendingCallableInvoke pending;
    pending.callable = callable;
    pending.args = std::move(args);
    pending.promise = promise;
    {
        std::lock_guard lock(callable_mutex_);
        pending_callables_.push_back(std::move(pending));
    }
    async_io_->wake_waiters();
}

bool Runtime::has_pending_callable_invokes() const {
    std::lock_guard lock(callable_mutex_);
    return !pending_callables_.empty();
}

void Runtime::drain_callable_invokes() {
    if (!callable_invoker_) {
        return;
    }
    std::vector<PendingCallableInvoke> pending;
    {
        std::lock_guard lock(callable_mutex_);
        pending.swap(pending_callables_);
    }
    for (PendingCallableInvoke& item : pending) {
        try {
            item.promise->set_value(callable_invoker_(item.callable, item.args));
        } catch (...) {
            item.promise->set_exception(std::current_exception());
        }
    }
}

[[noreturn]] void Runtime::fail(const std::string& message) {
    throw RuntimeError(message);
}

}  // namespace pscript
