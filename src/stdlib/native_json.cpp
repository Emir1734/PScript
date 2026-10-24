#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

#include <charconv>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {
namespace {

[[noreturn]] void json_fail(std::string message) {
    native_value_error(std::move(message));
}

bool json_encodable(const Value& value) {
    switch (value.kind) {
        case ValueKind::Null:
        case ValueKind::Bool:
        case ValueKind::Integer:
        case ValueKind::Float:
        case ValueKind::String:
        case ValueKind::List:
        case ValueKind::Tuple:
        case ValueKind::Dict:
            return true;
        default:
            return false;
    }
}

void append_unsigned(std::string& out, std::uint64_t value) {
    char buffer[24];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) {
        json_fail("invalid JSON number");
    }
    out.append(buffer, result.ptr);
}

void append_integer(std::string& out, std::int64_t value) {
    if (value < 0) {
        out.push_back('-');
        const std::uint64_t magnitude =
            value == std::numeric_limits<std::int64_t>::min()
                ? static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1
                : static_cast<std::uint64_t>(-value);
        append_unsigned(out, magnitude);
        return;
    }
    append_unsigned(out, static_cast<std::uint64_t>(value));
}

void append_float(std::string& out, double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec == std::errc{}) {
        out.append(buffer, result.ptr);
        return;
    }
    std::ostringstream fallback;
    fallback << value;
    out.append(fallback.str());
}

void append_hex4(std::string& out, unsigned int value) {
    constexpr char digits[] = "0123456789abcdef";
    out.push_back(digits[(value >> 12) & 0xF]);
    out.push_back(digits[(value >> 8) & 0xF]);
    out.push_back(digits[(value >> 4) & 0xF]);
    out.push_back(digits[value & 0xF]);
}

