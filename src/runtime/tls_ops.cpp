#include "runtime/tls_ops.h"

#include "runtime/http_ops.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace pscript {
namespace {

[[noreturn]] void tls_fail(std::string message) {
    network_error(std::move(message));
}

bool cancelled(const std::atomic<bool>* flag) {
    return flag != nullptr && flag->load();
}

}  // namespace

bool http_tls_available() {
#if defined(_WIN32) || defined(PSCRIPT_HAS_OPENSSL)
    return true;
#else
    return false;
#endif
}

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <schannel.h>

namespace {

struct SchannelState {
    CredHandle credentials{};
    CtxtHandle context{};
    bool context_initialized = false;
    SecPkgContext_StreamSizes sizes{};
    std::vector<std::uint8_t> extra;
};

SchannelState* state_ptr(const TlsConnection& connection) {
    return static_cast<SchannelState*>(connection.state.get());
}

void free_schannel(SchannelState* state) {
    if (state == nullptr) {
        return;
    }
    if (state->context_initialized) {
        DeleteSecurityContext(&state->context);
    }
    FreeCredentialsHandle(&state->credentials);
    delete state;
}

std::vector<std::uint8_t> encrypt_payload(SchannelState* state, const std::uint8_t* data, std::size_t length) {
    std::vector<std::uint8_t> buffer(state->sizes.cbHeader + length + state->sizes.cbTrailer);
    std::memcpy(buffer.data() + state->sizes.cbHeader, data, length);
    SecBuffer buffers[4];
    buffers[0].BufferType = SECBUFFER_STREAM_HEADER;
    buffers[0].pvBuffer = buffer.data();
    buffers[0].cbBuffer = state->sizes.cbHeader;
    buffers[1].BufferType = SECBUFFER_DATA;
    buffers[1].pvBuffer = buffer.data() + state->sizes.cbHeader;
    buffers[1].cbBuffer = static_cast<unsigned long>(length);
    buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;
    buffers[2].pvBuffer = buffer.data() + state->sizes.cbHeader + length;
    buffers[2].cbBuffer = state->sizes.cbTrailer;
    buffers[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
    if (EncryptMessage(&state->context, 0, &desc, 0) != SEC_E_OK) {
        tls_fail("TLS encrypt failed");
    }
    buffer.resize(buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);
    return buffer;
}

SECURITY_STATUS handshake_step(SchannelState* state, SocketHandle socket, const std::string& host,
                               std::vector<std::uint8_t>& io_buffer, ULONG* attrs_out,
                               std::atomic<bool>* cancelled_flag) {
    SecBuffer buffers[2];
    SecBufferDesc input_desc{SECBUFFER_VERSION, 0, nullptr};
    if (!io_buffer.empty()) {
        buffers[0].BufferType = SECBUFFER_TOKEN;
        buffers[0].pvBuffer = io_buffer.data();
        buffers[0].cbBuffer = static_cast<unsigned long>(io_buffer.size());
        input_desc.cBuffers = 1;
        input_desc.pBuffers = buffers;
    }

    SecBuffer out_buffers[1];
    out_buffers[0].BufferType = SECBUFFER_TOKEN;
    out_buffers[0].pvBuffer = nullptr;
    out_buffers[0].cbBuffer = 0;
    SecBufferDesc output_desc{SECBUFFER_VERSION, 1, out_buffers};

    const std::wstring whost(host.begin(), host.end());
    const SECURITY_STATUS status = InitializeSecurityContextW(
        &state->credentials, state->context_initialized ? &state->context : nullptr, const_cast<wchar_t*>(whost.c_str()),
        ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY |
            ISC_REQ_STREAM,
        0, SECURITY_NATIVE_DREP, input_desc.cBuffers > 0 ? &input_desc : nullptr, 0, &state->context, &output_desc,
        attrs_out, nullptr);
    state->context_initialized = true;

    if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS) {
        if (out_buffers[0].cbBuffer > 0 && out_buffers[0].pvBuffer != nullptr) {
            tcp_write_sync(socket, static_cast<const std::uint8_t*>(out_buffers[0].pvBuffer), out_buffers[0].cbBuffer);
            FreeContextBuffer(out_buffers[0].pvBuffer);
        }
    }
    if (status == SEC_E_INCOMPLETE_MESSAGE) {
        const std::vector<std::uint8_t> chunk = tcp_read_sync(socket, cancelled_flag);
        io_buffer.insert(io_buffer.end(), chunk.begin(), chunk.end());
    }
    return status;
}

bool decrypt_available(SchannelState* state, std::vector<std::uint8_t>& decrypted) {
    if (state->extra.empty()) {
        return false;
    }
    SecBuffer buffers[4];
    buffers[0].BufferType = SECBUFFER_DATA;
    buffers[0].pvBuffer = state->extra.data();
    buffers[0].cbBuffer = static_cast<unsigned long>(state->extra.size());
    buffers[1].BufferType = SECBUFFER_EMPTY;
    buffers[2].BufferType = SECBUFFER_EMPTY;
    buffers[3].BufferType = SECBUFFER_EMPTY;
    SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
    const SECURITY_STATUS status = DecryptMessage(&state->context, &desc, 0, nullptr);
    if (status == SEC_E_INCOMPLETE_MESSAGE) {
        return false;
    }
    if (status != SEC_E_OK) {
        tls_fail("TLS decrypt failed");
    }
    for (int i = 0; i < 4; ++i) {
        if (buffers[i].BufferType == SECBUFFER_DATA && buffers[i].cbBuffer > 0) {
            const auto* start = static_cast<const std::uint8_t*>(buffers[i].pvBuffer);
            decrypted.insert(decrypted.end(), start, start + buffers[i].cbBuffer);
        }
    }
    if (buffers[1].BufferType == SECBUFFER_EXTRA && buffers[1].cbBuffer > 0) {
        const auto* start = static_cast<const std::uint8_t*>(buffers[1].pvBuffer);
        state->extra.assign(start, start + buffers[1].cbBuffer);
    } else {
        state->extra.clear();
    }
    return true;
}

}  // namespace

