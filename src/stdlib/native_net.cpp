#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"
#include "stdlib/stdlib.h"
#include "runtime/async_io.h"
#include "runtime/net_ops.h"

#include <string>
#include <string_view>
#include <vector>

namespace pscript {
namespace {

constexpr std::uint32_t kConnectionClassIndex = 0;
constexpr std::uint32_t kServerClassIndex = 1;
constexpr std::uint32_t kUdpSocketClassIndex = 2;
constexpr std::uint32_t kPacketClassIndex = 3;

[[noreturn]] void net_fail(const std::string& message) {
    throw RuntimeError(message, RuntimeErrorCategory::Network);
}

NativeNetworkData& require_network(const Value& object, std::string_view operation) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_network) {
        net_fail(std::string(operation) + " expects a network object");
    }
    return *object.class_value->native_network;
}

NativeNetworkData& require_open_network(const Value& object, std::string_view operation) {
    NativeNetworkData& data = require_network(object, operation);
    if (data.closed || data.socket == kInvalidSocket) {
        net_fail(std::string(operation) + " on closed resource");
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

std::vector<std::uint8_t> bytes_or_utf8_string(const Value& value, std::string_view function,
                                               std::string_view what) {
    if (value.kind == ValueKind::String) {
        const std::string& text = value.string;
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }
    if (value.kind == ValueKind::Bytes && value.bytes) {
        return value.bytes->data;
    }
    throw RuntimeError(std::string(function) + " expects " + std::string(what) + " to be string or bytes",
                       RuntimeErrorCategory::Type);
}

Value make_tcp_connection(Runtime& runtime, SocketHandle socket) {
    (void)runtime;
    return net_make_connection(kConnectionClassIndex, static_cast<std::uint32_t>(NativeClassKind::TcpConnection),
                               socket);
}

Value make_tcp_server(Runtime& runtime, SocketHandle socket) {
    (void)runtime;
    auto data = std::make_shared<NativeNetworkData>();
    data->kind = static_cast<std::uint32_t>(NativeClassKind::TcpServer);
    data->socket = socket;
    data->closed = false;
    Value value = Runtime::make_class(kServerClassIndex, "Server", std::vector<std::string>{});
    value.class_value->native_network = std::move(data);
    return value;
}

Value make_udp_socket(Runtime& runtime, SocketHandle socket) {
    (void)runtime;
    auto data = std::make_shared<NativeNetworkData>();
    data->kind = static_cast<std::uint32_t>(NativeClassKind::UdpSocket);
    data->socket = socket;
    data->closed = false;
    Value value = Runtime::make_class(kUdpSocketClassIndex, "UdpSocket", std::vector<std::string>{});
    value.class_value->native_network = std::move(data);
    return value;
}

Value make_packet(const NetUdpPacket& packet) {
    return net_make_packet(kPacketClassIndex, packet);
}

void close_network(NativeNetworkData& data) {
    close_native_network(data);
    data.socket = kInvalidSocket;
}

bool use_async(Runtime& runtime) {
    return runtime.async_scheduler_active();
}

Value connection_read(Runtime& runtime, const Value& object) {
    NativeNetworkData& data = require_open_network(object, "read");
    if (use_async(runtime)) {
        return runtime.async_io().submit_net_read(object.class_value->native_network);
    }
    const std::vector<std::uint8_t> bytes = tcp_read_sync(static_cast<SocketHandle>(data.socket));
    return Runtime::make_bytes(bytes);
}

Value connection_write(Runtime& runtime, const Value& object, const std::vector<std::uint8_t>& payload) {
    NativeNetworkData& data = require_open_network(object, "write");
    if (use_async(runtime)) {
        return runtime.async_io().submit_net_write(object.class_value->native_network, payload);
    }
    const std::size_t written =
        tcp_write_sync(static_cast<SocketHandle>(data.socket), payload.data(), payload.size());
    return Runtime::make_integer(static_cast<std::int64_t>(written));
}

Value server_accept(Runtime& runtime, const Value& object) {
    NativeNetworkData& data = require_open_network(object, "accept");
    if (use_async(runtime)) {
        return runtime.async_io().submit_net_accept(object.class_value->native_network, kConnectionClassIndex);
    }
    const SocketHandle client = tcp_accept_sync(static_cast<SocketHandle>(data.socket));
    return make_tcp_connection(runtime, client);
}

Value udp_send(Runtime& runtime, const Value& object, const std::vector<std::uint8_t>& payload,
               const std::string& host, int port) {
    NativeNetworkData& data = require_open_network(object, "send");
    if (use_async(runtime)) {
        return runtime.async_io().submit_net_udp_send(object.class_value->native_network, payload, host, port);
    }
    udp_send_sync(static_cast<SocketHandle>(data.socket), payload.data(), payload.size(), host, port);
    return Runtime::make_integer(static_cast<std::int64_t>(payload.size()));
}

Value udp_receive(Runtime& runtime, const Value& object) {
    NativeNetworkData& data = require_open_network(object, "receive");
    if (use_async(runtime)) {
        return runtime.async_io().submit_net_udp_receive(object.class_value->native_network, kPacketClassIndex);
    }
    const NetUdpPacket packet = udp_receive_sync(static_cast<SocketHandle>(data.socket));
    return make_packet(packet);
}

}  // namespace

Value native_net_connect(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("connect", args, 2);
    const std::string& host = native_require_string_arg(args, 0, "connect", "host");
    const int port = require_port_arg(args[1], "connect");
    if (use_async(runtime)) {
        return runtime.async_io().submit_net_connect(host, port, kConnectionClassIndex);
    }
    const SocketHandle socket = tcp_connect_sync(host, port);
    return make_tcp_connection(runtime, socket);
}

Value native_net_listen(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("listen", args, 2);
    const std::string& host = native_require_string_arg(args, 0, "listen", "host");
    const int port = require_port_arg(args[1], "listen");
    (void)runtime;
    const SocketHandle socket = tcp_listen_sync(host, port);
    return make_tcp_server(runtime, socket);
}

Value native_net_udp(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("udp", args, 0);
    (void)runtime;
    const SocketHandle socket = udp_open_sync();
    return make_udp_socket(runtime, socket);
}

Value native_net_resolve(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("resolve", args, 1);
    const std::string& host = native_require_string_arg(args, 0, "resolve", "host");
    if (use_async(runtime)) {
        return runtime.async_io().submit_net_resolve(host);
    }
    const std::vector<std::string> addresses = dns_resolve_sync(host);
    std::vector<Value> items;
    items.reserve(addresses.size());
    for (const std::string& address : addresses) {
        items.push_back(Runtime::make_string(address));
    }
    return Runtime::make_list(std::move(items));
}

Value call_native_network_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                 Runtime& runtime) {
    NativeNetworkData& data = require_network(object, name);
    const NativeClassKind kind = static_cast<NativeClassKind>(data.kind);

    if (name == "is_open") {
        native_require_argc("is_open", args, 0);
        const bool open = !data.closed && data.socket != kInvalidSocket;
        return Runtime::make_bool(open);
    }
    if (name == "close") {
        native_require_argc("close", args, 0);
        close_network(data);
        return Runtime::make_null();
    }

    if (kind == NativeClassKind::TcpServer || kind == NativeClassKind::UdpSocket) {
        if (name == "local_port") {
            native_require_argc("local_port", args, 0);
            require_open_network(object, "local_port");
            const int port = socket_local_port(static_cast<SocketHandle>(data.socket));
            return Runtime::make_integer(port);
        }
    }

    if (kind == NativeClassKind::TcpConnection) {
        if (name == "read") {
            native_require_argc("read", args, 0);
            return connection_read(runtime, object);
        }
        if (name == "write") {
            native_require_argc("write", args, 1);
            const std::vector<std::uint8_t> payload = bytes_or_utf8_string(args[0], "write", "data");
            return connection_write(runtime, object, payload);
        }
    }

    if (kind == NativeClassKind::TcpServer) {
        if (name == "accept") {
            native_require_argc("accept", args, 0);
            return server_accept(runtime, object);
        }
    }

    if (kind == NativeClassKind::UdpSocket) {
        if (name == "send") {
            native_require_argc("send", args, 3);
            const std::vector<std::uint8_t> payload = bytes_or_utf8_string(args[0], "send", "data");
            const std::string& host = native_require_string_arg(args, 1, "send", "host");
            const int port = require_port_arg(args[2], "send");
            return udp_send(runtime, object, payload, host, port);
        }
        if (name == "receive") {
            native_require_argc("receive", args, 0);
            return udp_receive(runtime, object);
        }
    }

    net_fail(std::string("unknown network method '") + std::string(name) + "'");
}

bool is_native_network(const Value& object) {
    return object.kind == ValueKind::Class && object.class_value && object.class_value->native_network;
}

}  // namespace pscript
