#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

#include <cctype>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {
namespace {

[[noreturn]] void url_fail(std::string message) {
    native_value_error(std::move(message));
}

std::string url_encode_component(std::string_view text) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : text) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(hex[(ch >> 4) & 0xF]);
            out.push_back(hex[ch & 0xF]);
        }
    }
    return out;
}

std::string url_decode_component(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '%') {
            if (i + 2 >= text.size()) {
                url_fail("invalid percent-encoding");
            }
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') {
                    return c - '0';
                }
                if (c >= 'a' && c <= 'f') {
                    return c - 'a' + 10;
                }
                if (c >= 'A' && c <= 'F') {
                    return c - 'A' + 10;
                }
                return -1;
            };
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi < 0 || lo < 0) {
                url_fail("invalid percent-encoding");
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
            continue;
        }
        if (ch == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

Value url_dict(std::vector<std::pair<std::string, Value>> entries) {
    return Runtime::make_dict(std::move(entries));
}

}  // namespace

Value native_url_parse(const std::vector<Value>& args) {
    native_require_argc("parse", args, 1);
    const std::string& raw = native_require_string(args[0], "parse", "url");
    std::string scheme;
    std::string host;
    std::string path = "/";
    std::string query;
    std::string fragment;
    std::string_view rest = raw;
    const std::size_t scheme_end = rest.find("://");
    if (scheme_end != std::string_view::npos) {
        scheme = std::string(rest.substr(0, scheme_end));
        rest.remove_prefix(scheme_end + 3);
    }
    const std::size_t hash = rest.find('#');
    if (hash != std::string_view::npos) {
        fragment = std::string(rest.substr(hash + 1));
        rest = rest.substr(0, hash);
    }
    const std::size_t q = rest.find('?');
    if (q != std::string_view::npos) {
        query = std::string(rest.substr(q + 1));
        rest = rest.substr(0, q);
    }
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos) {
        host = std::string(rest);
    } else {
        host = std::string(rest.substr(0, slash));
        path = std::string(rest.substr(slash));
        if (path.empty()) {
            path = "/";
        }
    }
    return url_dict({{"scheme", Runtime::make_string(scheme)},
                     {"host", Runtime::make_string(host)},
                     {"path", Runtime::make_string(path)},
                     {"query", Runtime::make_string(query)},
                     {"fragment", Runtime::make_string(fragment)}});
}

Value native_url_build(const std::vector<Value>& args) {
    native_require_argc("build", args, 1);
    if (args[0].kind != ValueKind::Dict || !args[0].dict) {
        native_type_error("'build' expects a dict");
    }
    auto get = [&](std::string_view key) -> std::string {
        for (const auto& entry : args[0].dict->entries) {
            if (entry.first == key && entry.second.kind == ValueKind::String) {
                return entry.second.string;
            }
        }
        return {};
    };
    std::ostringstream out;
    const std::string scheme = get("scheme");
    const std::string host = get("host");
    if (!scheme.empty()) {
        out << scheme << "://";
    }
    out << host;
    std::string path = get("path");
    if (path.empty()) {
        path = "/";
    }
    if (path[0] != '/') {
        out << '/';
    }
    out << path;
    const std::string query = get("query");
    if (!query.empty()) {
        out << '?' << query;
    }
    const std::string fragment = get("fragment");
    if (!fragment.empty()) {
        out << '#' << fragment;
    }
    return Runtime::make_string(out.str());
}

Value native_url_join(const std::vector<Value>& args) {
    native_require_argc("join", args, 2);
    std::string base = native_require_string(args[0], "join", "base");
    const std::string& rel = native_require_string(args[1], "join", "relative");
    if (rel.empty()) {
        return Runtime::make_string(base);
    }
    if (rel.rfind("://", 0) != std::string::npos) {
        return Runtime::make_string(rel);
    }
    if (!base.empty() && base.back() != '/') {
        base.push_back('/');
    }
    if (!rel.empty() && rel.front() == '/') {
        return Runtime::make_string(rel);
    }
    return Runtime::make_string(base + rel);
}

Value native_url_encode(const std::vector<Value>& args) {
    native_require_argc("encode", args, 1);
    return Runtime::make_string(url_encode_component(native_require_string(args[0], "encode", "text")));
}

Value native_url_decode(const std::vector<Value>& args) {
    native_require_argc("decode", args, 1);
    return Runtime::make_string(url_decode_component(native_require_string(args[0], "decode", "text")));
}

Value native_url_parse_query(const std::vector<Value>& args) {
    native_require_argc("parse_query", args, 1);
    const std::string& query = native_require_string(args[0], "parse_query", "query");
    std::vector<std::pair<std::string, Value>> entries;
    std::string_view rest = query;
    while (!rest.empty()) {
        const std::size_t amp = rest.find('&');
        const std::string_view pair = rest.substr(0, amp);
        const std::size_t eq = pair.find('=');
        std::string key = url_decode_component(eq == std::string_view::npos ? pair : pair.substr(0, eq));
        std::string value;
        if (eq != std::string_view::npos) {
            value = url_decode_component(pair.substr(eq + 1));
        }
        entries.emplace_back(std::move(key), Runtime::make_string(value));
        if (amp == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(amp + 1);
    }
    return Runtime::make_dict(std::move(entries));
}

Value native_url_build_query(const std::vector<Value>& args) {
    native_require_argc("build_query", args, 1);
    if (args[0].kind != ValueKind::Dict || !args[0].dict) {
        native_type_error("'build_query' expects a dict");
    }
    std::ostringstream out;
    bool first = true;
    for (const auto& entry : args[0].dict->entries) {
        if (!first) {
            out << '&';
        }
        first = false;
        out << url_encode_component(entry.first) << '=';
        if (entry.second.kind == ValueKind::String) {
            out << url_encode_component(entry.second.string);
        } else {
            out << url_encode_component(Runtime::format(entry.second));
        }
    }
    return Runtime::make_string(out.str());
}

}  // namespace pscript
