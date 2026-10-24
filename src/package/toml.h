#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pscript::package {

struct TomlValue {
    enum class Kind { String, Table, Array };
    Kind kind = Kind::String;
    std::string string_value;
    std::map<std::string, TomlValue> table;
    std::vector<TomlValue> array;
};

struct TomlDocument {
    TomlValue root;
};

std::optional<TomlDocument> parse_toml(std::string_view source, std::string* error = nullptr);
std::string serialize_toml(const TomlDocument& document);

const TomlValue* toml_table_lookup(const TomlValue& table, std::string_view key);
std::optional<std::string> toml_get_string(const TomlValue& table, std::string_view key);

}  // namespace pscript::package
