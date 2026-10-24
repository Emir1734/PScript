#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {
namespace {

enum class TextEncoding {
    Utf8,
    Utf16,
    Ascii,
};

[[noreturn]] void encoding_fail(std::string message) {
    native_value_error(std::move(message));
}

TextEncoding parse_encoding_name(const Value& value, std::string_view function) {
    const std::string& name = native_require_string(value, function, "encoding");
    if (name == "utf8" || name == "utf-8") {
        return TextEncoding::Utf8;
    }
    if (name == "utf16" || name == "utf-16") {
        return TextEncoding::Utf16;
    }
    if (name == "ascii") {
        return TextEncoding::Ascii;
    }
    encoding_fail("unsupported encoding '" + name + "'");
}

TextEncoding encoding_from_args(const std::vector<Value>& args, std::size_t index, std::string_view function) {
    if (index >= args.size()) {
        return TextEncoding::Utf8;
    }
    return parse_encoding_name(args[index], function);
}

const std::vector<std::uint8_t>& require_bytes(const Value& value, std::string_view function) {
    if (value.kind != ValueKind::Bytes || !value.bytes) {
        native_type_error(std::string("'") + std::string(function) + "' expects bytes");
    }
    return value.bytes->data;
}

void append_codepoint_utf8(std::string& out, char32_t codepoint) {
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
        encoding_fail("invalid Unicode code point");
    }
}

