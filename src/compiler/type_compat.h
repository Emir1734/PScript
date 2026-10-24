#pragma once

#include "compiler/types.h"

#include <cstdint>
#include <functional>

namespace pscript {

struct AssignabilityResult {
    bool ok = false;
    bool runtime_check = false;
    bool coerce_int_to_float = false;
};

using SubclassPredicate = std::function<bool(std::uint32_t, std::uint32_t)>;

AssignabilityResult is_assignable(const TypeDescriptor& target, const TypeDescriptor& source,
                                  const SubclassPredicate& is_subclass = nullptr);

bool types_exactly_equal(const TypeDescriptor& left, const TypeDescriptor& right);

const char* static_type_name(StaticType type);

bool is_numeric_type(StaticType type);

}  // namespace pscript
