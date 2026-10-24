#pragma once

#include "runtime/net_ops.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pscript {

struct TlsConnection {
    SocketHandle socket = kInvalidSocket;
    std::shared_ptr<void> state;
};

bool http_tls_available();

TlsConnection tls_client_connect(SocketHandle socket, const std::string& host, std::atomic<bool>* cancelled);
void tls_write_all(const TlsConnection& connection, const std::uint8_t* data, std::size_t length,
                   std::atomic<bool>* cancelled);
std::vector<std::uint8_t> tls_read_http_message(const TlsConnection& connection, std::atomic<bool>* cancelled);
void tls_close(TlsConnection& connection);

bool http_run_tls_self_test();

}  // namespace pscript
