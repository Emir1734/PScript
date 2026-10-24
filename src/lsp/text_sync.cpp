#include "lsp/text_sync.h"

#include <optional>

namespace pscript::lsp {
namespace {

const JsonValue* object_field(const JsonValue& object, const char* key) {
    const std::optional<const JsonValue*> value = object.get(key);
    return value.has_value() ? *value : nullptr;
}

int json_int(const JsonValue& value) {
    if (value.kind == JsonValue::Kind::Number) {
        return static_cast<int>(value.number_value);
    }
    return 0;
}

int utf16_length(char32_t codepoint) {
    return codepoint > 0xFFFF ? 2 : 1;
}

bool decode_utf8_at(std::string_view text, std::size_t index, char32_t& out, int& nbytes) {
    if (index >= text.size()) {
        return false;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(text.data());
    const unsigned char b0 = data[index];
    if (b0 <= 0x7F) {
        out = b0;
        nbytes = 1;
        return true;
    }
    if ((b0 & 0xE0) == 0xC0) {
        nbytes = 2;
        if (index + 1 >= text.size() || (data[index + 1] & 0xC0) != 0x80) {
            return false;
        }
        out = (static_cast<char32_t>(b0 & 0x1F) << 6) | static_cast<char32_t>(data[index + 1] & 0x3F);
        return out >= 0x80;
    }
    if ((b0 & 0xF0) == 0xE0) {
        nbytes = 3;
        if (index + 2 >= text.size() || (data[index + 1] & 0xC0) != 0x80 || (data[index + 2] & 0xC0) != 0x80) {
            return false;
        }
        out = (static_cast<char32_t>(b0 & 0x0F) << 12) |
              (static_cast<char32_t>(data[index + 1] & 0x3F) << 6) |
              static_cast<char32_t>(data[index + 2] & 0x3F);
        return out >= 0x800 && !(out >= 0xD800 && out <= 0xDFFF);
    }
    if ((b0 & 0xF8) == 0xF0) {
        nbytes = 4;
        if (index + 3 >= text.size() || (data[index + 1] & 0xC0) != 0x80 || (data[index + 2] & 0xC0) != 0x80 ||
            (data[index + 3] & 0xC0) != 0x80) {
            return false;
        }
        out = (static_cast<char32_t>(b0 & 0x07) << 18) | (static_cast<char32_t>(data[index + 1] & 0x3F) << 12) |
              (static_cast<char32_t>(data[index + 2] & 0x3F) << 6) | static_cast<char32_t>(data[index + 3] & 0x3F);
        return out >= 0x10000 && out <= 0x10FFFF;
    }
    return false;
}

std::size_t utf16_to_byte_offset_in_line(std::string_view line, int utf16_offset) {
    if (utf16_offset <= 0) {
        return 0;
    }
    int units = 0;
    std::size_t byte_index = 0;
    while (byte_index < line.size() && units < utf16_offset) {
        char32_t cp = 0;
        int nbytes = 0;
        if (!decode_utf8_at(line, byte_index, cp, nbytes)) {
            ++byte_index;
            ++units;
            continue;
        }
        const int need = utf16_length(cp);
        if (units + need > utf16_offset) {
            return byte_index;
        }
        units += need;
        byte_index += static_cast<std::size_t>(nbytes);
    }
    return byte_index;
}

std::optional<std::size_t> byte_offset_at_position(std::string_view document, int line, int character) {
    if (line < 0 || character < 0) {
        return std::nullopt;
    }
    int current_line = 0;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i <= document.size(); ++i) {
        if (i == document.size() || document[i] == '\n') {
            if (current_line == line) {
                const std::string_view line_view = document.substr(line_start, i - line_start);
                return line_start + utf16_to_byte_offset_in_line(line_view, character);
            }
            ++current_line;
            line_start = i + 1;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string apply_text_document_content_change(std::string_view document, const JsonValue& change) {
    const JsonValue* text_value = object_field(change, "text");
    const std::string insert_text =
        text_value && text_value->kind == JsonValue::Kind::String ? text_value->string_value : std::string{};

    const JsonValue* range_value = object_field(change, "range");
    if (!range_value || range_value->kind != JsonValue::Kind::Object) {
        return insert_text;
    }

    const JsonValue* start_value = object_field(*range_value, "start");
    const JsonValue* end_value = object_field(*range_value, "end");
    if (!start_value || !end_value) {
        return insert_text;
    }

    const JsonValue* start_line = object_field(*start_value, "line");
    const JsonValue* start_char = object_field(*start_value, "character");
    const JsonValue* end_line = object_field(*end_value, "line");
    const JsonValue* end_char = object_field(*end_value, "character");
    if (!start_line || !start_char || !end_line || !end_char) {
        return insert_text;
    }

    const std::optional<std::size_t> start_byte =
        byte_offset_at_position(document, json_int(*start_line), json_int(*start_char));
    const std::optional<std::size_t> end_byte =
        byte_offset_at_position(document, json_int(*end_line), json_int(*end_char));
    if (!start_byte.has_value() || !end_byte.has_value() || *end_byte < *start_byte) {
        return std::string(document);
    }

    std::string out;
    out.reserve(document.size() - (*end_byte - *start_byte) + insert_text.size());
    out.append(document.substr(0, *start_byte));
    out.append(insert_text);
    out.append(document.substr(*end_byte));
    return out;
}

}  // namespace pscript::lsp
