#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"
#include "stdlib/stdlib.h"
#include "runtime/async_io.h"
#include "runtime/http_ops.h"
#include "runtime/net_ops.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pscript {
namespace {

std::mutex g_active_servers_mutex;
std::vector<NativeHttpServerData*> g_active_servers;

void register_active_server(NativeHttpServerData* server) {
    std::lock_guard lock(g_active_servers_mutex);
    g_active_servers.push_back(server);
}

void unregister_active_server(NativeHttpServerData* server) {
    std::lock_guard lock(g_active_servers_mutex);
    g_active_servers.erase(std::remove(g_active_servers.begin(), g_active_servers.end(), server),
                           g_active_servers.end());
}

void write_client_response(SocketHandle client, int status, const std::vector<std::uint8_t>& body) {
    const std::vector<std::uint8_t> response = http_format_response(status, body);
    tcp_write_sync(client, response.data(), response.size());
    net_close_socket(client);
}

constexpr std::uint32_t kResponseClassIndex = 0;
constexpr std::uint32_t kRequestClassIndex = 1;
constexpr std::uint32_t kHttpServerClassIndex = 2;
constexpr std::uint32_t kTcpConnectionClassIndex = 0;

[[noreturn]] void http_fail(const std::string& message) {
    throw RuntimeError(message, RuntimeErrorCategory::Network);
}

NativeHttpServerData& require_http_server(const Value& object, std::string_view operation) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_http) {
        http_fail(std::string(operation) + " expects an HTTP server");
    }
    return *object.class_value->native_http;
}

NativeHttpServerData& require_open_http_server(const Value& object, std::string_view operation) {
    NativeHttpServerData& data = require_http_server(object, operation);
    if (data.closed) {
        http_fail(std::string(operation) + " on closed resource");
    }
    return data;
}

int require_port_arg(const Value& value, std::string_view function) {
    if (value.kind != ValueKind::Integer) {
        throw RuntimeError(std::string(function) + " expects port to be int", RuntimeErrorCategory::Type);
    }
    validate_network_port(value.integer, function);
    return static_cast<int>(value.integer);
}

std::vector<std::uint8_t> optional_body_arg(const std::vector<Value>& args, std::size_t index, std::string_view function) {
    if (args.size() <= index || args[index].kind == ValueKind::Null) {
        return {};
    }
    if (args[index].kind == ValueKind::String) {
        const std::string& text = args[index].string;
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }
    if (args[index].kind == ValueKind::Bytes && args[index].bytes) {
        return args[index].bytes->data;
    }
    throw RuntimeError(std::string(function) + " expects body to be string or bytes", RuntimeErrorCategory::Type);
}

std::map<std::string, std::string> optional_headers_arg(const std::vector<Value>& args, std::size_t index,
                                                        std::string_view function) {
    if (args.size() <= index || args[index].kind == ValueKind::Null) {
        return {};
    }
    if (args[index].kind != ValueKind::Dict || !args[index].dict) {
        throw RuntimeError(std::string(function) + " expects headers to be dict", RuntimeErrorCategory::Type);
    }
    std::map<std::string, std::string> headers;
    for (const auto& [key, value] : args[index].dict->entries) {
        if (value.kind != ValueKind::String) {
            throw RuntimeError(std::string(function) + " expects header values to be string", RuntimeErrorCategory::Type);
        }
        headers[key] = value.string;
    }
    return headers;
}

void queue_handler_invoke(Runtime& runtime, NativeHttpServerData& server, SocketHandle client, const Value& handler,
                          const Value& request) {
    auto promise = std::make_shared<std::promise<Value>>();
    NativeHttpServerData::PendingHandler pending;
    pending.client = static_cast<std::uintptr_t>(client);
    pending.promise = promise;
    pending.future = promise->get_future();
    runtime.queue_callable_invoke(handler, {request}, promise);
    runtime.async_io().wake_waiters();
    std::lock_guard lock(server.pending_mutex);
    server.pending_handlers.push_back(std::move(pending));
}

