#pragma once

#include "runtime/runtime.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace pscript {

using SocketHandle = std::uintptr_t;
constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(~static_cast<SocketHandle>(0));

struct NetUdpPacket {
    std::vector<std::uint8_t> data;
    std::string host;
    int port = 0;
};

void net_platform_init();
void net_close_socket(SocketHandle socket);

SocketHandle tcp_connect_sync(const std::string& host, int port, std::atomic<bool>* cancelled = nullptr);
SocketHandle tcp_listen_sync(const std::string& host, int port);
SocketHandle tcp_accept_sync(SocketHandle listener, std::atomic<bool>* cancelled = nullptr);
std::vector<std::uint8_t> tcp_read_sync(SocketHandle socket, std::atomic<bool>* cancelled = nullptr);
std::size_t tcp_write_sync(SocketHandle socket, const std::uint8_t* data, std::size_t length);
SocketHandle udp_open_sync();
int socket_local_port(SocketHandle socket);
void udp_send_sync(SocketHandle socket, const std::uint8_t* data, std::size_t length, const std::string& host,
                   int port);
NetUdpPacket udp_receive_sync(SocketHandle socket, std::atomic<bool>* cancelled = nullptr);
std::vector<std::string> dns_resolve_sync(const std::string& host);

Value net_make_connection(std::uint32_t class_index, std::uint32_t kind, SocketHandle socket);
Value net_make_packet(std::uint32_t class_index, const NetUdpPacket& packet);

void validate_network_port(std::int64_t port, std::string_view operation);
void validate_network_host(const std::string& host, std::string_view operation);

[[noreturn]] void network_error(std::string message);

}  // namespace pscript
