#pragma once

#include "runtime/net_ops.h"
#include "runtime/runtime.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pscript {

constexpr int kHttpMaxRedirects = 10;

struct HttpUrl {
    bool https = false;
    std::string host;
    int port = 80;
    std::string path = "/";
};

struct HttpClientResponse {
    int status = 0;
    std::map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
};

struct HttpServerRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::vector<std::uint8_t> body;
};

HttpUrl http_parse_url(const std::string& url);
std::string http_url_to_string(const HttpUrl& url);

HttpClientResponse http_request_sync(const std::string& method, const std::string& url,
                                   const std::vector<std::uint8_t>& body,
                                   const std::map<std::string, std::string>& headers,
                                   std::atomic<bool>* cancelled = nullptr);

std::vector<std::uint8_t> http_read_message_sync(SocketHandle socket, std::atomic<bool>* cancelled);
HttpServerRequest http_parse_request(const std::vector<std::uint8_t>& raw);
std::vector<std::uint8_t> http_format_response(int status, const std::map<std::string, std::string>& headers,
                                               const std::vector<std::uint8_t>& body);
std::vector<std::uint8_t> http_format_response(int status, const std::vector<std::uint8_t>& body);

Value http_make_response_value(std::uint32_t class_index, const HttpClientResponse& response);
Value http_make_request_value(std::uint32_t class_index, const HttpServerRequest& request);

std::vector<std::uint8_t> body_from_handler_result(const Value& value);
std::optional<std::string> http_header_lookup(const std::map<std::string, std::string>& headers,
                                              std::string_view name);

bool http_message_size(const std::vector<std::uint8_t>& buffer, std::size_t* total_size);

bool http_tls_available();
bool http_run_tls_self_test();
void http_server_poll_pending_handlers(Runtime& runtime);

}  // namespace pscript