std::vector<std::uint8_t> encode_utf8(std::string_view text) {
    return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> encode_ascii(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.reserve(text.size());
    for (unsigned char ch : text) {
        if (ch > 0x7F) {
            encoding_fail("character cannot be encoded as ASCII");
        }
        out.push_back(ch);
    }
    return out;
}

std::vector<std::uint8_t> encode_utf16le(std::string_view text) {
    std::vector<std::uint8_t> out;
    out.push_back(0xFF);
    out.push_back(0xFE);
    std::size_t index = 0;
    while (index < text.size()) {
        unsigned char lead = static_cast<unsigned char>(text[index++]);
        char32_t codepoint = lead;
        if (lead < 0x80) {
            // single byte
        } else if ((lead & 0xE0) == 0xC0 && index < text.size()) {
            unsigned char next = static_cast<unsigned char>(text[index++]);
            if ((next & 0xC0) != 0x80) {
                encoding_fail("invalid UTF-8 text");
            }
            codepoint = ((lead & 0x1F) << 6) | (next & 0x3F);
        } else if ((lead & 0xF0) == 0xE0 && index + 1 <= text.size()) {
            unsigned char b1 = static_cast<unsigned char>(text[index++]);
            unsigned char b2 = static_cast<unsigned char>(text[index++]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) {
                encoding_fail("invalid UTF-8 text");
            }
            codepoint = ((lead & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        } else if ((lead & 0xF8) == 0xF0 && index + 2 <= text.size()) {
            unsigned char b1 = static_cast<unsigned char>(text[index++]);
            unsigned char b2 = static_cast<unsigned char>(text[index++]);
            unsigned char b3 = static_cast<unsigned char>(text[index++]);
            if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) {
                encoding_fail("invalid UTF-8 text");
            }
            codepoint = ((lead & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        } else {
            encoding_fail("invalid UTF-8 text");
        }
        if (codepoint <= 0xFFFF) {
            out.push_back(static_cast<std::uint8_t>(codepoint & 0xFF));
            out.push_back(static_cast<std::uint8_t>((codepoint >> 8) & 0xFF));
        } else {
            const char32_t adjusted = codepoint - 0x10000;
            const char32_t high = 0xD800 + ((adjusted >> 10) & 0x3FF);
            const char32_t low = 0xDC00 + (adjusted & 0x3FF);
            out.push_back(static_cast<std::uint8_t>(high & 0xFF));
            out.push_back(static_cast<std::uint8_t>((high >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>(low & 0xFF));
            out.push_back(static_cast<std::uint8_t>((low >> 8) & 0xFF));
        }
    }
    return out;
}

std::string decode_utf8(const std::vector<std::uint8_t>& data) {
    std::string out;
    std::size_t index = 0;
    while (index < data.size()) {
        const unsigned char lead = data[index++];
        if (lead <= 0x7F) {
            out.push_back(static_cast<char>(lead));
            continue;
        }
        if ((lead & 0xE0) == 0xC0) {
            if (index >= data.size() || (data[index] & 0xC0) != 0x80 || (lead & 0xFE) == 0xC0) {
                encoding_fail("invalid UTF-8 byte sequence");
            }
            const unsigned char next = data[index++];
            const char32_t codepoint = ((lead & 0x1F) << 6) | (next & 0x3F);
            append_codepoint_utf8(out, codepoint);
            continue;
        }
        if ((lead & 0xF0) == 0xE0) {
            if (index + 1 >= data.size() || (data[index] & 0xC0) != 0x80 || (data[index + 1] & 0xC0) != 0x80) {
                encoding_fail("invalid UTF-8 byte sequence");
            }
            const unsigned char b1 = data[index++];
            const unsigned char b2 = data[index++];
            const char32_t codepoint = ((lead & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            append_codepoint_utf8(out, codepoint);
            continue;
        }
        if ((lead & 0xF8) == 0xF0) {
            if (index + 2 >= data.size() || (data[index] & 0xC0) != 0x80 || (data[index + 1] & 0xC0) != 0x80 ||
                (data[index + 2] & 0xC0) != 0x80) {
                encoding_fail("invalid UTF-8 byte sequence");
            }
            const unsigned char b1 = data[index++];
            const unsigned char b2 = data[index++];
            const unsigned char b3 = data[index++];
            const char32_t codepoint =
                ((lead & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
            append_codepoint_utf8(out, codepoint);
            continue;
        }
        encoding_fail("invalid UTF-8 byte sequence");
    }
    return out;
}

std::string decode_ascii(const std::vector<std::uint8_t>& data) {
    std::string out;
    out.reserve(data.size());
    for (std::uint8_t byte : data) {
        if (byte > 0x7F) {
            encoding_fail("invalid ASCII byte sequence");
        }
        out.push_back(static_cast<char>(byte));
    }
    return out;
}

std::string decode_utf16le(const std::vector<std::uint8_t>& data) {
    if (data.size() < 2 || data[0] != 0xFF || data[1] != 0xFE) {
        encoding_fail("UTF-16 data must begin with BOM 0xFF 0xFE");
    }
    if ((data.size() - 2) % 2 != 0) {
        encoding_fail("invalid UTF-16 byte sequence");
    }
    std::string out;
    for (std::size_t i = 2; i + 1 < data.size(); i += 2) {
        const char32_t unit = static_cast<char32_t>(data[i] | (static_cast<char32_t>(data[i + 1]) << 8));
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 3 >= data.size()) {
                encoding_fail("invalid UTF-16 byte sequence");
            }
            const char32_t low = static_cast<char32_t>(data[i + 2] | (static_cast<char32_t>(data[i + 3]) << 8));
            if (low < 0xDC00 || low > 0xDFFF) {
                encoding_fail("invalid UTF-16 byte sequence");
            }
            const char32_t codepoint = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            append_codepoint_utf8(out, codepoint);
            i += 2;
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            encoding_fail("invalid UTF-16 byte sequence");
        }
        append_codepoint_utf8(out, unit);
    }
    return out;
}

}  // namespace

Value native_encoding_encode(const std::vector<Value>& args) {
    native_require_min_argc("encode", args, 1);
    if (args.size() > 2) {
        encoding_fail("'encode' expects 1 or 2 arguments");
    }
    const std::string& text = native_require_string(args[0], "encode", "text");
    const TextEncoding encoding = encoding_from_args(args, 1, "encode");
    switch (encoding) {
        case TextEncoding::Utf8:
            return Runtime::make_bytes(encode_utf8(text));
        case TextEncoding::Ascii:
            return Runtime::make_bytes(encode_ascii(text));
        case TextEncoding::Utf16:
            return Runtime::make_bytes(encode_utf16le(text));
    }
    encoding_fail("unsupported encoding");
}

Value native_encoding_decode(const std::vector<Value>& args) {
    native_require_min_argc("decode", args, 1);
    if (args.size() > 2) {
        encoding_fail("'decode' expects 1 or 2 arguments");
    }
    const std::vector<std::uint8_t>& data = require_bytes(args[0], "decode");
    const TextEncoding encoding = encoding_from_args(args, 1, "decode");
    switch (encoding) {
        case TextEncoding::Utf8:
            return Runtime::make_string(decode_utf8(data));
        case TextEncoding::Ascii:
            return Runtime::make_string(decode_ascii(data));
        case TextEncoding::Utf16:
            return Runtime::make_string(decode_utf16le(data));
    }
    encoding_fail("unsupported encoding");
}

}  // namespace pscript