TlsConnection tls_client_connect(SocketHandle socket, const std::string& host, std::atomic<bool>* cancelled_flag) {
    auto state = std::make_shared<SchannelState>();
    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
    cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;
    if (AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &cred,
                                  nullptr, nullptr, &state->credentials, nullptr) != SEC_E_OK) {
        tls_fail("TLS credentials initialization failed");
    }

    std::vector<std::uint8_t> io_buffer;
    SECURITY_STATUS status = SEC_I_CONTINUE_NEEDED;
    while (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_INCOMPLETE_MESSAGE) {
        if (cancelled(cancelled_flag)) {
            tls_fail("operation cancelled");
        }
        ULONG attrs = 0;
        status = handshake_step(state.get(), socket, host, io_buffer, &attrs, cancelled_flag);
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            continue;
        }
        if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED && status != SEC_I_INCOMPLETE_CREDENTIALS) {
            tls_fail("TLS handshake failed");
        }
        if (status == SEC_E_OK) {
            break;
        }
        if (io_buffer.empty()) {
            const std::vector<std::uint8_t> chunk = tcp_read_sync(socket, cancelled_flag);
            io_buffer.insert(io_buffer.end(), chunk.begin(), chunk.end());
        }
    }

    SecPkgContext_StreamSizes sizes{};
    if (QueryContextAttributes(&state->context, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
        tls_fail("TLS stream setup failed");
    }
    state->sizes = sizes;
    return TlsConnection{socket, std::move(state)};
}

void tls_write_all(const TlsConnection& connection, const std::uint8_t* data, std::size_t length,
                   std::atomic<bool>* cancelled_flag) {
    SchannelState* state = state_ptr(connection);
    std::size_t offset = 0;
    while (offset < length) {
        if (cancelled(cancelled_flag)) {
            tls_fail("operation cancelled");
        }
        const std::size_t chunk = (std::min)(length - offset, static_cast<std::size_t>(state->sizes.cbMaximumMessage));
        const std::vector<std::uint8_t> encrypted = encrypt_payload(state, data + offset, chunk);
        tcp_write_sync(connection.socket, encrypted.data(), encrypted.size());
        offset += chunk;
    }
}

std::vector<std::uint8_t> tls_read_http_message(const TlsConnection& connection, std::atomic<bool>* cancelled_flag) {
    SchannelState* state = state_ptr(connection);
    std::vector<std::uint8_t> decrypted;
    std::size_t target = 0;
    while (!http_message_size(decrypted, &target)) {
        if (cancelled(cancelled_flag)) {
            tls_fail("operation cancelled");
        }
        if (state->extra.empty()) {
            const std::vector<std::uint8_t> chunk = tcp_read_sync(connection.socket, cancelled_flag);
            state->extra.insert(state->extra.end(), chunk.begin(), chunk.end());
        }
        if (!decrypt_available(state, decrypted)) {
            const std::vector<std::uint8_t> chunk = tcp_read_sync(connection.socket, cancelled_flag);
            state->extra.insert(state->extra.end(), chunk.begin(), chunk.end());
        }
        if (decrypted.size() > 16 * 1024 * 1024) {
            tls_fail("HTTP message too large");
        }
    }
    decrypted.resize(target);
    return decrypted;
}

void tls_close(TlsConnection& connection) {
    if (connection.state) {
        free_schannel(state_ptr(connection));
        connection.state.reset();
    }
}

bool http_run_tls_self_test() {
    return http_tls_available();
}

#elif defined(PSCRIPT_HAS_OPENSSL)

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <poll.h>

namespace {

constexpr int kTlsPollIntervalMs = 50;

struct OpenSslTlsState {
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;

    ~OpenSslTlsState() {
        if (ssl != nullptr) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
        }
    }
};

OpenSslTlsState* state_ptr(const TlsConnection& connection) {
    return static_cast<OpenSslTlsState*>(connection.state.get());
}

