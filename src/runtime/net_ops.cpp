#include "runtime/net_ops.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pscript {
namespace {

constexpr int kPollIntervalMs = 50;
constexpr std::size_t kTcpReadChunk = 4096;
constexpr std::size_t kUdpMaxDatagram = 65535;

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            network_error("failed to initialize Winsock");
        }
    }
    ~WinsockInit() { WSACleanup(); }
};

const WinsockInit& winsock_init() {
    static const WinsockInit init;
    return init;
}

SocketHandle to_handle(SOCKET socket) {
    return static_cast<SocketHandle>(socket);
}

SOCKET from_handle(SocketHandle socket) {
    return static_cast<SOCKET>(socket);
}

bool is_cancelled(std::atomic<bool>* cancelled) {
    return cancelled != nullptr && cancelled->load(std::memory_order_acquire);
}

int last_socket_error() {
    return WSAGetLastError();
}

bool is_would_block(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

bool is_interrupted(int error) {
    return error == WSAEINTR;
}

void set_nonblocking(SocketHandle socket, bool enabled) {
    u_long mode = enabled ? 1 : 0;
    ioctlsocket(from_handle(socket), FIONBIO, &mode);
}

bool poll_socket(SocketHandle socket, bool read_ready, bool write_ready, int timeout_ms) {
    winsock_init();
    fd_set readfds;
    fd_set writefds;
    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    if (read_ready) {
        FD_SET(from_handle(socket), &readfds);
    }
    if (write_ready) {
        FD_SET(from_handle(socket), &writefds);
    }
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(0, read_ready ? &readfds : nullptr, write_ready ? &writefds : nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return false;
    }
    if (read_ready && FD_ISSET(from_handle(socket), &readfds)) {
        return true;
    }
    if (write_ready && FD_ISSET(from_handle(socket), &writefds)) {
        return true;
    }
    return false;
}

std::string socket_error_message(int error) {
    char buffer[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0, buffer,
                   sizeof(buffer), nullptr);
    std::string message(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' || message.back() == ' ')) {
        message.pop_back();
    }
    if (message.empty()) {
        return "socket error " + std::to_string(error);
    }
    return message;
}

#else

SocketHandle to_handle(int socket) { return socket; }
int from_handle(SocketHandle socket) { return static_cast<int>(socket); }

bool is_cancelled(std::atomic<bool>* cancelled) {
    return cancelled != nullptr && cancelled->load(std::memory_order_acquire);
}

int last_socket_error() { return errno; }

bool is_would_block(int error) { return error == EINPROGRESS || error == EWOULDBLOCK; }

bool is_interrupted(int error) { return error == EINTR; }

void set_nonblocking(SocketHandle socket, bool enabled) {
    const int flags = fcntl(from_handle(socket), F_GETFL, 0);
    if (flags < 0) {
        network_error("failed to configure socket");
    }
    if (fcntl(from_handle(socket), F_SETFL, enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) != 0) {
        network_error("failed to configure socket");
    }
}

bool poll_socket(SocketHandle socket, bool read_ready, bool write_ready, int timeout_ms) {
    pollfd descriptor{};
    descriptor.fd = from_handle(socket);
    if (read_ready) {
        descriptor.events |= POLLIN;
    }
    if (write_ready) {
        descriptor.events |= POLLOUT;
    }
    const int ready = poll(&descriptor, 1, timeout_ms);
    if (ready <= 0) {
        return false;
    }
    if (read_ready && (descriptor.revents & POLLIN) != 0) {
        return true;
    }
    if (write_ready && (descriptor.revents & POLLOUT) != 0) {
        return true;
    }
    return false;
}

std::string socket_error_message(int error) { return std::strerror(error); }

#endif

void wait_or_cancel(std::atomic<bool>* cancelled) {
    if (cancelled == nullptr) {
        return;
    }
#ifdef _WIN32
    Sleep(kPollIntervalMs);
#else
    poll(nullptr, 0, kPollIntervalMs);
#endif
}

std::string address_to_host(const sockaddr* address, socklen_t length) {
    std::array<char, NI_MAXHOST> host{};
    if (getnameinfo(address, length, host.data(), static_cast<socklen_t>(host.size()), nullptr, 0,
                    NI_NUMERICHOST) != 0) {
        return "0.0.0.0";
    }
    return host.data();
}

int address_to_port(const sockaddr* address, socklen_t length) {
    if (address->sa_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(address)->sin_port);
    }
    if (address->sa_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(address)->sin6_port);
    }
    return 0;
}

