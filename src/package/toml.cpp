#include "package/toml.h"

#include <cctype>
#include <sstream>

namespace pscript::package {
namespace {

void trim(std::string_view& text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::optional<std::string> parse_quoted_string(std::string_view text, std::size_t* index) {
    if (*index >= text.size() || text[*index] != '"') {
        return std::nullopt;
    }
    ++(*index);
    std::string out;
    while (*index < text.size()) {
        const char ch = text[(*index)++];
        if (ch == '"') {
            return out;
        }
        if (ch == '\\' && *index < text.size()) {
            out.push_back(text[(*index)++]);
            continue;
        }
        out.push_back(ch);
    }
    return std::nullopt;
}

std::optional<std::string> parse_inline_table_value(std::string_view text, std::size_t* index) {
    while (*index < text.size() && (text[*index] == ' ' || text[*index] == '\t')) {
        ++(*index);
    }
    if (*index < text.size() && text[*index] == '"') {
        return parse_quoted_string(text, index);
    }
    const std::size_t start = *index;
    while (*index < text.size() && text[*index] != ',' && text[*index] != '}') {
        ++(*index);
    }
    std::string_view token = text.substr(start, *index - start);
    trim(token);
    if (token.empty()) {
        return std::nullopt;
    }
    return std::string(token);
}

bool parse_inline_table(std::string_view text, TomlValue& out) {
    out.kind = TomlValue::Kind::Table;
    std::size_t index = 0;
    if (index >= text.size() || text[index] != '{') {
        return false;
    }
    ++index;
    while (index < text.size()) {
        while (index < text.size() && (text[index] == ' ' || text[index] == '\t' || text[index] == ',')) {
            ++index;
        }
        if (index < text.size() && text[index] == '}') {
            return true;
        }
        const std::size_t key_start = index;
        while (index < text.size() && text[index] != '=' && text[index] != '}') {
            ++index;
        }
        std::string_view key = text.substr(key_start, index - key_start);
        trim(key);
        if (key.empty() || index >= text.size() || text[index] != '=') {
            return false;
        }
        ++index;
        const auto value = parse_inline_table_value(text, &index);
        if (!value) {
            return false;
        }
        TomlValue entry;
        entry.kind = TomlValue::Kind::String;
        entry.string_value = *value;
        out.table[std::string(key)] = std::move(entry);
    }
    return false;
}

std::optional<std::string> parse_value_token(std::string_view line, std::size_t eq) {
    std::string_view value = line.substr(eq + 1);
    trim(value);
    if (value.empty()) {
        return std::nullopt;
    }
    if (value.front() == '"') {
        std::size_t index = 0;
        return parse_quoted_string(value, &index);
    }
    if (value.front() == '{') {
        return std::string(value);
    }
    return std::string(value);
}

}  // namespace

std::optional<TomlDocument> parse_toml(std::string_view source, std::string* error) {
    TomlDocument document;
    document.root.kind = TomlValue::Kind::Table;
    TomlValue* current = &document.root;

    std::size_t line_start = 0;
    while (line_start <= source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        if (line_end == std::string_view::npos) {
            line_end = source.size();
        }
        std::string_view line = source.substr(line_start, line_end - line_start);
        trim(line);
        line_start = line_end + 1;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '[') {
            if (line.back() != ']') {
                if (error) {
                    *error = "invalid table header";
                }
                return std::nullopt;
            }
            std::string_view header = line.substr(1, line.size() - 2);
            trim(header);
            TomlValue& table = document.root.table.emplace(std::string(header), TomlValue{}).first->second;
            table.kind = TomlValue::Kind::Table;
            current = &table;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            if (error) {
                *error = "expected key = value";
            }
            return std::nullopt;
        }
        std::string_view key = line.substr(0, eq);
        trim(key);
        if (key.empty()) {
            if (error) {
                *error = "empty key";
            }
            return std::nullopt;
        }
        std::string_view value_view = line.substr(eq + 1);
        trim(value_view);
        TomlValue entry;
        if (!value_view.empty() && value_view.front() == '{') {
            if (!parse_inline_table(value_view, entry)) {
                if (error) {
                    *error = "invalid inline table for key " + std::string(key);
                }
                return std::nullopt;
            }
        } else {
            const auto value = parse_value_token(line, eq);
            if (!value) {
                if (error) {
                    *error = "invalid value for key " + std::string(key);
                }
                return std::nullopt;
            }
            entry.kind = TomlValue::Kind::String;
            entry.string_value = *value;
        }
        current->table[std::string(key)] = std::move(entry);
    }
    return document;
}

std::string escape_toml_string(const std::string& text) {
    std::string out;
    for (char ch : text) {
        if (ch == '\\' || ch == '"') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

void serialize_table_body(std::ostringstream& out, const TomlValue& table, const std::string& prefix) {
    for (const auto& [key, value] : table.table) {
        if (value.kind == TomlValue::Kind::String) {
            out << key << " = \"" << escape_toml_string(value.string_value) << "\"\n";
            continue;
        }
        if (value.kind == TomlValue::Kind::Table) {
            const std::string child_section = prefix.empty() ? key : prefix + "." + key;
            out << '[' << child_section << "]\n";
            serialize_table_body(out, value, child_section);
            out << '\n';
        }
    }
}

std::string serialize_toml(const TomlDocument& document) {
    std::ostringstream out;
    if (document.root.kind != TomlValue::Kind::Table) {
        return out.str();
    }
    for (const auto& [section, table] : document.root.table) {
        if (table.kind != TomlValue::Kind::Table) {
            continue;
        }
        out << '[' << section << "]\n";
        serialize_table_body(out, table, section);
        out << '\n';
    }
    return out.str();
}

const TomlValue* toml_table_lookup(const TomlValue& table, std::string_view key) {
    if (table.kind != TomlValue::Kind::Table) {
        return nullptr;
    }
    const auto found = table.table.find(std::string(key));
    if (found == table.table.end()) {
        return nullptr;
    }
    return &found->second;
}

std::optional<std::string> toml_get_string(const TomlValue& table, std::string_view key) {
    const TomlValue* value = toml_table_lookup(table, key);
    if (value == nullptr || value->kind != TomlValue::Kind::String) {
        return std::nullopt;
    }
    return value->string_value;
}

}  // namespace pscript::package