const NativeHttpServerData::Route* match_route(const NativeHttpServerData& server, std::string_view method,
                                               std::string_view path) {
    for (const NativeHttpServerData::Route& route : server.routes) {
        if (route.method == method && route.path == path) {
            return &route;
        }
    }
    return nullptr;
}

void handle_client(Runtime* runtime, SocketHandle client, NativeHttpServerData& server) {
    try {
        const std::vector<std::uint8_t> raw = http_read_message_sync(client, nullptr);
        const HttpServerRequest request = http_parse_request(raw);
        const NativeHttpServerData::Route* route = match_route(server, request.method, request.path);
        if (route != nullptr) {
            const Value request_value = http_make_request_value(kRequestClassIndex, request);
            queue_handler_invoke(*runtime, server, client, route->handler, request_value);
            return;
        }
        write_client_response(client, 404, std::vector<std::uint8_t>{'N', 'o', 't', ' ', 'F', 'o', 'u', 'n', 'd'});
    } catch (...) {
        try {
            write_client_response(client, 500, {});
        } catch (...) {
            net_close_socket(client);
        }
    }
}

void http_server_worker(Runtime* runtime, NativeHttpServerData* server) {
    while (!server->stop_requested.load()) {
        if (server->closed || !server->listener || server->listener->closed) {
            break;
        }
        try {
            const SocketHandle client =
                tcp_accept_sync(static_cast<SocketHandle>(server->listener->socket), &server->stop_requested);
            handle_client(runtime, client, *server);
        } catch (const RuntimeError& err) {
            if (err.message() == "operation cancelled") {
                break;
            }
            if (server->stop_requested.load()) {
                break;
            }
        }
    }
}

Value make_http_server(Runtime& runtime, int port) {
    (void)runtime;
    auto data = std::make_shared<NativeHttpServerData>();
    const SocketHandle listener = tcp_listen_sync("127.0.0.1", port);
    data->listener = std::make_shared<NativeNetworkData>();
    data->listener->kind = static_cast<std::uint32_t>(NativeClassKind::TcpServer);
    data->listener->socket = listener;
    data->listener->closed = false;
    Value value = Runtime::make_class(kHttpServerClassIndex, "HttpServer", std::vector<std::string>{});
    value.class_value->native_http = std::move(data);
    return value;
}

bool use_async(Runtime& runtime) {
    return runtime.async_scheduler_active();
}

}  // namespace

Value native_http_get(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("get", args, 1);
    const std::string& url = native_require_string_arg(args, 0, "get", "url");
    if (use_async(runtime)) {
        return runtime.async_io().submit_http_request("GET", url, {}, {}, kResponseClassIndex);
    }
    const HttpClientResponse response = http_request_sync("GET", url, {}, {});
    return http_make_response_value(kResponseClassIndex, response);
}

Value native_http_post(Runtime& runtime, const std::vector<Value>& args) {
    native_require_min_argc("post", args, 2);
    const std::string& url = native_require_string_arg(args, 0, "post", "url");
    const std::vector<std::uint8_t> body = optional_body_arg(args, 1, "post");
    if (use_async(runtime)) {
        return runtime.async_io().submit_http_request("POST", url, body, {}, kResponseClassIndex);
    }
    const HttpClientResponse response = http_request_sync("POST", url, body, {});
    return http_make_response_value(kResponseClassIndex, response);
}

Value native_http_request(Runtime& runtime, const std::vector<Value>& args) {
    native_require_min_argc("request", args, 2);
    const std::string& method = native_require_string_arg(args, 0, "request", "method");
    const std::string& url = native_require_string_arg(args, 1, "request", "url");
    const std::vector<std::uint8_t> body = optional_body_arg(args, 2, "request");
    const std::map<std::string, std::string> headers = optional_headers_arg(args, 3, "request");
    if (use_async(runtime)) {
        return runtime.async_io().submit_http_request(method, url, body, headers, kResponseClassIndex);
    }
    const HttpClientResponse response = http_request_sync(method, url, body, headers);
    return http_make_response_value(kResponseClassIndex, response);
}