int socket_fd(SocketHandle socket) { return static_cast<int>(socket); }

void wait_for_ssl_io(SocketHandle socket, bool want_read) {
    pollfd descriptor{};
    descriptor.fd = socket_fd(socket);
    descriptor.events = want_read ? POLLIN : POLLOUT;
    poll(&descriptor, 1, kTlsPollIntervalMs);
}

bool ssl_io_ready(SSL* ssl, int ret, bool& want_read) {
    const int error = SSL_get_error(ssl, ret);
    if (error == SSL_ERROR_WANT_READ) {
        want_read = true;
        return false;
    }
    if (error == SSL_ERROR_WANT_WRITE) {
        want_read = false;
        return false;
    }
    return true;
}

}  // namespace

TlsConnection tls_client_connect(SocketHandle socket, const std::string& host, std::atomic<bool>* cancelled_flag) {
    auto state = std::make_shared<OpenSslTlsState>();
    state->ctx = SSL_CTX_new(TLS_client_method());
    if (state->ctx == nullptr) {
        tls_fail("TLS context initialization failed");
    }
    state->ssl = SSL_new(state->ctx);
    if (state->ssl == nullptr) {
        tls_fail("TLS session initialization failed");
    }
    if (SSL_set_fd(state->ssl, socket_fd(socket)) != 1) {
        tls_fail("TLS socket binding failed");
    }
    if (SSL_set_tlsext_host_name(state->ssl, host.c_str()) != 1) {
        tls_fail("TLS SNI setup failed");
    }
    if (SSL_set1_host(state->ssl, host.c_str()) != 1) {
        tls_fail("TLS host verification setup failed");
    }
    SSL_set_verify(state->ssl, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_default_verify_paths(state->ctx) != 1) {
        tls_fail("TLS trust store setup failed");
    }

    for (;;) {
        if (cancelled(cancelled_flag)) {
            tls_fail("operation cancelled");
        }
        const int ret = SSL_connect(state->ssl);
        if (ret == 1) {
            break;
        }
        bool want_read = true;
        if (ssl_io_ready(state->ssl, ret, want_read)) {
            tls_fail("TLS handshake failed");
        }
        wait_for_ssl_io(socket, want_read);
    }

    return TlsConnection{socket, std::move(state)};
}

void tls_write_all(const TlsConnection& connection, const std::uint8_t* data, std::size_t length,
                   std::atomic<bool>* cancelled_flag) {
    OpenSslTlsState* state = state_ptr(connection);
    std::size_t offset = 0;
    while (offset < length) {
        if (cancelled(cancelled_flag)) {
            tls_fail("operation cancelled");
        }
        const int ret = SSL_write(state->ssl, data + offset, static_cast<int>(length - offset));
        if (ret > 0) {
            offset += static_cast<std::size_t>(ret);
            continue;
        }
        bool want_read = true;
        if (ssl_io_ready(state->ssl, ret, want_read)) {
            tls_fail("TLS write failed");
        }
        wait_for_ssl_io(connection.socket, want_read);
    }
}

std::vector<std::uint8_t> tls_read_http_message(const TlsConnection& connection, std::atomic<bool>* cancelled_flag) {
    OpenSslTlsState* state = state_ptr(connection);
    std::vector<std::uint8_t> decrypted;
    std::size_t target = 0;
    std::array<std::uint8_t, 4096> chunk{};
    while (!http_message_size(decrypted, &target)) {
        if (cancelled(cancelled_flag)) {
            tls_fail("operation cancelled");
        }
        const int ret = SSL_read(state->ssl, chunk.data(), static_cast<int>(chunk.size()));
        if (ret > 0) {
            decrypted.insert(decrypted.end(), chunk.begin(), chunk.begin() + ret);
            if (decrypted.size() > 16 * 1024 * 1024) {
                tls_fail("HTTP message too large");
            }
            continue;
        }
        if (ret == 0) {
            tls_fail("TLS connection closed while reading HTTP message");
        }
        bool want_read = true;
        if (ssl_io_ready(state->ssl, ret, want_read)) {
            tls_fail("TLS read failed");
        }
        wait_for_ssl_io(connection.socket, want_read);
    }
    decrypted.resize(target);
    return decrypted;
}

void tls_close(TlsConnection& connection) { connection.state.reset(); }

bool http_run_tls_self_test() { return http_tls_available(); }

#else

TlsConnection tls_client_connect(SocketHandle, const std::string&, std::atomic<bool>*) {
    tls_fail("HTTPS is not available on this platform build");
}

void tls_write_all(const TlsConnection&, const std::uint8_t*, std::size_t, std::atomic<bool>*) {
    tls_fail("HTTPS is not available on this platform build");
}

std::vector<std::uint8_t> tls_read_http_message(const TlsConnection&, std::atomic<bool>*) {
    tls_fail("HTTPS is not available on this platform build");
    return {};
}

void tls_close(TlsConnection&) {}

bool http_run_tls_self_test() { return false; }

#endif

}  // namespace pscript
