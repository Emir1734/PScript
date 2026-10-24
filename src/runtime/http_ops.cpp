#include "runtime/http_ops.h"

#include "runtime/tls_ops.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>
#include <string_view>

namespace pscript {
namespace {

constexpr std::size_t kHttpReadChunk = 8192;
constexpr std::size_t kMaxHttpMessageSize = 16 * 1024 * 1024;

[[noreturn]] void http_fail(std::string message) {
    network_error(std::move(message));
}

std::string ascii_lower_copy(std::string text) {
    for (char& ch : text) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return text;
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::optional<std::size_t> find_header_end(const std::vector<std::uint8_t>& buffer) {
    for (std::size_t i = 3; i < buffer.size(); ++i) {
        if (buffer[i - 3] == '\r' && buffer[i - 2] == '\n' && buffer[i - 1] == '\r' && buffer[i] == '\n') {
            return i + 1;
        }
    }
    return std::nullopt;
}

std::string trim(std::string_view text) {
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    return std::string(text.substr(start, end - start));
}

std::optional<std::int64_t> parse_int64(std::string_view text) {
    std::int64_t value = 0;
    const std::string digits(text);
    const char* begin = digits.c_str();
    const char* end = begin + digits.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::nullopt;
    }
    return value;
}

std::map<std::string, std::string> parse_headers(std::string_view block) {
    std::map<std::string, std::string> headers;
    std::istringstream stream{std::string(block)};
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = ascii_lower_copy(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        headers[name] = value;
    }
    return headers;
}

std::vector<std::uint8_t> recv_some(SocketHandle socket, std::atomic<bool>* cancelled) {
    return tcp_read_sync(socket, cancelled);
}

std::vector<std::uint8_t> read_http_message_plain(SocketHandle socket, std::atomic<bool>* cancelled) {
    std::vector<std::uint8_t> buffer;
    std::size_t target = 0;
    while (!http_message_size(buffer, &target)) {
        if (cancelled != nullptr && cancelled->load()) {
            http_fail("operation cancelled");
        }
        const std::vector<std::uint8_t> chunk = recv_some(socket, cancelled);
        buffer.insert(buffer.end(), chunk.begin(), chunk.end());
        if (buffer.size() > kMaxHttpMessageSize) {
            http_fail("HTTP message too large");
        }
    }
    buffer.resize(target);
    return buffer;
}

std::vector<std::uint8_t> build_request_bytes(const std::string& method, const HttpUrl& url,
                                              const std::vector<std::uint8_t>& body,
                                              const std::map<std::string, std::string>& headers) {
    std::ostringstream request;
    request << method << ' ' << url.path << " HTTP/1.1\r\n";
    request << "Host: " << url.host;
    if ((url.https && url.port != 443) || (!url.https && url.port != 80)) {
        request << ':' << url.port;
    }
    request << "\r\n";
    request << "Connection: close\r\n";
    request << "Accept: */*\r\n";
    for (const auto& [name, value] : headers) {
        request << name << ": " << value << "\r\n";
    }
    if (!body.empty()) {
        request << "Content-Length: " << body.size() << "\r\n";
    }
    request << "\r\n";
    std::string text = request.str();
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

HttpClientResponse parse_client_response(const std::vector<std::uint8_t>& raw) {
    const std::optional<std::size_t> header_end = find_header_end(raw);
    if (!header_end.has_value()) {
        http_fail("invalid HTTP response");
    }
    const std::string header_block(reinterpret_cast<const char*>(raw.data()), *header_end);
    std::istringstream status_line{header_block};
    std::string http_version;
    int status = 0;
    std::string reason;
    status_line >> http_version >> status;
    std::getline(status_line, reason);
    HttpClientResponse response;
    response.status = status;
    response.headers = parse_headers(std::string_view(header_block).substr(header_block.find("\r\n") + 2));
    const std::size_t body_start = *header_end;
    response.body.assign(raw.begin() + static_cast<std::ptrdiff_t>(body_start), raw.end());
    return response;
}

std::optional<std::string> header_value(const std::map<std::string, std::string>& headers, std::string_view name) {
    const std::string lowered = ascii_lower_copy(std::string(name));
    const auto it = headers.find(lowered);
    if (it != headers.end()) {
        return it->second;
    }
    for (const auto& [key, value] : headers) {
        if (ascii_lower_copy(key) == lowered) {
            return value;
        }
    }
    return std::nullopt;
}

HttpClientResponse http_exchange(const std::string& method, const HttpUrl& url,
                                 const std::vector<std::uint8_t>& body,
                                 const std::map<std::string, std::string>& headers,
                                 std::atomic<bool>* cancelled) {
    const SocketHandle socket = tcp_connect_sync(url.host, url.port, cancelled);
    std::vector<std::uint8_t> raw;
    try {
        const std::vector<std::uint8_t> request = build_request_bytes(method, url, body, headers);
        if (url.https) {
            TlsConnection tls = tls_client_connect(socket, url.host, cancelled);
            tls_write_all(tls, request.data(), request.size(), cancelled);
            raw = tls_read_http_message(tls, cancelled);
            tls_close(tls);
        } else {
            tcp_write_sync(socket, request.data(), request.size());
            raw = read_http_message_plain(socket, cancelled);
        }
    } catch (...) {
        net_close_socket(socket);
        throw;
    }
    net_close_socket(socket);
    return parse_client_response(raw);
}

}  // namespace

HttpUrl http_parse_url(const std::string& url) {
    HttpUrl parsed;
    std::string_view view(url);
    if (starts_with(view, "http://")) {
        view.remove_prefix(7);
        parsed.https = false;
        parsed.port = 80;
    } else if (starts_with(view, "https://")) {
        view.remove_prefix(8);
        parsed.https = true;
        parsed.port = 443;
    } else {
        http_fail("URL must start with http:// or https://");
    }
    if (view.empty()) {
        http_fail("invalid URL");
    }
    std::string host_port(view);
    std::string path = "/";
    const std::size_t slash = host_port.find('/');
    if (slash != std::string::npos) {
        path = host_port.substr(slash);
        host_port = host_port.substr(0, slash);
    }
    if (host_port.empty()) {
        http_fail("invalid URL host");
    }
    if (host_port.front() == '[') {
        const std::size_t closing = host_port.find(']');
        if (closing == std::string::npos) {
            http_fail("invalid IPv6 URL host");
        }
        parsed.host = host_port.substr(1, closing - 1);
        if (closing + 1 < host_port.size()) {
            if (host_port[closing + 1] != ':') {
                http_fail("invalid URL host");
            }
            const std::optional<std::int64_t> port = parse_int64(host_port.substr(closing + 2));
            if (!port.has_value() || *port < 0 || *port > 65535) {
                http_fail("invalid URL port");
            }
            parsed.port = static_cast<int>(*port);
        }
    } else {
        const std::size_t colon = host_port.rfind(':');
        if (colon != std::string::npos) {
            const std::optional<std::int64_t> port = parse_int64(host_port.substr(colon + 1));
            if (!port.has_value() || *port < 0 || *port > 65535) {
                http_fail("invalid URL port");
            }
            parsed.port = static_cast<int>(*port);
            parsed.host = host_port.substr(0, colon);
        } else {
            parsed.host = host_port;
        }
    }
    parsed.path = path.empty() ? "/" : path;
    if (parsed.https && !http_tls_available()) {
        http_fail("HTTPS is not available on this platform build");
    }
    return parsed;
}

std::string http_url_to_string(const HttpUrl& url) {
    std::ostringstream out;
    out << (url.https ? "https://" : "http://") << url.host;
    if ((url.https && url.port != 443) || (!url.https && url.port != 80)) {
        out << ':' << url.port;
    }
    out << url.path;
    return out.str();
}

HttpClientResponse http_request_sync(const std::string& method, const std::string& url,
                                     const std::vector<std::uint8_t>& body,
                                     const std::map<std::string, std::string>& headers,
                                     std::atomic<bool>* cancelled) {
    HttpUrl current = http_parse_url(url);
    std::string request_method = method;
    std::vector<std::uint8_t> request_body = body;
    for (int redirect = 0; redirect <= kHttpMaxRedirects; ++redirect) {
        HttpClientResponse response =
            http_exchange(request_method, current, request_body, headers, cancelled);
        if (response.status != 301 && response.status != 302 && response.status != 303 && response.status != 307 &&
            response.status != 308) {
            return response;
        }
        if (redirect == kHttpMaxRedirects) {
            http_fail("too many HTTP redirects");
        }
        const std::optional<std::string> location = header_value(response.headers, "location");
        if (!location.has_value() || location->empty()) {
            http_fail("redirect response missing Location header");
        }
        current = http_parse_url(*location);
        if (response.status == 303) {
            request_method = "GET";
            request_body.clear();
        }
    }
    http_fail("too many HTTP redirects");
}

std::vector<std::uint8_t> http_read_message_sync(SocketHandle socket, std::atomic<bool>* cancelled) {
    return read_http_message_plain(socket, cancelled);
}

HttpServerRequest http_parse_request(const std::vector<std::uint8_t>& raw) {
    const std::optional<std::size_t> header_end = find_header_end(raw);
    if (!header_end.has_value()) {
        http_fail("invalid HTTP request");
    }
    const std::string header_block(reinterpret_cast<const char*>(raw.data()), *header_end);
    std::istringstream status_line{header_block};
    HttpServerRequest request;
    status_line >> request.method >> request.path;
    request.headers = parse_headers(std::string_view(header_block).substr(header_block.find("\r\n") + 2));
    const std::size_t body_start = *header_end;
    request.body.assign(raw.begin() + static_cast<std::ptrdiff_t>(body_start), raw.end());
    return request;
}

std::vector<std::uint8_t> http_format_response(int status, const std::map<std::string, std::string>& headers,
                                               const std::vector<std::uint8_t>& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << status << ' ';
    switch (status) {
        case 200:
            response << "OK";
            break;
        case 404:
            response << "Not Found";
            break;
        default:
            response << "Response";
            break;
    }
    response << "\r\n";
    response << "Connection: close\r\n";
    response << "Content-Length: " << body.size() << "\r\n";
    for (const auto& [name, value] : headers) {
        response << name << ": " << value << "\r\n";
    }
    response << "\r\n";
    std::string text = response.str();
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<std::uint8_t> http_format_response(int status, const std::vector<std::uint8_t>& body) {
    return http_format_response(status, {}, body);
}

Value http_make_response_value(std::uint32_t class_index, const HttpClientResponse& response) {
    std::vector<std::pair<std::string, Value>> header_entries;
    header_entries.reserve(response.headers.size());
    for (const auto& [name, value] : response.headers) {
        header_entries.emplace_back(name, Runtime::make_string(value));
    }
    Value object = Runtime::make_class(class_index, "Response", {"status", "headers", "body"});
    object.class_value->fields = {
        Runtime::make_integer(static_cast<std::int64_t>(response.status)),
        Runtime::make_dict(std::move(header_entries)),
        Runtime::make_bytes(response.body),
    };
    return object;
}

Value http_make_request_value(std::uint32_t class_index, const HttpServerRequest& request) {
    std::vector<std::pair<std::string, Value>> header_entries;
    header_entries.reserve(request.headers.size());
    for (const auto& [name, value] : request.headers) {
        header_entries.emplace_back(name, Runtime::make_string(value));
    }
    Value object = Runtime::make_class(class_index, "Request", {"method", "path", "headers", "body"});
    object.class_value->fields = {
        Runtime::make_string(request.method),
        Runtime::make_string(request.path),
        Runtime::make_dict(std::move(header_entries)),
        Runtime::make_bytes(request.body),
    };
    return object;
}

std::vector<std::uint8_t> body_from_handler_result(const Value& value) {
    if (value.kind == ValueKind::String) {
        const std::string& text = value.string;
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }
    if (value.kind == ValueKind::Bytes && value.bytes) {
        return value.bytes->data;
    }
    throw RuntimeError("HTTP handler must return string or bytes", RuntimeErrorCategory::Type);
}

std::optional<std::string> http_header_lookup(const std::map<std::string, std::string>& headers,
                                              std::string_view name) {
    return header_value(headers, name);
}

bool http_message_size(const std::vector<std::uint8_t>& buffer, std::size_t* total_size) {
    const std::optional<std::size_t> header_end = find_header_end(buffer);
    if (!header_end.has_value()) {
        return false;
    }
    const std::string header_block(reinterpret_cast<const char*>(buffer.data()), *header_end);
    const std::map<std::string, std::string> headers = parse_headers(
        std::string_view(header_block).substr(header_block.find("\r\n") + 2));
    std::size_t target = buffer.size();
    if (const auto it = headers.find("content-length"); it != headers.end()) {
        const std::optional<std::int64_t> length = parse_int64(it->second);
        if (!length.has_value() || *length < 0) {
            http_fail("invalid Content-Length header");
        }
        target = *header_end + static_cast<std::size_t>(*length);
    } else {
        target = *header_end;
    }
    *total_size = target;
    return buffer.size() >= target;
}

}  // namespace pscript