SocketHandle create_resolved_socket(const std::string& host, int port, bool passive) {
    net_platform_init();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (passive) {
        hints.ai_flags = AI_PASSIVE;
    }
    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    const char* host_ptr = host.c_str();
    if (passive && (host == "0.0.0.0" || host == "::" || host.empty())) {
        host_ptr = nullptr;
    }
    const int rc = getaddrinfo(host_ptr, port_text.c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
        if (results != nullptr) {
            freeaddrinfo(results);
        }
#ifdef _WIN32
        network_error("DNS resolution failed for '" + host + "': " + gai_strerrorA(rc));
#else
        network_error("DNS resolution failed for '" + host + "': " + gai_strerror(rc));
#endif
    }

    SocketHandle created = kInvalidSocket;
    for (addrinfo* cursor = results; cursor != nullptr; cursor = cursor->ai_next) {
        created = to_handle(socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol));
        if (created == kInvalidSocket) {
            continue;
        }
        if (passive) {
            int yes = 1;
            setsockopt(from_handle(created), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                       sizeof(yes));
            if (bind(from_handle(created), cursor->ai_addr, static_cast<int>(cursor->ai_addrlen)) == 0) {
                break;
            }
        } else if (connect(from_handle(created), cursor->ai_addr, static_cast<int>(cursor->ai_addrlen)) == 0) {
            break;
        }
        net_close_socket(created);
        created = kInvalidSocket;
    }
    freeaddrinfo(results);
    if (created == kInvalidSocket) {
        network_error("connection failed for '" + host + ":" + port_text + "'");
    }
    return created;
}

SocketHandle connect_with_cancellation(const std::string& host, int port, std::atomic<bool>* cancelled) {
    net_platform_init();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
        if (results != nullptr) {
            freeaddrinfo(results);
        }
#ifdef _WIN32
        network_error("DNS resolution failed for '" + host + "': " + gai_strerrorA(rc));
#else
        network_error("DNS resolution failed for '" + host + "': " + gai_strerror(rc));
#endif
    }

    SocketHandle connected = kInvalidSocket;
    for (addrinfo* cursor = results; cursor != nullptr; cursor = cursor->ai_next) {
        if (is_cancelled(cancelled)) {
            freeaddrinfo(results);
            network_error("operation cancelled");
        }
        connected = to_handle(socket(cursor->ai_family, cursor->ai_socktype, cursor->ai_protocol));
        if (connected == kInvalidSocket) {
            continue;
        }
        set_nonblocking(connected, true);
        const int connect_rc = connect(from_handle(connected), cursor->ai_addr, static_cast<int>(cursor->ai_addrlen));
        if (connect_rc == 0) {
            set_nonblocking(connected, false);
            break;
        }
        const int error = last_socket_error();
        if (!is_would_block(error)) {
            net_close_socket(connected);
            connected = kInvalidSocket;
            continue;
        }
        while (true) {
            if (is_cancelled(cancelled)) {
                net_close_socket(connected);
                freeaddrinfo(results);
                network_error("operation cancelled");
            }
            int so_error = 0;
            socklen_t so_length = sizeof(so_error);
            if (getsockopt(from_handle(connected), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error),
                           &so_length) == 0 &&
                so_error != 0) {
                net_close_socket(connected);
                connected = kInvalidSocket;
                break;
            }
            if (poll_socket(connected, false, true, kPollIntervalMs)) {
                so_error = 0;
                so_length = sizeof(so_error);
                if (getsockopt(from_handle(connected), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error),
                               &so_length) != 0 ||
                    so_error != 0) {
                    net_close_socket(connected);
                    connected = kInvalidSocket;
                    break;
                }
                set_nonblocking(connected, false);
                break;
            }
            wait_or_cancel(cancelled);
        }
        if (connected != kInvalidSocket) {
            break;
        }
    }
    freeaddrinfo(results);
    if (connected == kInvalidSocket) {
        network_error("connection failed for '" + host + ":" + port_text + "'");
    }
    return connected;
}

