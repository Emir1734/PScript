#include "lexer/keywords.h"

#include <algorithm>

namespace pscript {
const std::vector<std::string_view>& language_keywords() {
    static const std::vector<std::string_view> kKeywords = {
        "and",       "as",        "async",     "await",     "break",     "catch",     "class",
        "const",     "constructor", "continue", "defer",     "else",      "elif",      "elseif",    "enum",
        "false",     "finally",   "for",       "from",      "func",      "if",        "import",
        "in",        "input",     "interface", "match",     "not",       "null",      "or",
        "parent",    "pass",      "priv",      "private",   "prot",      "protected", "protocol",
        "pub",       "public",    "record",    "return",    "struct",    "super",     "this",
        "throw",     "true",      "try",       "while",     "with",
    };
    return kKeywords;
}

const std::vector<std::string_view>& language_builtins() {
    static const std::vector<std::string_view> kBuiltins = {
        "abs",   "annotations", "async_input", "bool",  "clamp", "delay",   "float", "input", "int",
        "max",   "members",     "min",         "print", "race",  "range",   "round", "str",   "timeout",
        "type",  "type_of",     "wait_all",    "wait_any",    "all",         "filter",      "map",
        "parallel_map",
        "sort_by", "group_by",  "take",
    };
    return kBuiltins;
}

const std::vector<std::string_view>& language_whitespace_call_builtins() {
    static const std::vector<std::string_view> kWhitelist = {
        "abs", "bool", "clamp", "float", "int", "max", "min", "round", "str",
    };
    return kWhitelist;
}

bool is_whitespace_call_builtin(std::string_view name) {
    for (const std::string_view entry : language_whitespace_call_builtins()) {
        if (entry == name) {
            return true;
        }
    }
    return false;
}

const std::vector<std::string_view>& language_primitive_types() {
    static const std::vector<std::string_view> kTypes = {
        "any", "bool", "float", "int", "list", "map", "set", "string", "task", "tuple",
    };
    return kTypes;
}

const std::vector<std::string_view>& language_public_completion_identifiers() {
    static const std::vector<std::string_view>* cached = [] {
        std::vector<std::string_view> merged;
        auto append_unique = [&](const std::vector<std::string_view>& items) {
            for (std::string_view item : items) {
                if (std::find(merged.begin(), merged.end(), item) == merged.end()) {
                    merged.push_back(item);
                }
            }
        };
        append_unique(language_keywords());
        append_unique(language_builtins());
        append_unique(language_primitive_types());
        return new std::vector<std::string_view>(std::move(merged));
    }();
    return *cached;
}

}  // namespace pscript