void encode_string(std::string& out, std::string_view text) {
    out.reserve(out.size() + text.size() + 2 + (text.size() >> 4));
    out.push_back('"');
    for (unsigned char ch : text) {
        switch (ch) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (ch < 0x20) {
                    out.append("\\u");
                    append_hex4(out, ch);
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    out.push_back('"');
}

void encode_value(const Value& value, std::string& out, int indent, bool pretty);

void encode_array_items(const std::vector<Value>& items, std::string& out, int indent, bool pretty) {
    out.push_back('[');
    if (pretty && !items.empty()) {
        out.push_back('\n');
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (pretty) {
            out.append(static_cast<std::size_t>(indent + 2), ' ');
        }
        encode_value(items[i], out, indent + 2, pretty);
        if (i + 1 < items.size()) {
            out.push_back(',');
        }
        if (pretty) {
            out.push_back('\n');
        }
    }
    if (pretty && !items.empty()) {
        out.append(static_cast<std::size_t>(indent), ' ');
    }
    out.push_back(']');
}

void encode_value(const Value& value, std::string& out, int indent, bool pretty) {
    if (!json_encodable(value)) {
        json_fail("value is not JSON serializable");
    }
    switch (value.kind) {
        case ValueKind::Null:
            out.append("null");
            return;
        case ValueKind::Bool:
            out.append(value.boolean ? "true" : "false");
            return;
        case ValueKind::Integer:
            append_integer(out, value.integer);
            return;
        case ValueKind::Float:
            append_float(out, value.floating);
            return;
        case ValueKind::String:
            encode_string(out, value.string);
            return;
        case ValueKind::List:
            if (!value.list) {
                json_fail("invalid list");
            }
            encode_array_items(value.list->items, out, indent, pretty);
            return;
        case ValueKind::Tuple:
            if (!value.tuple) {
                json_fail("invalid tuple");
            }
            encode_array_items(value.tuple->items, out, indent, pretty);
            return;
        case ValueKind::Dict: {
            if (!value.dict) {
                json_fail("invalid dictionary");
            }
            out.push_back('{');
            if (pretty && !value.dict->entries.empty()) {
                out.push_back('\n');
            }
            for (std::size_t i = 0; i < value.dict->entries.size(); ++i) {
                if (pretty) {
                    out.append(static_cast<std::size_t>(indent + 2), ' ');
                }
                encode_string(out, value.dict->entries[i].first);
                out.push_back(':');
                if (pretty) {
                    out.push_back(' ');
                }
                encode_value(value.dict->entries[i].second, out, indent + 2, pretty);
                if (i + 1 < value.dict->entries.size()) {
                    out.push_back(',');
                }
                if (pretty) {
                    out.push_back('\n');
                }
            }
            if (pretty && !value.dict->entries.empty()) {
                out.append(static_cast<std::size_t>(indent), ' ');
            }
            out.push_back('}');
            return;
        }
        default:
            json_fail("value is not JSON serializable");
    }
}

std::string encode_json(const Value& value, bool pretty) {
    std::string out;
    out.reserve(256);
    encode_value(value, out, 0, pretty);
    return out;
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    Value parse() {
        skip_ws();
        Value value = parse_value();
        skip_ws();
        if (pos_ != text_.size()) {
            json_fail("invalid JSON");
        }
        return value;
    }

private:
    std::string_view text_;
    std::size_t pos_ = 0;

    char peek() const {
        if (pos_ >= text_.size()) {
            return '\0';
        }
        return text_[pos_];
    }

    char get() {
        if (pos_ >= text_.size()) {
            json_fail("invalid JSON");
        }
        return text_[pos_++];
    }

    void skip_ws() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    Value parse_value() {
        skip_ws();
        const char ch = peek();
        if (ch == '{') {
            return parse_object();
        }
        if (ch == '[') {
            return parse_array();
        }
        if (ch == '"') {
            return Runtime::make_string(parse_string());
        }
        if (ch == 't') {
            expect_literal("true");
            return Runtime::make_bool(true);
        }
        if (ch == 'f') {
            expect_literal("false");
            return Runtime::make_bool(false);
        }
        if (ch == 'n') {
            expect_literal("null");
            return Runtime::make_null();
        }
        if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) {
            return parse_number();
        }
        json_fail("invalid JSON");
    }

    void expect_literal(std::string_view literal) {
        for (char expected : literal) {
            if (get() != expected) {
                json_fail("invalid JSON");
            }
        }
    }

    std::string parse_string() {
        if (get() != '"') {
            json_fail("invalid JSON");
        }
        std::string out;
        out.reserve(32);
        while (true) {
            const char ch = get();
            if (ch == '"') {
                return out;
            }
            if (ch == '\\') {
                const char escaped = get();
                switch (escaped) {
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
                        int codepoint = 0;
                        for (int i = 0; i < 4; ++i) {
                            const char digit = get();
                            codepoint <<= 4;
                            if (digit >= '0' && digit <= '9') {
                                codepoint |= digit - '0';
                            } else if (digit >= 'a' && digit <= 'f') {
                                codepoint |= digit - 'a' + 10;
                            } else if (digit >= 'A' && digit <= 'F') {
                                codepoint |= digit - 'A' + 10;
                            } else {
                                json_fail("invalid JSON");
                            }
                        }
                        if (codepoint <= 0x7F) {
                            out.push_back(static_cast<char>(codepoint));
                        } else if (codepoint <= 0x7FF) {
                            out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
                            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
                            out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
                        }
                        break;
                    }
                    default:
                        json_fail("invalid JSON");
                }
                continue;
            }
            out.push_back(ch);
        }
    }

    Value parse_number() {
        const std::size_t start = pos_;
        bool negative = false;
        if (peek() == '-') {
            negative = true;
            ++pos_;
        }
        if (!std::isdigit(static_cast<unsigned char>(peek()))) {
            json_fail("invalid JSON number");
        }
        std::int64_t integer_value = 0;
        bool integer_overflow = false;
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            const int digit = peek() - '0';
            if (integer_value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
                integer_overflow = true;
            }
            integer_value = integer_value * 10 + digit;
            ++pos_;
        }
        bool is_float = false;
        if (peek() == '.') {
            is_float = true;
            ++pos_;
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            ++pos_;
            if (peek() == '+' || peek() == '-') {
                ++pos_;
            }
            while (std::isdigit(static_cast<unsigned char>(peek()))) {
                ++pos_;
            }
        }
        if (!is_float && integer_overflow) {
            const std::string lexeme(text_.substr(start, pos_ - start));
            try {
                return Runtime::make_integer(std::stoll(lexeme));
            } catch (...) {
                json_fail("invalid JSON number");
            }
        }
        if (!is_float) {
            if (negative) {
                if (integer_value > static_cast<std::int64_t>(std::numeric_limits<std::int64_t>::max()) + 1) {
                    json_fail("invalid JSON number");
                }
                if (integer_value == static_cast<std::int64_t>(std::numeric_limits<std::int64_t>::max()) + 1) {
                    return Runtime::make_integer(std::numeric_limits<std::int64_t>::min());
                }
                return Runtime::make_integer(-integer_value);
            }
            return Runtime::make_integer(integer_value);
        }
        const std::string_view lexeme = text_.substr(start, pos_ - start);
        try {
            return Runtime::make_float(std::stod(std::string(lexeme)));
        } catch (...) {
            json_fail("invalid JSON number");
        }
    }

    Value parse_array() {
        if (get() != '[') {
            json_fail("invalid JSON");
        }
        skip_ws();
        std::vector<Value> items;
        items.reserve(16);
        if (peek() == ']') {
            get();
            return Runtime::make_list(std::move(items));
        }
        while (true) {
            items.push_back(parse_value());
            skip_ws();
            if (peek() == ']') {
                get();
                return Runtime::make_list(std::move(items));
            }
            if (get() != ',') {
                json_fail("invalid JSON");
            }
            skip_ws();
        }
    }

    Value parse_object() {
        if (get() != '{') {
            json_fail("invalid JSON");
        }
        skip_ws();
        std::vector<std::pair<std::string, Value>> entries;
        entries.reserve(8);
        if (peek() == '}') {
            get();
            return Runtime::make_dict(std::move(entries));
        }
        while (true) {
            skip_ws();
            if (peek() != '"') {
                json_fail("invalid JSON");
            }
            std::string key = parse_string();
            skip_ws();
            if (get() != ':') {
                json_fail("invalid JSON");
            }
            skip_ws();
            entries.emplace_back(std::move(key), parse_value());
            skip_ws();
            if (peek() == '}') {
                get();
                return Runtime::make_dict(std::move(entries));
            }
            if (get() != ',') {
                json_fail("invalid JSON");
            }
            skip_ws();
        }
    }
};

