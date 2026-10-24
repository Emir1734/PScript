#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pscript {

enum class StaticType {
    Integer,
    Float,
    Bool,
    String,
    Null,
    List,
    Dict,
    Tuple,
    Set,
    Struct,
    Class,
    Enum,
    Any,
    Task,
    Union,
    TypeParam,
    Unknown,
};

struct TypeDescriptor {
    StaticType kind = StaticType::Unknown;
    std::optional<std::uint32_t> struct_index;
    std::optional<std::uint32_t> class_index;
    std::optional<std::uint32_t> enum_index;
    std::optional<std::string> type_param_name;
    std::vector<TypeDescriptor> type_args;
};

TypeDescriptor normalize_union_type(std::vector<TypeDescriptor>&& members);
bool is_union_member(const TypeDescriptor& union_type, const TypeDescriptor& member);

}  // namespace pscript
