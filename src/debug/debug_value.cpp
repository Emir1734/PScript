#include "debug/debug_value.h"

#include <sstream>

namespace pscript::debug {
namespace {

std::string truncate_string(std::string_view text, std::size_t max_len) {
    if (text.size() <= max_len) {
        return std::string(text);
    }
    return std::string(text.substr(0, max_len)) + "...";
}

}  // namespace

ValueInspector::ValueInspector(std::uint32_t max_depth, std::uint32_t max_children)
    : max_depth_(max_depth), max_children_(max_children) {}

DebugValue ValueInspector::inspect(const Value& value) const {
    next_id_ = 1;
    object_store_.clear();
    visit_stack_.clear();
    return inspect_value(value, 0);
}

const Value* ValueInspector::stored_value(std::uint32_t child_id) const {
    for (const auto& entry : object_store_) {
        if (entry.first == child_id) {
            return &entry.second;
        }
    }
    return nullptr;
}

DebugValueKind ValueInspector::classify(const Value& value) const {
    switch (value.kind) {
        case ValueKind::Integer:
            return DebugValueKind::Integer;
        case ValueKind::Float:
            return DebugValueKind::Float;
        case ValueKind::Bool:
            return DebugValueKind::Bool;
        case ValueKind::String:
            return DebugValueKind::String;
        case ValueKind::Null:
            return DebugValueKind::Null;
        case ValueKind::List:
            return DebugValueKind::List;
        case ValueKind::Tuple:
            return DebugValueKind::Tuple;
        case ValueKind::Dict:
            return DebugValueKind::Dict;
        case ValueKind::Set:
            return DebugValueKind::Set;
        case ValueKind::Bytes:
            return DebugValueKind::Bytes;
        case ValueKind::Struct:
            return DebugValueKind::Struct;
        case ValueKind::Enum:
            return DebugValueKind::Enum;
        case ValueKind::Class:
            return DebugValueKind::Object;
        case ValueKind::Function:
            return DebugValueKind::Function;
        case ValueKind::Module:
            return DebugValueKind::Module;
        case ValueKind::Task:
            return DebugValueKind::Task;
        default:
            return DebugValueKind::Other;
    }
}

std::string ValueInspector::value_summary(const Value& value) const {
    switch (value.kind) {
        case ValueKind::Integer:
            return std::to_string(value.integer);
        case ValueKind::Float: {
            std::ostringstream out;
            out << value.floating;
            return out.str();
        }
        case ValueKind::Bool:
            return value.boolean ? "true" : "false";
        case ValueKind::String:
            return "\"" + truncate_string(value.string, kDefaultMaxString) + "\"";
        case ValueKind::Null:
            return "null";
        case ValueKind::List:
            return value.list ? "list(len=" + std::to_string(value.list->items.size()) + ")" : "list";
        case ValueKind::Tuple:
            return value.tuple ? "tuple(len=" + std::to_string(value.tuple->items.size()) + ")" : "tuple";
        case ValueKind::Dict:
            return value.dict ? "dict(len=" + std::to_string(value.dict->entries.size()) + ")" : "dict";
        case ValueKind::Set:
            return value.set ? "set(len=" + std::to_string(value.set->items.size()) + ")" : "set";
        case ValueKind::Bytes:
            return value.bytes ? "bytes(len=" + std::to_string(value.bytes->data.size()) + ")" : "bytes";
        case ValueKind::Struct:
            return value.struct_value ? value.struct_value->type_name : "struct";
        case ValueKind::Class:
            return value.class_value ? value.class_value->type_name : "class";
        case ValueKind::Enum:
            return value.enum_value ? value.enum_value->enum_name + "." + value.enum_value->member_name : "enum";
        case ValueKind::Function:
            return value.function && !value.function->name.empty() ? value.function->name : "<function>";
        default:
            return Runtime::format(value);
    }
}

DebugValue ValueInspector::inspect_value(const Value& value, std::uint32_t depth) const {
    DebugValue out;
    out.kind = classify(value);
    out.type_name = out.summary;
    out.summary = value_summary(value);
    if (depth >= max_depth_) {
        out.truncated = true;
        return out;
    }
    const void* identity = nullptr;
    if (value.kind == ValueKind::List && value.list) {
        identity = value.list.get();
    } else if (value.kind == ValueKind::Dict && value.dict) {
        identity = value.dict.get();
    } else if (value.kind == ValueKind::Class && value.class_value) {
        identity = value.class_value.get();
    }
    if (identity != nullptr) {
        for (const void* seen : visit_stack_) {
            if (seen == identity) {
                out.kind = DebugValueKind::Cycle;
                out.summary = "<cycle>";
                return out;
            }
        }
        visit_stack_.push_back(identity);
    }

    std::uint32_t child_count = 0;
    auto push_child = [&](const std::string& name, const Value& child) {
        if (child_count >= max_children_) {
            out.truncated = true;
            return;
        }
        DebugProperty prop;
        prop.name = name;
        const DebugValue nested = inspect_value(child, depth + 1);
        prop.kind = nested.kind;
        prop.summary = nested.summary;
        prop.expandable = nested.kind == DebugValueKind::List || nested.kind == DebugValueKind::Dict ||
                          nested.kind == DebugValueKind::Object || nested.kind == DebugValueKind::Struct ||
                          !nested.properties.empty();
        if (prop.expandable) {
            prop.child_id = next_id_++;
            object_store_.emplace_back(prop.child_id, child);
        }
        out.properties.push_back(std::move(prop));
        ++child_count;
    };

    if (value.kind == ValueKind::List && value.list) {
        for (std::size_t i = 0; i < value.list->items.size(); ++i) {
            push_child("[" + std::to_string(i) + "]", value.list->items[i]);
        }
    } else if (value.kind == ValueKind::Tuple && value.tuple) {
        for (std::size_t i = 0; i < value.tuple->items.size(); ++i) {
            push_child("[" + std::to_string(i) + "]", value.tuple->items[i]);
        }
    } else if (value.kind == ValueKind::Dict && value.dict) {
        for (const auto& entry : value.dict->entries) {
            push_child(entry.first, entry.second);
        }
    } else if (value.kind == ValueKind::Struct && value.struct_value) {
        for (std::size_t i = 0; i < value.struct_value->fields.size(); ++i) {
            const std::string& name =
                i < value.struct_value->field_names.size() ? value.struct_value->field_names[i]
                                                           : ("field" + std::to_string(i));
            push_child(name, value.struct_value->fields[i]);
        }
    } else if (value.kind == ValueKind::Class && value.class_value) {
        for (std::size_t i = 0; i < value.class_value->fields.size(); ++i) {
            const std::string& name =
                i < value.class_value->field_names.size() ? value.class_value->field_names[i]
                                                          : ("field" + std::to_string(i));
            push_child(name, value.class_value->fields[i]);
        }
    } else if (value.kind == ValueKind::Enum && value.enum_value) {
        if (value.enum_value->payload.kind != ValueKind::Null) {
            push_child("payload", value.enum_value->payload);
        }
    }

    if (identity != nullptr && !visit_stack_.empty()) {
        visit_stack_.pop_back();
    }
    return out;
}

}  // namespace pscript::debug
