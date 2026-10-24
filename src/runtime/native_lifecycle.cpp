#include "runtime/runtime.h"

#include "runtime/net_ops.h"

#include <fstream>

namespace pscript {

void close_native_file_handle(NativeFileHandleData& data) {
    if (data.closed) {
        return;
    }
    if (data.stream && data.stream->is_open()) {
        data.stream->close();
    }
    data.closed = true;
}

void close_native_network(NativeNetworkData& data) {
    if (data.closed) {
        return;
    }
    if (data.socket != static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0))) {
        net_close_socket(static_cast<SocketHandle>(data.socket));
    }
    data.closed = true;
    data.socket = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
}

NativeFileHandleData::~NativeFileHandleData() {
    close_native_file_handle(*this);
}

NativeNetworkData::~NativeNetworkData() {
    close_native_network(*this);
}

}  // namespace pscript
