#include "compiler/type_compat.h"

namespace pscript {

const char* static_type_name(StaticType type) {
    switch (type) {
        case StaticType::Integer:
            return "int";
        case StaticType::Float:
            return "float";
        case StaticType::Bool:
            return "bool";
        case StaticType::String:
            return "string";
        case StaticType::Null:
            return "null";
        case StaticType::List:
            return "list";
        case StaticType::Dict:
            return "dict";
        case StaticType::Tuple:
            return "tuple";
        case StaticType::Set:
            return "set";
        case StaticType::Struct:
            return "struct";
        case StaticType::Class:
            return "class";
        case StaticType::Enum:
            return "enum";
        case StaticType::Any:
            return "any";
        case StaticType::Task:
            return "task";
        case StaticType::Union:
            return "union";
        case StaticType::TypeParam:
            return "typeparam";
        case StaticType::Unknown:
            return "unknown";
    }
    return "unknown";
}

bool is_numeric_type(StaticType type) {
    return type == StaticType::Integer || type == StaticType::Float;
}

bool types_exactly_equal(const TypeDescriptor& left, const TypeDescriptor& right) {
    if (left.kind != right.kind) {
        return false;
    }
    if (left.kind == StaticType::TypeParam) {
        return left.type_param_name == right.type_param_name;
    }
    if (left.kind == StaticType::Struct && left.struct_index != right.struct_index) {
        return false;
    }
    if (left.kind == StaticType::Class && left.class_index != right.class_index) {
        return false;
    }
    if (left.kind == StaticType::Enum && left.enum_index != right.enum_index) {
        return false;
    }
    if (left.type_args.size() != right.type_args.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.type_args.size(); ++i) {
        if (!types_exactly_equal(left.type_args[i], right.type_args[i])) {
            return false;
        }
    }
    return true;
}

static bool assignable_to_union(const TypeDescriptor& union_type, const TypeDescriptor& source,
                                const SubclassPredicate& is_subclass) {
    for (const TypeDescriptor& member : union_type.type_args) {
        AssignabilityResult part =
            is_assignable(member, source, is_subclass);
        if (part.ok) {
            return true;
        }
    }
    return false;
}

AssignabilityResult is_assignable(const TypeDescriptor& target, const TypeDescriptor& source,
                                  const SubclassPredicate& is_subclass) {
    AssignabilityResult result;

    if (target.kind == StaticType::Any) {
        result.ok = true;
        return result;
    }

    if (source.kind == StaticType::Unknown) {
        result.ok = true;
        return result;
    }

    if (source.kind == StaticType::Any) {
        result.ok = true;
        result.runtime_check = true;
        return result;
    }

    if (target.kind == StaticType::TypeParam || source.kind == StaticType::TypeParam) {
        result.ok = true;
        return result;
    }

    if (target.kind == StaticType::Union) {
        if (assignable_to_union(target, source, is_subclass)) {
            result.ok = true;
            if (source.kind == StaticType::Any) {
                result.runtime_check = true;
            } else if (source.kind != StaticType::Union) {
                result.runtime_check = true;
            }
        }
        return result;
    }

    if (source.kind == StaticType::Union) {
        for (const TypeDescriptor& member : source.type_args) {
            AssignabilityResult part = is_assignable(target, member, is_subclass);
            if (!part.ok) {
                return result;
            }
        }
        result.ok = true;
        result.runtime_check = true;
        return result;
    }

    if (target.kind == StaticType::Float && source.kind == StaticType::Integer) {
        result.ok = true;
        result.coerce_int_to_float = true;
        return result;
    }

    if (target.kind == StaticType::Struct && source.kind == StaticType::Struct) {
        if (!target.struct_index || !source.struct_index || *target.struct_index == *source.struct_index) {
            result.ok = types_exactly_equal(target, source) ||
                        (!target.struct_index || !source.struct_index || *target.struct_index == *source.struct_index);
            if (target.struct_index && !source.struct_index) {
                result.runtime_check = true;
            }
        }
        return result;
    }

    if (target.kind == StaticType::Enum && source.kind == StaticType::Enum) {
        if (!target.enum_index || !source.enum_index || *target.enum_index == *source.enum_index) {
            result.ok = types_exactly_equal(target, source) ||
                        (!target.enum_index || !source.enum_index || *target.enum_index == *source.enum_index);
            if (target.enum_index && !source.enum_index) {
                result.runtime_check = true;
            }
        }
        return result;
    }

    if (target.kind == StaticType::Class && source.kind == StaticType::Class) {
        if (!target.type_args.empty() || !source.type_args.empty()) {
            if (types_exactly_equal(target, source)) {
                result.ok = true;
            } else if (!target.type_args.empty() && source.type_args.empty() && target.class_index &&
                       source.class_index && *target.class_index == *source.class_index) {
                result.ok = true;
            }
            return result;
        }
        if (target.class_index && source.class_index && is_subclass &&
            is_subclass(*source.class_index, *target.class_index)) {
            result.ok = true;
        } else if (!target.class_index || !source.class_index) {
            result.ok = true;
        }
        return result;
    }

    if (target.kind == StaticType::Task && source.kind == StaticType::Task) {
        result.ok = true;
        if (!target.type_args.empty() && !source.type_args.empty() &&
            !types_exactly_equal(target.type_args[0], source.type_args[0])) {
            result.runtime_check = true;
        } else if (!target.type_args.empty() && source.type_args.empty()) {
            result.runtime_check = true;
        }
        return result;
    }

    if (target.kind != source.kind) {
        return result;
    }

    if (!target.type_args.empty() && source.type_args.empty()) {
        result.ok = true;
        result.runtime_check = true;
        return result;
    }

    if (!target.type_args.empty() && !source.type_args.empty()) {
        if (types_exactly_equal(target, source)) {
            result.ok = true;
        }
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace pscript