bool parse_pretty_flag(const std::vector<Value>& args, std::size_t index) {
    if (index >= args.size()) {
        return false;
    }
    if (args[index].kind != ValueKind::Bool) {
        native_type_error("pretty flag must be a bool");
    }
    return args[index].boolean;
}

std::filesystem::path json_path_arg(const std::vector<Value>& args, std::size_t index, std::string_view function) {
    return std::filesystem::path(native_require_string_arg(args, index, function, "path"));
}

std::string read_json_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        native_runtime_error(native_filesystem_message("read", path.generic_string(),
                                                       std::runtime_error("file not found or not readable")));
    }
    const std::streamsize size = in.tellg();
    in.seekg(0, std::ios::beg);
    std::string buffer;
    if (size > 0) {
        buffer.resize(static_cast<std::size_t>(size));
        in.read(buffer.data(), size);
        if (!in) {
            native_runtime_error(native_filesystem_message("read", path.generic_string(),
                                                           std::runtime_error("read failed")));
        }
    }
    return buffer;
}

void write_json_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        native_runtime_error(native_filesystem_message("write", path.generic_string(),
                                                       std::runtime_error("file not writable")));
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
        native_runtime_error(native_filesystem_message("write", path.generic_string(),
                                                       std::runtime_error("write failed")));
    }
}

}  // namespace

Value native_json_encode(const std::vector<Value>& args) {
    native_require_min_argc("encode", args, 1);
    if (args.size() > 2) {
        native_value_error("'encode' expects 1 or 2 arguments");
    }
    const bool pretty = parse_pretty_flag(args, 1);
    return Runtime::make_string(encode_json(args[0], pretty));
}

Value native_json_decode(const std::vector<Value>& args) {
    native_require_argc("decode", args, 1);
    const std::string& text = native_require_string(args[0], "decode", "text");
    JsonParser parser(std::string_view{text});
    return parser.parse();
}

Value native_json_valid(const std::vector<Value>& args) {
    native_require_argc("valid", args, 1);
    const std::string& text = native_require_string(args[0], "valid", "text");
    try {
        JsonParser parser(std::string_view{text});
        (void)parser.parse();
        return Runtime::make_bool(true);
    } catch (const RuntimeError&) {
        return Runtime::make_bool(false);
    }
}

Value native_json_read(const std::vector<Value>& args) {
    native_require_argc("read", args, 1);
    const std::filesystem::path path = json_path_arg(args, 0, "read");
    const std::string file_text = read_json_file(path);
    JsonParser parser(file_text);
    return parser.parse();
}

Value native_json_write(const std::vector<Value>& args) {
    native_require_min_argc("write", args, 2);
    if (args.size() > 3) {
        native_value_error("'write' expects 2 or 3 arguments");
    }
    const std::filesystem::path path = json_path_arg(args, 0, "write");
    const bool pretty = parse_pretty_flag(args, 2);
    write_json_file(path, encode_json(args[1], pretty));
    return Runtime::make_null();
}

}  // namespace pscript
