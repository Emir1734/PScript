#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {
namespace {

constexpr std::uint32_t kMatchClassIndex = 0;

[[noreturn]] void regex_fail(std::string message) {
    native_value_error(std::move(message));
}

[[noreturn]] void regex_compile_fail(const std::regex_error& error) {
    native_value_error(std::string("invalid regular expression: ") + error.what());
}

std::regex compile_pattern(const std::string& pattern) {
    try {
        return std::regex(pattern, std::regex_constants::ECMAScript);
    } catch (const std::regex_error& error) {
        regex_compile_fail(error);
    }
}

Value make_match(bool found, std::string text, std::int64_t start, std::int64_t end, std::vector<Value> groups) {
    static const std::vector<std::string> field_names = {"found", "text", "start", "end", "groups"};
    Value value = Runtime::make_class(kMatchClassIndex, "Match", field_names);
    value.class_value->fields[0] = Runtime::make_bool(found);
    value.class_value->fields[1] = Runtime::make_string(std::move(text));
    value.class_value->fields[2] = Runtime::make_integer(start);
    value.class_value->fields[3] = Runtime::make_integer(end);
    value.class_value->fields[4] = Runtime::make_list(std::move(groups));
    return value;
}

std::vector<Value> capture_groups(const std::smatch& match) {
    std::vector<Value> groups;
    groups.reserve(match.size() > 0 ? match.size() - 1 : 0);
    for (std::size_t i = 1; i < match.size(); ++i) {
        groups.push_back(Runtime::make_string(match[i].str()));
    }
    return groups;
}

std::int64_t require_replace_count(const Value& value) {
    if (value.kind != ValueKind::Integer) {
        native_type_error("'replace' expects an integer count");
    }
    if (value.integer < 0) {
        regex_fail("'replace' count must be non-negative");
    }
    return value.integer;
}

}  // namespace

Value native_re_is_match(const std::vector<Value>& args) {
    native_require_argc("is_match", args, 2);
    const std::string& pattern = native_require_string(args[0], "is_match", "pattern");
    const std::string& text = native_require_string(args[1], "is_match", "text");
    const std::regex regex = compile_pattern(pattern);
    return Runtime::make_bool(std::regex_match(text, regex));
}

Value native_re_search(const std::vector<Value>& args) {
    native_require_argc("search", args, 2);
    const std::string& pattern = native_require_string(args[0], "search", "pattern");
    const std::string& text = native_require_string(args[1], "search", "text");
    const std::regex regex = compile_pattern(pattern);
    std::smatch match;
    if (!std::regex_search(text, match, regex)) {
        return Runtime::make_null();
    }
    return make_match(true, match.str(), static_cast<std::int64_t>(match.position()),
                      static_cast<std::int64_t>(match.position() + match.length()), capture_groups(match));
}

Value native_re_find_all(const std::vector<Value>& args) {
    native_require_argc("find_all", args, 2);
    const std::string& pattern = native_require_string(args[0], "find_all", "pattern");
    const std::string& text = native_require_string(args[1], "find_all", "text");
    const std::regex regex = compile_pattern(pattern);
    std::vector<Value> matches;
    auto begin = std::sregex_iterator(text.begin(), text.end(), regex);
    const auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::smatch& match = *it;
        matches.push_back(make_match(true, match.str(), static_cast<std::int64_t>(match.position()),
                                     static_cast<std::int64_t>(match.position() + match.length()),
                                     capture_groups(match)));
    }
    return Runtime::make_list(std::move(matches));
}

Value native_re_replace(const std::vector<Value>& args) {
    native_require_min_argc("replace", args, 3);
    if (args.size() > 4) {
        regex_fail("'replace' expects 3 or 4 arguments");
    }
    const std::string& pattern = native_require_string(args[0], "replace", "pattern");
    const std::string& replacement = native_require_string(args[1], "replace", "replacement");
    const std::string& text = native_require_string(args[2], "replace", "text");
    const std::regex regex = compile_pattern(pattern);
    if (args.size() == 3) {
        return Runtime::make_string(std::regex_replace(text, regex, replacement));
    }
    const std::int64_t count = require_replace_count(args[3]);
    if (count == 0) {
        return Runtime::make_string(text);
    }
    std::string result = text;
    std::smatch match;
    for (std::int64_t i = 0; i < count; ++i) {
        if (!std::regex_search(result, match, regex)) {
            break;
        }
        result.replace(static_cast<std::size_t>(match.position()), match.length(), replacement);
    }
    return Runtime::make_string(result);
}

Value native_re_split(const std::vector<Value>& args) {
    native_require_argc("split", args, 2);
    const std::string& pattern = native_require_string(args[0], "split", "pattern");
    const std::string& text = native_require_string(args[1], "split", "text");
    const std::regex regex = compile_pattern(pattern);
    std::vector<Value> parts;
    auto begin = std::sregex_token_iterator(text.begin(), text.end(), regex, -1);
    const auto end = std::sregex_token_iterator();
    for (auto it = begin; it != end; ++it) {
        parts.push_back(Runtime::make_string(*it));
    }
    return Runtime::make_list(std::move(parts));
}

}  // namespace pscript
