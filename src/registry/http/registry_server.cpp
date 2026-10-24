#include "registry/http/registry_server.h"

#include "runtime/http_ops.h"
#include "runtime/net_ops.h"

#include <optional>

namespace pscript::registry {
namespace {

constexpr std::size_t kMaxRequestBytes = 1ULL * 1024 * 1024;

}  // namespace

RegistryServer::RegistryServer(RegistryConfig config, RegistryService& service)
    : config_(std::move(config)),
      service_(service),
      router_(service_, config_.api_prefix, config_.max_search_results, config_.auth.max_auth_body_bytes,
              config_.max_upload_bytes) {}

RegistryServer::~RegistryServer() {
    stop();
}

HttpResponseBody RegistryServer::handle_request(const pscript::HttpServerRequest& request) const {
    return router_.handle(request, client_key_for_request(request));
}

std::string RegistryServer::client_key_for_request(const pscript::HttpServerRequest& request) const {
    if (config_.trust_forwarded_client) {
        const std::optional<std::string> forwarded = pscript::http_header_lookup(request.headers, "X-Forwarded-For");
        if (forwarded && !forwarded->empty()) {
            const std::size_t comma = forwarded->find(',');
            if (comma == std::string::npos) {
                return *forwarded;
            }
            return forwarded->substr(0, comma);
        }
    }
    return config_.bind_host;
}

bool RegistryServer::start(std::string* error) {
    if (running_) {
        return true;
    }
    listener_ = pscript::tcp_listen_sync(config_.bind_host, config_.bind_port);
    bound_port_ = pscript::socket_local_port(listener_);
    running_ = true;
    worker_ = std::thread([this]() { accept_loop(); });
    return true;
}

void RegistryServer::stop() {
    if (!running_) {
        return;
    }
    running_ = false;
    if (listener_ != pscript::kInvalidSocket) {
        pscript::net_close_socket(listener_);
        listener_ = pscript::kInvalidSocket;
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void RegistryServer::accept_loop() {
    while (running_) {
        pscript::SocketHandle client = pscript::tcp_accept_sync(listener_, nullptr);
        if (client == pscript::kInvalidSocket) {
            continue;
        }
        try {
            const std::vector<std::uint8_t> raw = pscript::http_read_message_sync(client, nullptr);
            if (raw.size() > kMaxRequestBytes) {
                const auto response = pscript::http_format_response(413, std::vector<std::uint8_t>{});
                pscript::tcp_write_sync(client, response.data(), response.size());
                pscript::net_close_socket(client);
                continue;
            }
            const pscript::HttpServerRequest request = pscript::http_parse_request(raw);
            const HttpResponseBody handled = router_.handle(request, client_key_for_request(request));
            std::map<std::string, std::string> headers = handled.headers;
            headers["Connection"] = "close";
            const auto response = pscript::http_format_response(handled.status, headers, handled.body);
            pscript::tcp_write_sync(client, response.data(), response.size());
        } catch (...) {
            const auto response = pscript::http_format_response(500, std::vector<std::uint8_t>{});
            pscript::tcp_write_sync(client, response.data(), response.size());
        }
        pscript::net_close_socket(client);
    }
}

}  // namespace pscript::registry