void set_blocking_mode(SocketHandle socket, bool blocking) {
    set_nonblocking(socket, !blocking);
}

}  // namespace

void net_platform_init() {
#ifdef _WIN32
    (void)winsock_init();
#endif
}

void net_close_socket(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(from_handle(socket));
#else
    close(from_handle(socket));
#endif
}

void validate_network_port(std::int64_t port, std::string_view operation) {
    if (port < 0 || port > 65535) {
        throw RuntimeError(std::string(operation) + " expects port in range 0..65535", RuntimeErrorCategory::Value);
    }
}

void validate_network_host(const std::string& host, std::string_view operation) {
    if (host.empty()) {
        throw RuntimeError(std::string(operation) + " expects a non-empty host", RuntimeErrorCategory::Value);
    }
}

[[noreturn]] void network_error(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Network);
}

SocketHandle tcp_connect_sync(const std::string& host, int port, std::atomic<bool>* cancelled) {
    validate_network_host(host, "connect");
    validate_network_port(port, "connect");
    if (cancelled != nullptr) {
        return connect_with_cancellation(host, port, cancelled);
    }
    return create_resolved_socket(host, port, false);
}

SocketHandle tcp_listen_sync(const std::string& host, int port) {
    validate_network_host(host, "listen");
    validate_network_port(port, "listen");
    const SocketHandle listener = create_resolved_socket(host, port, true);
    if (listen(from_handle(listener), SOMAXCONN) != 0) {
        const int error = last_socket_error();
        net_close_socket(listener);
        network_error("listen failed on '" + host + ":" + std::to_string(port) + "': " + socket_error_message(error));
    }
    return listener;
}

SocketHandle tcp_accept_sync(SocketHandle listener, std::atomic<bool>* cancelled) {
    if (listener == kInvalidSocket) {
        network_error("accept on invalid server");
    }
    if (cancelled == nullptr) {
        set_blocking_mode(listener, true);
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const SocketHandle client =
            to_handle(accept(from_handle(listener), reinterpret_cast<sockaddr*>(&address), &length));
        if (client == kInvalidSocket) {
            network_error("accept failed: " + socket_error_message(last_socket_error()));
        }
        return client;
    }
    set_nonblocking(listener, true);
    while (true) {
        if (is_cancelled(cancelled)) {
            set_nonblocking(listener, false);
            network_error("operation cancelled");
        }
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const SocketHandle client =
            to_handle(accept(from_handle(listener), reinterpret_cast<sockaddr*>(&address), &length));
        if (client != kInvalidSocket) {
            set_nonblocking(listener, false);
            return client;
        }
        const int error = last_socket_error();
        if (!is_would_block(error) && !is_interrupted(error)) {
            set_nonblocking(listener, false);
            network_error("accept failed: " + socket_error_message(error));
        }
        if (poll_socket(listener, true, false, kPollIntervalMs)) {
            continue;
        }
        wait_or_cancel(cancelled);
    }
}

