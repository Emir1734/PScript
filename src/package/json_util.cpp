#include "package/json_util.h"

#include <cctype>
#include <sstream>

namespace pscript::package {

std::string json_escape(std::string_view text) {
    std::ostringstream out;
    for (char ch : text) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
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
                if (static_cast<unsigned char>(ch) < 0x20) {
                    out << "\\u" << std::hex << std::uppercase;
                    out.width(4);
                    out.fill('0');
                    out << static_cast<int>(static_cast<unsigned char>(ch));
                    out << std::dec;
                } else {
                    out << ch;
                }
                break;
        }
    }
    return out.str();
}

std::string json_string(std::string_view text) {
    return '"' + json_escape(text) + '"';
}

std::string json_auth_credentials_body(const std::string& username, std::string_view password) {
    return std::string("{\"username\":") + json_string(username) + ",\"password\":" + json_string(password) + '}';
}

std::optional<std::string> json_parse_string_field(std::string_view body, std::string_view field_name,
                                                   std::size_t max_body_bytes) {
    if (body.size() > max_body_bytes) {
        return std::nullopt;
    }
    for (unsigned char ch : body) {
        if (ch < 0x09 || (ch > 0x0d && ch < 0x20)) {
            return std::nullopt;
        }
    }
    const std::string needle = "\"" + std::string(field_name) + "\"";
    const std::size_t key = body.find(needle);
    if (key == std::string::npos) {
        return std::nullopt;
    }
    std::size_t colon = body.find(':', key + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    std::size_t quote = body.find('"', colon + 1);
    if (quote == std::string::npos) {
        return std::nullopt;
    }
    std::string value;
    bool escape = false;
    for (std::size_t i = quote + 1; i < body.size(); ++i) {
        const char ch = body[i];
        if (escape) {
            if (ch == 'n') {
                value.push_back('\n');
            } else if (ch == 'r') {
                value.push_back('\r');
            } else if (ch == 't') {
                value.push_back('\t');
            } else if (ch == 'u') {
                return std::nullopt;
            } else {
                value.push_back(ch);
            }
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

}  // namespace pscript::package
