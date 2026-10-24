#include "lsp/uri_util.h"

#include <cctype>
#include <sstream>

namespace pscript::lsp {
namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

std::string percent_decode(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '%' && i + 2 < text.size()) {
            const int hi = hex_value(text[i + 1]);
            const int lo = hex_value(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(text[i]);
    }
    return out;
}

std::string percent_encode(const std::string& text) {
    std::ostringstream out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '/') {
            out << static_cast<char>(c);
        } else {
            out << '%' << std::hex << std::uppercase << (c >> 4) << (c & 0xF) << std::dec;
        }
    }
    return out.str();
}

}  // namespace

std::string uri_to_path(std::string_view uri) {
    constexpr std::string_view kFilePrefix = "file://";
    if (uri.rfind(kFilePrefix, 0) != 0) {
        return std::string(uri);
    }
    std::string path = percent_decode(uri.substr(kFilePrefix.size()));
    if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':') {
        path.erase(0, 1);
    }
    return path;
}

namespace {

bool path_ends_with_phs(std::string_view text) {
    const auto ends_with = [](std::string_view value, std::string_view suffix) {
        return value.size() >= suffix.size() &&
               value.substr(value.size() - suffix.size()) == suffix;
    };
    return ends_with(text, ".phs") || ends_with(text, ".PHS");
}

}  // namespace

bool is_pscript_document_uri(std::string_view uri, std::string_view path) {
    constexpr std::string_view kFilePrefix = "file://";
    if (uri.rfind(kFilePrefix, 0) != 0) {
        return false;
    }
    const std::string path_from_uri = uri_to_path(uri);
    if (path_from_uri != path) {
        return false;
    }
    if (path.empty() || path == "/" || path == "\\") {
        return false;
    }
    if (!path_ends_with_phs(path)) {
        return false;
    }
    std::string_view file_part = uri.substr(kFilePrefix.size());
    const std::size_t query = file_part.find_first_of("?#");
    if (query != std::string_view::npos) {
        file_part = file_part.substr(0, query);
    }
    const std::string decoded_part = percent_decode(file_part);
    if (decoded_part.empty() || decoded_part == "/" || decoded_part == "//") {
        return false;
    }
    return path_ends_with_phs(decoded_part);
}

std::string path_to_uri(const std::string& path) {
    std::string normalized = path;
    for (char& c : normalized) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (normalized.size() >= 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) && normalized[1] == ':') {
        return "file:///" + percent_encode(normalized);
    }
    return "file://" + percent_encode(normalized);
}

}  // namespace pscript::lsp
