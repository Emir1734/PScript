#pragma once

#include "runtime/runtime.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pscript::debug {

enum class DebugValueKind : std::uint8_t {
    Integer,
    Float,
    Bool,
    String,
    Null,
    List,
    Tuple,
    Dict,
    Set,
    Bytes,
    Struct,
    Enum,
    Object,
    Function,
    Native,
    Module,
    Iterator,
    Task,
    Other,
    Unavailable,
    Truncated,
    Cycle,
};

struct DebugProperty {
    std::string name;
    DebugValueKind kind = DebugValueKind::Other;
    std::string summary;
    std::uint32_t child_id = 0;
    bool expandable = false;
};

struct DebugValue {
    DebugValueKind kind = DebugValueKind::Other;
    std::string type_name;
    std::string summary;
    std::vector<DebugProperty> properties;
    bool truncated = false;
};

struct ValueInspector {
    static constexpr std::uint32_t kDefaultMaxDepth = 8;
    static constexpr std::uint32_t kDefaultMaxChildren = 64;
    static constexpr std::uint32_t kDefaultMaxString = 4096;

    explicit ValueInspector(std::uint32_t max_depth = kDefaultMaxDepth,
                            std::uint32_t max_children = kDefaultMaxChildren);

    DebugValue inspect(const Value& value) const;
    const Value* stored_value(std::uint32_t child_id) const;

private:
    std::uint32_t max_depth_;
    std::uint32_t max_children_;
    mutable std::uint32_t next_id_ = 1;
    mutable std::vector<std::pair<std::uint32_t, Value>> object_store_;
    mutable std::vector<const void*> visit_stack_;

    DebugValue inspect_value(const Value& value, std::uint32_t depth) const;
    std::string value_summary(const Value& value) const;
    DebugValueKind classify(const Value& value) const;
};

}  // namespace pscript::debug