Value native_http_server(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("server", args, 1);
    const int port = require_port_arg(args[0], "server");
    return make_http_server(runtime, port);
}

Value call_native_http_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                              Runtime& runtime) {
    NativeHttpServerData& data = require_http_server(object, name);

    if (name == "is_open") {
        native_require_argc("is_open", args, 0);
        return Runtime::make_bool(!data.closed);
    }
    if (name == "local_port") {
        native_require_argc("local_port", args, 0);
        require_open_http_server(object, "local_port");
        if (!data.listener) {
            http_fail("invalid HTTP server listener");
        }
        return Runtime::make_integer(socket_local_port(static_cast<SocketHandle>(data.listener->socket)));
    }
    if (name == "get" || name == "post") {
        native_require_argc(name, args, 2);
        const std::string& path = native_require_string_arg(args, 0, name, "path");
        if (args[1].kind != ValueKind::Function || !args[1].function) {
            throw RuntimeError(std::string(name) + " expects handler to be a function", RuntimeErrorCategory::Type);
        }
        data.routes.push_back(NativeHttpServerData::Route{std::string(name == "get" ? "GET" : "POST"), path, args[1]});
        return Runtime::make_null();
    }
    if (name == "start") {
        native_require_argc("start", args, 0);
        require_open_http_server(object, "start");
        if (!use_async(runtime)) {
            throw RuntimeError("http.server.start requires async func main", RuntimeErrorCategory::Runtime);
        }
        if (data.running) {
            return Runtime::make_null();
        }
        data.running = true;
        data.stop_requested.store(false);
        register_active_server(&data);
        NativeHttpServerData* server_ptr = &data;
        data.worker = std::thread([server_ptr, runtime_ptr = &runtime]() { http_server_worker(runtime_ptr, server_ptr); });
        return Runtime::make_null();
    }
    if (name == "close") {
        native_require_argc("close", args, 0);
        if (!data.closed) {
            data.stop_requested.store(true);
            unregister_active_server(&data);
            if (data.listener && !data.listener->closed) {
                net_close_socket(static_cast<SocketHandle>(data.listener->socket));
                data.listener->socket = kInvalidSocket;
                data.listener->closed = true;
            }
            if (data.worker.joinable()) {
                data.worker.join();
            }
            data.running = false;
            data.closed = true;
        }
        return Runtime::make_null();
    }
    http_fail("HttpServer has no method '" + std::string(name) + "'");
}

bool is_native_http_server(const Value& object) {
    return object.kind == ValueKind::Class && object.class_value && object.class_value->native_http;
}

void poll_pending_handlers(Runtime& runtime) {
    std::vector<NativeHttpServerData*> servers;
    {
        std::lock_guard lock(g_active_servers_mutex);
        servers = g_active_servers;
    }
    for (NativeHttpServerData* server : servers) {
        if (server == nullptr) {
            continue;
        }
        std::vector<NativeHttpServerData::PendingHandler> ready;
        {
            std::lock_guard lock(server->pending_mutex);
            for (auto it = server->pending_handlers.begin(); it != server->pending_handlers.end();) {
                if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                    ++it;
                    continue;
                }
                ready.push_back(std::move(*it));
                it = server->pending_handlers.erase(it);
            }
        }
        for (NativeHttpServerData::PendingHandler& pending : ready) {
            try {
                const Value result = pending.future.get();
                const std::vector<std::uint8_t> body = body_from_handler_result(result);
                write_client_response(static_cast<SocketHandle>(pending.client), 200, body);
            } catch (...) {
                write_client_response(static_cast<SocketHandle>(pending.client), 500, {});
            }
        }
    }
    (void)runtime;
}

void http_server_poll_pending_handlers(Runtime& runtime) {
    poll_pending_handlers(runtime);
}

}  // namespace pscript
