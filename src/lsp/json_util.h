#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pscript::lsp {

std::string json_escape(std::string_view text);

class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool bool_value = false;
    double number_value = 0;
    std::string string_value;
    std::vector<JsonValue> array_value;
    std::unordered_map<std::string, JsonValue> object_value;

    static JsonValue make_null();
    static JsonValue make_bool(bool value);
    static JsonValue make_number(double value);
    static JsonValue make_string(std::string value);
    static JsonValue make_array(std::vector<JsonValue> items);
    static JsonValue make_object(std::unordered_map<std::string, JsonValue> fields);

    std::optional<const JsonValue*> get(std::string_view key) const;
    std::string stringify() const;
};

std::optional<JsonValue> parse_json(std::string_view text);

}  // namespace pscript::lsp
