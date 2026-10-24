#pragma once

#include <string_view>
#include <vector>

namespace pscript {

const std::vector<std::string_view>& language_keywords();

const std::vector<std::string_view>& language_builtins();
const std::vector<std::string_view>& language_whitespace_call_builtins();
bool is_whitespace_call_builtin(std::string_view name);

/** Primitive and built-in type names used in annotations (completion + docs). */
const std::vector<std::string_view>& language_primitive_types();

/** Union of keywords, builtins, and primitive types for completion regression tests. */
const std::vector<std::string_view>& language_public_completion_identifiers();

}  // namespace pscript
