#include "lsp/json_util.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <sstream>

namespace pscript::lsp {
namespace {

bool append_codepoint_utf8(std::string& out, char32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
        return true;
    }
    if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return true;
    }
    if (codepoint <= 0xFFFF) {
        if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            return false;
        }
        out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return true;
    }
    if (codepoint <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return true;
    }
    return false;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return text;
}

class Parser {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    std::optional<JsonValue> parse_value() {
        skip_ws();
        if (eof()) {
            return std::nullopt;
        }
        const char c = peek();
        if (c == '{') {
            return parse_object();
        }
        if (c == '[') {
            return parse_array();
        }
        if (c == '"') {
            return parse_string();
        }
        if (c == 't' || c == 'f') {
            return parse_bool();
        }
        if (c == 'n') {
            return parse_null();
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
            return parse_number();
        }
        return std::nullopt;
    }

private:
    std::string_view input_;
    std::size_t pos_ = 0;

    bool eof() const { return pos_ >= input_.size(); }
    char peek() const { return eof() ? '\0' : input_[pos_]; }
    char get() { return eof() ? '\0' : input_[pos_++]; }

    void skip_ws() {
        while (!eof() && std::isspace(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
    }

    bool consume(char expected) {
        skip_ws();
        if (peek() != expected) {
            return false;
        }
        ++pos_;
        return true;
    }

    std::optional<JsonValue> parse_null() {
        if (input_.compare(pos_, 4, "null") != 0) {
            return std::nullopt;
        }
        pos_ += 4;
        return JsonValue::make_null();
    }

    std::optional<JsonValue> parse_bool() {
        if (input_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return JsonValue::make_bool(true);
        }
        if (input_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return JsonValue::make_bool(false);
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_number() {
        const std::size_t start = pos_;
        if (peek() == '-') {
            ++pos_;
        }
        while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
            ++pos_;
        }
        if (peek() == '.') {
            ++pos_;
            while (!eof() && std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        const std::string_view slice = input_.substr(start, pos_ - start);
        double value = 0;
        std::from_chars(slice.data(), slice.data() + slice.size(), value);
        return JsonValue::make_number(value);
    }

    std::optional<JsonValue> parse_string() {
        if (!consume('"')) {
            return std::nullopt;
        }
        std::string out;
        while (!eof()) {
            const char c = get();
            if (c == '"') {
                return JsonValue::make_string(std::move(out));
            }
            if (c == '\\') {
                if (eof()) {
                    return std::nullopt;
                }
                const char esc = get();
                switch (esc) {
                    case '"':
                        out.push_back('"');
                        break;
                    case '\\':
                        out.push_back('\\');
                        break;
                    case '/':
                        out.push_back('/');
                        break;
                    case 'b':
                        out.push_back('\b');
                        break;
                    case 'f':
                        out.push_back('\f');
                        break;
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'u': {
                        char32_t codepoint = 0;
                        for (int i = 0; i < 4; ++i) {
                            if (eof()) {
                                return std::nullopt;
                            }
                            const int value = hex_value(get());
                            if (value < 0) {
                                return std::nullopt;
                            }
                            codepoint = (codepoint << 4) | static_cast<char32_t>(value);
                        }
                        if (!append_codepoint_utf8(out, codepoint)) {
                            return std::nullopt;
                        }
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            } else {
                out.push_back(c);
            }
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_array() {
        if (!consume('[')) {
            return std::nullopt;
        }
        std::vector<JsonValue> items;
        skip_ws();
        if (consume(']')) {
            return JsonValue::make_array(std::move(items));
        }
        while (true) {
            std::optional<JsonValue> value = parse_value();
            if (!value.has_value()) {
                return std::nullopt;
            }
            items.push_back(std::move(*value));
            skip_ws();
            if (consume(']')) {
                break;
            }
            if (!consume(',')) {
                return std::nullopt;
            }
        }
        return JsonValue::make_array(std::move(items));
    }

    std::optional<JsonValue> parse_object() {
        if (!consume('{')) {
            return std::nullopt;
        }
        std::unordered_map<std::string, JsonValue> fields;
        skip_ws();
        if (consume('}')) {
            return JsonValue::make_object(std::move(fields));
        }
        while (true) {
            std::optional<JsonValue> key_value = parse_string();
            if (!key_value.has_value() || key_value->kind != JsonValue::Kind::String) {
                return std::nullopt;
            }
            if (!consume(':')) {
                return std::nullopt;
            }
            std::optional<JsonValue> value = parse_value();
            if (!value.has_value()) {
                return std::nullopt;
            }
            fields.emplace(std::move(key_value->string_value), std::move(*value));
            skip_ws();
            if (consume('}')) {
                break;
            }
            if (!consume(',')) {
                return std::nullopt;
            }
        }
        return JsonValue::make_object(std::move(fields));
    }
};

}  // namespace

std::string json_escape(std::string_view text) {
    std::ostringstream out;
    for (const char c : text) {
        switch (c) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                out << c;
                break;
        }
    }
    return out.str();
}

JsonValue JsonValue::make_null() { return JsonValue{}; }
JsonValue JsonValue::make_bool(bool value) {
    JsonValue v;
    v.kind = Kind::Bool;
    v.bool_value = value;
    return v;
}
JsonValue JsonValue::make_number(double value) {
    JsonValue v;
    v.kind = Kind::Number;
    v.number_value = value;
    return v;
}
JsonValue JsonValue::make_string(std::string value) {
    JsonValue v;
    v.kind = Kind::String;
    v.string_value = std::move(value);
    return v;
}
JsonValue JsonValue::make_array(std::vector<JsonValue> items) {
    JsonValue v;
    v.kind = Kind::Array;
    v.array_value = std::move(items);
    return v;
}
JsonValue JsonValue::make_object(std::unordered_map<std::string, JsonValue> fields) {
    JsonValue v;
    v.kind = Kind::Object;
    v.object_value = std::move(fields);
    return v;
}

std::optional<const JsonValue*> JsonValue::get(std::string_view key) const {
    if (kind != Kind::Object) {
        return std::nullopt;
    }
    const auto it = object_value.find(std::string(key));
    if (it == object_value.end()) {
        return std::nullopt;
    }
    return &it->second;
}

std::string JsonValue::stringify() const {
    switch (kind) {
        case Kind::Null:
            return "null";
        case Kind::Bool:
            return bool_value ? "true" : "false";
        case Kind::Number: {
            std::ostringstream out;
            out << number_value;
            return out.str();
        }
        case Kind::String:
            return "\"" + json_escape(string_value) + "\"";
        case Kind::Array: {
            std::ostringstream out;
            out << '[';
            for (std::size_t i = 0; i < array_value.size(); ++i) {
                if (i > 0) {
                    out << ',';
                }
                out << array_value[i].stringify();
            }
            out << ']';
            return out.str();
        }
        case Kind::Object: {
            std::ostringstream out;
            out << '{';
            bool first = true;
            for (const auto& [key, value] : object_value) {
                if (!first) {
                    out << ',';
                }
                first = false;
                out << '"' << json_escape(key) << "\":" << value.stringify();
            }
            out << '}';
            return out.str();
        }
    }
    return "null";
}

std::optional<JsonValue> parse_json(std::string_view text) {
    Parser parser(trim(text));
    return parser.parse_value();
}

}  // namespace pscript::lsp