std::vector<std::uint8_t> tcp_read_sync(SocketHandle socket, std::atomic<bool>* cancelled) {
    if (socket == kInvalidSocket) {
        network_error("read on invalid connection");
    }
    if (cancelled == nullptr) {
        set_blocking_mode(socket, true);
        std::vector<std::uint8_t> buffer(kTcpReadChunk);
        const int received =
            recv(from_handle(socket), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
        if (received > 0) {
            buffer.resize(static_cast<std::size_t>(received));
            return buffer;
        }
        if (received == 0) {
            network_error("connection closed");
        }
        network_error("read failed: " + socket_error_message(last_socket_error()));
    }
    set_nonblocking(socket, true);
    while (true) {
        if (is_cancelled(cancelled)) {
            set_nonblocking(socket, false);
            network_error("operation cancelled");
        }
        if (poll_socket(socket, true, false, kPollIntervalMs)) {
            std::vector<std::uint8_t> buffer(kTcpReadChunk);
            const int received =
                recv(from_handle(socket), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (received > 0) {
                buffer.resize(static_cast<std::size_t>(received));
                set_nonblocking(socket, false);
                return buffer;
            }
            if (received == 0) {
                set_nonblocking(socket, false);
                network_error("connection closed");
            }
            const int error = last_socket_error();
            if (!is_would_block(error) && !is_interrupted(error)) {
                set_nonblocking(socket, false);
                network_error("read failed: " + socket_error_message(error));
            }
        } else {
            wait_or_cancel(cancelled);
        }
    }
}

std::size_t tcp_write_sync(SocketHandle socket, const std::uint8_t* data, std::size_t length) {
    if (socket == kInvalidSocket) {
        network_error("write on invalid connection");
    }
    set_blocking_mode(socket, true);
    std::size_t written_total = 0;
    while (written_total < length) {
        const int written = send(from_handle(socket), reinterpret_cast<const char*>(data + written_total),
                                 static_cast<int>(length - written_total), 0);
        if (written > 0) {
            written_total += static_cast<std::size_t>(written);
            continue;
        }
        if (written == 0) {
            network_error("write failed: connection closed");
        }
        const int error = last_socket_error();
        network_error("write failed: " + socket_error_message(error));
    }
    return written_total;
}

SocketHandle udp_open_sync() {
    net_platform_init();
    const SocketHandle socket = to_handle(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (socket == kInvalidSocket) {
        network_error("failed to create UDP socket");
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = 0;
    if (bind(from_handle(socket), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        net_close_socket(socket);
        network_error("failed to bind UDP socket");
    }
    return socket;
}

int socket_local_port(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        network_error("local_port on invalid socket");
    }
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(from_handle(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        network_error("failed to query local port");
    }
    return address_to_port(reinterpret_cast<sockaddr*>(&address), length);
}

void udp_send_sync(SocketHandle socket, const std::uint8_t* data, std::size_t length, const std::string& host,
                   int port) {
    validate_network_host(host, "send");
    validate_network_port(port, "send");
    if (socket == kInvalidSocket) {
        network_error("send on invalid UDP socket");
    }
    net_platform_init();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo* results = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
        if (results != nullptr) {
            freeaddrinfo(results);
        }
#ifdef _WIN32
        network_error("DNS resolution failed for '" + host + "': " + gai_strerrorA(rc));
#else
        network_error("DNS resolution failed for '" + host + "': " + gai_strerror(rc));
#endif
    }
    const int sent = sendto(from_handle(socket), reinterpret_cast<const char*>(data), static_cast<int>(length), 0,
                            results->ai_addr, static_cast<int>(results->ai_addrlen));
    freeaddrinfo(results);
    if (sent < 0) {
        network_error("UDP send failed: " + socket_error_message(last_socket_error()));
    }
    if (static_cast<std::size_t>(sent) != length) {
        network_error("UDP send failed: partial datagram write");
    }
}

NetUdpPacket udp_receive_sync(SocketHandle socket, std::atomic<bool>* cancelled) {
    if (socket == kInvalidSocket) {
        network_error("receive on invalid UDP socket");
    }
    if (cancelled == nullptr) {
        set_blocking_mode(socket, true);
        std::vector<std::uint8_t> buffer(kUdpMaxDatagram);
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const int received = recvfrom(from_handle(socket), reinterpret_cast<char*>(buffer.data()),
                                      static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&address),
                                      &length);
        if (received > 0) {
            buffer.resize(static_cast<std::size_t>(received));
            NetUdpPacket packet;
            packet.data = std::move(buffer);
            packet.host = address_to_host(reinterpret_cast<sockaddr*>(&address), length);
            packet.port = address_to_port(reinterpret_cast<sockaddr*>(&address), length);
            return packet;
        }
        network_error("UDP receive failed: " + socket_error_message(last_socket_error()));
    }
    set_nonblocking(socket, true);
    while (true) {
        if (is_cancelled(cancelled)) {
            set_nonblocking(socket, false);
            network_error("operation cancelled");
        }
        if (poll_socket(socket, true, false, kPollIntervalMs)) {
            std::vector<std::uint8_t> buffer(kUdpMaxDatagram);
            sockaddr_storage address{};
            socklen_t length = sizeof(address);
            const int received = recvfrom(from_handle(socket), reinterpret_cast<char*>(buffer.data()),
                                          static_cast<int>(buffer.size()), 0, reinterpret_cast<sockaddr*>(&address),
                                          &length);
            if (received > 0) {
                buffer.resize(static_cast<std::size_t>(received));
                NetUdpPacket packet;
                packet.data = std::move(buffer);
                packet.host = address_to_host(reinterpret_cast<sockaddr*>(&address), length);
                packet.port = address_to_port(reinterpret_cast<sockaddr*>(&address), length);
                set_nonblocking(socket, false);
                return packet;
            }
            if (received == 0) {
                continue;
            }
            const int error = last_socket_error();
            if (!is_would_block(error) && !is_interrupted(error)) {
                set_nonblocking(socket, false);
                network_error("UDP receive failed: " + socket_error_message(error));
            }
        } else {
            wait_or_cancel(cancelled);
        }
    }
}

std::vector<std::string> dns_resolve_sync(const std::string& host) {
    validate_network_host(host, "resolve");
    net_platform_init();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &results);
    if (rc != 0 || results == nullptr) {
        if (results != nullptr) {
            freeaddrinfo(results);
        }
#ifdef _WIN32
        network_error("DNS resolution failed for '" + host + "': " + gai_strerrorA(rc));
#else
        network_error("DNS resolution failed for '" + host + "': " + gai_strerror(rc));
#endif
    }

    std::vector<std::string> addresses;
    std::unordered_set<std::string> seen;
    for (const addrinfo* cursor = results; cursor != nullptr; cursor = cursor->ai_next) {
        std::array<char, NI_MAXHOST> buffer{};
        if (getnameinfo(cursor->ai_addr, static_cast<socklen_t>(cursor->ai_addrlen), buffer.data(),
                        static_cast<socklen_t>(buffer.size()), nullptr, 0, NI_NUMERICHOST) != 0) {
            continue;
        }
        const std::string address = buffer.data();
        if (seen.insert(address).second) {
            addresses.push_back(address);
        }
    }
    freeaddrinfo(results);
    if (addresses.empty()) {
        network_error("DNS resolution failed for '" + host + "': no addresses found");
    }
    std::sort(addresses.begin(), addresses.end());
    return addresses;
}

Value net_make_connection(std::uint32_t class_index, std::uint32_t kind, SocketHandle socket) {
    auto data = std::make_shared<NativeNetworkData>();
    data->kind = kind;
    data->socket = socket;
    data->closed = false;
    Value value = Runtime::make_class(class_index, "Connection", std::vector<std::string>{});
    value.class_value->native_network = std::move(data);
    return value;
}

Value net_make_packet(std::uint32_t class_index, const NetUdpPacket& packet) {
    Value value = Runtime::make_class(class_index, "Packet", {"data", "host", "port"});
    Runtime::set_field(value, 0, Runtime::make_bytes(packet.data));
    Runtime::set_field(value, 1, Runtime::make_string(packet.host));
    Runtime::set_field(value, 2, Runtime::make_integer(packet.port));
    return value;
}

}  // namespace pscript
