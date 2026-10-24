#include "runtime/async_io.h"

#include "runtime/sql_ops.h"
#include "runtime/sql_session.h"
#include "runtime/sql_transaction.h"

#include "runtime/http_ops.h"
#include "runtime/net_ops.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

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
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace pscript {
namespace {

[[noreturn]] void async_io_fail(const std::string& message) {
    throw RuntimeError(message, RuntimeErrorCategory::Runtime);
}

void require_async_scheduler(const AsyncSchedulerHooks& hooks) {
    if (!hooks.scheduler_active || !hooks.scheduler_active()) {
        async_io_fail("async I/O requires an active async scheduler");
    }
}

#ifdef _WIN32
struct WinsockInit {
    WinsockInit() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            async_io_fail("failed to initialize Winsock");
        }
    }
    ~WinsockInit() { WSACleanup(); }
};

const WinsockInit& winsock_init() {
    static const WinsockInit init;
    return init;
}

using TestSocketHandle = SOCKET;
constexpr TestSocketHandle kTestInvalidSocket = INVALID_SOCKET;

void close_socket(TestSocketHandle socket) {
    if (socket != kTestInvalidSocket) {
        closesocket(socket);
    }
}

bool poll_socket_readable(TestSocketHandle socket, int timeout_ms) {
    winsock_init();
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(socket, &readfds);
    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select(0, &readfds, nullptr, nullptr, &timeout);
    return ready > 0 && FD_ISSET(socket, &readfds);
}

bool create_loopback_socket_pair(TestSocketHandle out[2]) {
    winsock_init();
    TestSocketHandle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kTestInvalidSocket) {
        return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(listener);
        return false;
    }
    int address_length = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length) != 0) {
        close_socket(listener);
        return false;
    }
    if (listen(listener, 1) != 0) {
        close_socket(listener);
        return false;
    }
    out[0] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (out[0] == kTestInvalidSocket) {
        close_socket(listener);
        return false;
    }
    if (connect(out[0], reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close_socket(out[0]);
        close_socket(listener);
        return false;
    }
    out[1] = accept(listener, nullptr, nullptr);
    close_socket(listener);
    if (out[1] == kTestInvalidSocket) {
        close_socket(out[0]);
        return false;
    }
    return true;
}

#else

using TestSocketHandle = int;
constexpr TestSocketHandle kTestInvalidSocket = -1;

void close_socket(TestSocketHandle socket) {
    if (socket >= 0) {
        close(socket);
    }
}

bool poll_socket_readable(TestSocketHandle socket, int timeout_ms) {
    pollfd descriptor{};
    descriptor.fd = socket;
    descriptor.events = POLLIN;
    const int ready = poll(&descriptor, 1, timeout_ms);
    return ready > 0 && (descriptor.revents & POLLIN) != 0;
}

bool create_loopback_socket_pair(TestSocketHandle out[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, out) != 0) {
        return false;
    }
    return true;
}

#endif

std::string read_file_text_sync(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        async_io_fail("read failed for '" + path.generic_string() + "': file not found or not readable");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

void write_file_text_sync(const std::filesystem::path& path, std::string_view content, bool append) {
    const auto mode = append ? (std::ios::binary | std::ios::app) : (std::ios::binary | std::ios::trunc);
    std::ofstream out(path, mode);
    if (!out) {
        async_io_fail(std::string(append ? "append" : "write") + " failed for '" + path.generic_string() +
                      "': file not writable");
    }
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!out) {
        async_io_fail(std::string(append ? "append" : "write") + " failed for '" + path.generic_string() +
                      "': write failed");
    }
}

void validate_fs_read_path(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        if (std::filesystem::is_directory(path)) {
            async_io_fail("read failed for '" + path.generic_string() + "': path is a directory");
        }
        async_io_fail("read failed for '" + path.generic_string() + "': file not found");
    }
}

void validate_fs_write_path(const std::filesystem::path& path) {
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path)) {
        async_io_fail(std::string("write failed for '") + path.generic_string() + "': path is a directory");
    }
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        async_io_fail(std::string("write failed for '") + path.generic_string() +
                      "': parent directory does not exist");
    }
}

struct OpenMode {
    std::ios::openmode mode = std::ios::in;
    bool can_read = false;
    bool can_write = false;
};

OpenMode parse_handle_mode(std::string_view mode_text, const std::string& path) {
    OpenMode parsed;
    if (mode_text == "r") {
        parsed.mode = std::ios::in;
        parsed.can_read = true;
    } else if (mode_text == "w") {
        parsed.mode = std::ios::out | std::ios::trunc;
        parsed.can_write = true;
    } else if (mode_text == "a") {
        parsed.mode = std::ios::out | std::ios::app;
        parsed.can_write = true;
    } else if (mode_text == "r+") {
        parsed.mode = std::ios::in | std::ios::out;
        parsed.can_read = true;
        parsed.can_write = true;
    } else if (mode_text == "w+") {
        parsed.mode = std::ios::in | std::ios::out | std::ios::trunc;
        parsed.can_read = true;
        parsed.can_write = true;
    } else if (mode_text == "a+") {
        parsed.mode = std::ios::in | std::ios::out | std::ios::app;
        parsed.can_read = true;
        parsed.can_write = true;
    } else {
        async_io_fail("unsupported file open mode '" + std::string(mode_text) + "'");
    }
    if (path.empty()) {
        async_io_fail("file path must not be empty");
    }
    return parsed;
}

NativeFileHandleData& require_open_handle(NativeFileHandleData& data) {
    if (data.closed || !data.stream || !data.stream->is_open()) {
        async_io_fail("file handle is closed");
    }
    return data;
}

}  // namespace

AsyncIoService::AsyncIoService(Runtime& runtime) : runtime_(runtime) {}

AsyncIoService::~AsyncIoService() {
    shutdown();
}

void AsyncIoService::set_hooks(AsyncSchedulerHooks hooks) {
    std::lock_guard lock(mutex_);
    hooks_ = std::move(hooks);
}

void AsyncIoService::clear_hooks() {
    std::lock_guard lock(mutex_);
    hooks_ = {};
}

void AsyncIoService::ensure_worker_started() {
    if (running_.load()) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (running_.load()) {
        return;
    }
    shutdown_requested_ = false;
    running_ = true;
    worker_ = std::thread([this]() { worker_main(); });
}

bool AsyncIoService::uses_dedicated_thread(IoKind kind) const {
    switch (kind) {
        case IoKind::NetConnect:
        case IoKind::NetRead:
        case IoKind::NetWrite:
        case IoKind::NetAccept:
        case IoKind::NetUdpSend:
        case IoKind::NetUdpReceive:
        case IoKind::NetResolve:
        case IoKind::HttpRequest:
            return true;
        default:
            return false;
    }
}

void AsyncIoService::run_work_item(const std::shared_ptr<IoWorkItem>& work) {
    AsyncIoCompletion completion;
    completion.task = work->task.lock();
    execute_work(work, completion);

    {
        std::lock_guard lock(mutex_);
        active_operations_.erase(std::remove(active_operations_.begin(), active_operations_.end(), work),
                                 active_operations_.end());
    }

    if (!completion.task) {
        std::lock_guard lock(mutex_);
        if (in_flight_count_ > 0) {
            --in_flight_count_;
        }
        cv_.notify_all();
        return;
    }

    if (work->cancelled.load(std::memory_order_acquire) || completion.task->cancel_requested) {
        completion.cancelled = true;
        completion.success = false;
        if (work->kind == IoKind::SqlOperation && work->sql_transaction && work->sql_transaction->is_active()) {
            try {
                work->sql_transaction->rollback();
            } catch (...) {
            }
        }
    }

    push_completion(std::move(completion));
}

void AsyncIoService::dispatch_work(std::shared_ptr<IoWorkItem> work) {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_requested_.load()) {
            return;
        }
        active_operations_.push_back(work);
        ++in_flight_count_;
    }
    std::thread([this, work = std::move(work)]() mutable { run_work_item(work); }).detach();
}

void AsyncIoService::enqueue_work(std::shared_ptr<IoWorkItem> work) {
    {
        std::lock_guard lock(mutex_);
        if (shutdown_requested_.load()) {
            return;
        }
        work_queue_.push_back(std::move(work));
        ++in_flight_count_;
    }
    cv_.notify_one();
}

void AsyncIoService::worker_main() {
    while (true) {
        std::shared_ptr<IoWorkItem> work;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() { return shutdown_requested_.load() || !work_queue_.empty(); });
            if (shutdown_requested_.load() && work_queue_.empty()) {
                break;
            }
            if (work_queue_.empty()) {
                continue;
            }
            work = std::move(work_queue_.front());
            work_queue_.pop_front();
            active_operations_.push_back(work);
        }
        run_work_item(work);
    }
}

void AsyncIoService::push_completion(AsyncIoCompletion completion) {
    {
        std::lock_guard lock(mutex_);
        completions_.push_back(std::move(completion));
        if (in_flight_count_ > 0) {
            --in_flight_count_;
        }
    }
    cv_.notify_one();
    cv_.notify_all();
}

Value AsyncIoService::submit_task(IoKind kind, const std::shared_ptr<IoWorkItem>& work, bool defer_until_scheduled) {
    require_async_scheduler(hooks_);
    auto task = std::make_shared<TaskData>();
    task->state = TaskState::Pending;
    task->function_index = 0xFFFFFFFFu;
    task->io_operation_id = work->id;
    work->task = task;
    if (hooks_.track_task) {
        hooks_.track_task(task);
    }
    if (uses_dedicated_thread(kind)) {
        dispatch_work(work);
        return Runtime::make_task(std::move(task));
    }
    if (!defer_until_scheduled) {
        ensure_worker_started();
        enqueue_work(work);
        return Runtime::make_task(std::move(task));
    }
    std::lock_guard lock(mutex_);
    deferred_operations_[work->id] = work;
    return Runtime::make_task(std::move(task));
}

Value AsyncIoService::submit_fs_read(const std::string& path) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::FsRead;
    work->path = path;
    return submit_task(IoKind::FsRead, work);
}

Value AsyncIoService::submit_fs_write(const std::string& path, std::string content, bool append) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = append ? IoKind::FsAppend : IoKind::FsWrite;
    work->path = path;
    work->content = std::move(content);
    return submit_task(work->kind, work);
}

Value AsyncIoService::submit_handle_read(const std::shared_ptr<NativeFileHandleData>& handle) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::HandleRead;
    work->handle = handle;
    return submit_task(IoKind::HandleRead, work);
}

Value AsyncIoService::submit_handle_write(const std::shared_ptr<NativeFileHandleData>& handle,
                                          std::string content) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::HandleWrite;
    work->handle = handle;
    work->content = std::move(content);
    return submit_task(IoKind::HandleWrite, work);
}

Value AsyncIoService::submit_stdin_line(std::optional<std::string> prompt) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::StdinLine;
    work->prompt = std::move(prompt);
    return submit_task(IoKind::StdinLine, work, true);
}

Value AsyncIoService::submit_net_connect(const std::string& host, int port, std::uint32_t connection_class_index) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::NetConnect;
    work->path = host;
    work->port = port;
    work->class_index = connection_class_index;
    return submit_task(IoKind::NetConnect, work);
}

Value AsyncIoService::submit_net_read(const std::shared_ptr<NativeNetworkData>& connection) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::NetRead;
    work->network = connection;
    return submit_task(IoKind::NetRead, work);
}

Value AsyncIoService::submit_net_write(const std::shared_ptr<NativeNetworkData>& connection,
                                       std::vector<std::uint8_t> data) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::NetWrite;
    work->network = connection;
    work->bytes = std::move(data);
    return submit_task(IoKind::NetWrite, work);
}

Value AsyncIoService::submit_net_accept(const std::shared_ptr<NativeNetworkData>& server,
                                        std::uint32_t connection_class_index) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::NetAccept;
    work->network = server;
    work->class_index = connection_class_index;
    return submit_task(IoKind::NetAccept, work);
}

Value AsyncIoService::submit_net_udp_send(const std::shared_ptr<NativeNetworkData>& socket,
                                          std::vector<std::uint8_t> data, const std::string& host, int port) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::NetUdpSend;
    work->network = socket;
    work->bytes = std::move(data);
    work->path = host;
    work->port = port;
    return submit_task(IoKind::NetUdpSend, work);
}

Value AsyncIoService::submit_net_udp_receive(const std::shared_ptr<NativeNetworkData>& socket,
                                             std::uint32_t packet_class_index) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::NetUdpReceive;
    work->network = socket;
    work->class_index = packet_class_index;
    return submit_task(IoKind::NetUdpReceive, work);
}

Value AsyncIoService::submit_net_resolve(const std::string& host) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::NetResolve;
    work->path = host;
    return submit_task(IoKind::NetResolve, work);
}

Value AsyncIoService::submit_sql_operation(std::uint32_t operation, const std::shared_ptr<NativeDatabaseData>& database,
                                           const std::string& sql, const std::vector<Value>& params,
                                           const std::shared_ptr<SqlTransactionState>& transaction,
                                           std::uint32_t statement_class_index, std::uint32_t rows_class_index) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::SqlOperation;
    work->sql_database = database;
    work->sql_transaction = transaction;
    work->content = sql;
    work->sql_params = params;
    work->sql_operation = operation;
    work->sql_statement_class_index = statement_class_index;
    work->sql_rows_class_index = rows_class_index;
    return submit_task(IoKind::SqlOperation, work);
}

Value AsyncIoService::submit_http_request(const std::string& method, const std::string& url,
                                          std::vector<std::uint8_t> body,
                                          const std::map<std::string, std::string>& headers,
                                          std::uint32_t response_class_index) {
    auto work = std::make_shared<IoWorkItem>();
    work->id = next_operation_id_.fetch_add(1);
    work->kind = IoKind::HttpRequest;
    work->content = method;
    work->path = url;
    work->bytes = std::move(body);
    work->headers = headers;
    work->class_index = response_class_index;
    return submit_task(IoKind::HttpRequest, work);
}

void AsyncIoService::execute_work(const std::shared_ptr<IoWorkItem>& work, AsyncIoCompletion& completion) {
    if (work->cancelled.load(std::memory_order_acquire)) {
        completion.cancelled = true;
        return;
    }

    try {
        switch (work->kind) {
            case IoKind::FsRead: {
                const std::filesystem::path path(work->path);
                validate_fs_read_path(path);
                completion.result = Runtime::make_string(read_file_text_sync(path));
                break;
            }
            case IoKind::FsWrite: {
                const std::filesystem::path path(work->path);
                validate_fs_write_path(path);
                write_file_text_sync(path, work->content, false);
                completion.result = Runtime::make_null();
                break;
            }
            case IoKind::FsAppend: {
                const std::filesystem::path path(work->path);
                validate_fs_write_path(path);
                write_file_text_sync(path, work->content, true);
                completion.result = Runtime::make_null();
                break;
            }
            case IoKind::HandleRead: {
                if (!work->handle) {
                    async_io_fail("invalid file handle operation");
                }
                NativeFileHandleData& data = require_open_handle(*work->handle);
                const OpenMode open_mode = parse_handle_mode(data.mode, data.path);
                if (!open_mode.can_read) {
                    async_io_fail("file is not open for reading");
                }
                std::ostringstream buffer;
                buffer << data.stream->rdbuf();
                completion.result = Runtime::make_string(buffer.str());
                break;
            }
            case IoKind::HandleWrite: {
                if (!work->handle) {
                    async_io_fail("invalid file handle operation");
                }
                NativeFileHandleData& data = require_open_handle(*work->handle);
                const OpenMode open_mode = parse_handle_mode(data.mode, data.path);
                if (!open_mode.can_write) {
                    async_io_fail("file is not open for writing");
                }
                if (!data.stream->write(work->content.data(), static_cast<std::streamsize>(work->content.size()))) {
                    async_io_fail("write failed for '" + data.path + "'");
                }
                completion.result = Runtime::make_null();
                break;
            }
            case IoKind::StdinLine:
                if (work->cancelled.load(std::memory_order_acquire)) {
                    completion.cancelled = true;
                    break;
                }
                if (work->prompt.has_value()) {
                    runtime_.output() << *work->prompt;
                    runtime_.output().flush();
                }
                if (runtime_.input_stream() == nullptr) {
                    async_io_fail("end of input");
                }
                {
                    std::string line;
                    if (!std::getline(*runtime_.input_stream(), line)) {
                        async_io_fail("end of input");
                    }
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    completion.result = Runtime::make_string(std::move(line));
                }
                break;
            case IoKind::NetConnect: {
                const SocketHandle socket =
                    tcp_connect_sync(work->path, work->port, &work->cancelled);
                completion.result =
                    net_make_connection(work->class_index, static_cast<std::uint32_t>(7), socket);
                break;
            }
            case IoKind::NetRead: {
                if (!work->network || work->network->closed) {
                    network_error("read on closed connection");
                }
                const std::vector<std::uint8_t> data =
                    tcp_read_sync(static_cast<SocketHandle>(work->network->socket), &work->cancelled);
                completion.result = Runtime::make_bytes(data);
                break;
            }
            case IoKind::NetWrite: {
                if (!work->network || work->network->closed) {
                    network_error("write on closed connection");
                }
                const std::size_t written = tcp_write_sync(static_cast<SocketHandle>(work->network->socket),
                                                             work->bytes.data(), work->bytes.size());
                completion.result = Runtime::make_integer(static_cast<std::int64_t>(written));
                break;
            }
            case IoKind::NetAccept: {
                if (!work->network || work->network->closed) {
                    network_error("accept on closed server");
                }
                const SocketHandle client =
                    tcp_accept_sync(static_cast<SocketHandle>(work->network->socket), &work->cancelled);
                completion.result =
                    net_make_connection(work->class_index, static_cast<std::uint32_t>(7), client);
                break;
            }
            case IoKind::NetUdpSend: {
                if (!work->network || work->network->closed) {
                    network_error("send on closed UDP socket");
                }
                udp_send_sync(static_cast<SocketHandle>(work->network->socket), work->bytes.data(), work->bytes.size(),
                              work->path, work->port);
                completion.result = Runtime::make_integer(static_cast<std::int64_t>(work->bytes.size()));
                break;
            }
            case IoKind::NetUdpReceive: {
                if (!work->network || work->network->closed) {
                    network_error("receive on closed UDP socket");
                }
                const NetUdpPacket packet =
                    udp_receive_sync(static_cast<SocketHandle>(work->network->socket), &work->cancelled);
                completion.result = net_make_packet(work->class_index, packet);
                break;
            }
            case IoKind::NetResolve: {
                std::vector<std::string> addresses = dns_resolve_sync(work->path);
                std::vector<Value> items;
                items.reserve(addresses.size());
                for (const std::string& address : addresses) {
                    items.push_back(Runtime::make_string(address));
                }
                completion.result = Runtime::make_list(std::move(items));
                break;
            }
            case IoKind::HttpRequest: {
                const HttpClientResponse response =
                    http_request_sync(work->content, work->path, work->bytes, work->headers, &work->cancelled);
                completion.result = http_make_response_value(work->class_index, response);
                break;
            }
            case IoKind::SqlOperation: {
                if (!work->sql_database || work->sql_database->closed) {
                    async_io_fail("SQL operation on a closed database");
                }
                if (work->sql_transaction) {
                    work->sql_transaction->require_active("async SQL operation");
                }
                if (work->cancelled.load(std::memory_order_acquire)) {
                    completion.cancelled = true;
                    break;
                }
                SqlDatabaseLock guard(work->sql_database);
                if (!guard.db()) {
                    async_io_fail("SQL operation on a closed database");
                }
                switch (work->sql_operation) {
                    case 1: {
                        const SqlExecuteOutcome outcome =
                            sql_execute_sql_unlocked(guard.db(), work->content, work->sql_params);
                        completion.result = make_sql_execute_result(outcome);
                        break;
                    }
                    case 2: {
                        const std::shared_ptr<SqlRowsState> rows =
                            sql_query_sql_unlocked(work->sql_database, guard.db(), work->content, work->sql_params);
                        completion.result =
                            sql_attach_rows(work->sql_rows_class_index, work->sql_database, rows);
                        break;
                    }
                    case 3: {
                        completion.result = sql_query_one_sql_unlocked(work->sql_database, guard.db(), work->content,
                                                                       work->sql_params);
                        break;
                    }
                    case 4: {
                        const std::shared_ptr<SqlStatementState> statement =
                            sql_prepare_statement(work->sql_database, work->content);
                        completion.result = sql_attach_statement(work->sql_statement_class_index, work->sql_database,
                                                               statement);
                        break;
                    }
                    default:
                        async_io_fail("invalid SQL async operation");
                }
                break;
            }
        }
    } catch (const RuntimeError& err) {
        completion.success = false;
        completion.error_category = err.category();
        completion.error_message = err.message();
    }
}

bool AsyncIoService::has_pending_completions() const {
    std::lock_guard lock(mutex_);
    return !completions_.empty();
}

bool AsyncIoService::has_in_flight_operations() const {
    std::lock_guard lock(mutex_);
    return in_flight_count_ > 0;
}

std::vector<AsyncIoCompletion> AsyncIoService::drain_completions() {
    std::lock_guard lock(mutex_);
    std::vector<AsyncIoCompletion> drained(completions_.begin(), completions_.end());
    completions_.clear();
    return drained;
}

void AsyncIoService::wait_until(std::optional<std::chrono::steady_clock::time_point> deadline) const {
    const auto should_wake = [this]() {
        if (!completions_.empty() || shutdown_requested_.load()) {
            return true;
        }
        if (hooks_.has_pending_callable_invokes && hooks_.has_pending_callable_invokes()) {
            return true;
        }
        return false;
    };
    std::unique_lock lock(mutex_);
    if (deadline.has_value()) {
        cv_.wait_until(lock, *deadline, should_wake);
    } else {
        cv_.wait(lock, should_wake);
    }
}

void AsyncIoService::wake_waiters() {
    cv_.notify_all();
}

void AsyncIoService::ensure_io_started(std::uint64_t operation_id) {
    if (operation_id == 0) {
        return;
    }
    std::shared_ptr<IoWorkItem> work;
    {
        std::lock_guard lock(mutex_);
        const auto it = deferred_operations_.find(operation_id);
        if (it == deferred_operations_.end()) {
            return;
        }
        work = it->second;
        deferred_operations_.erase(it);
    }
    if (work->cancelled.load(std::memory_order_acquire)) {
        AsyncIoCompletion completion;
        completion.task = work->task.lock();
        completion.cancelled = true;
        completion.success = false;
        push_completion(std::move(completion));
        return;
    }
    ensure_worker_started();
    enqueue_work(std::move(work));
}

void AsyncIoService::cancel_operation(std::uint64_t operation_id) {
    if (operation_id == 0) {
        return;
    }
    std::lock_guard lock(mutex_);
    const auto deferred = deferred_operations_.find(operation_id);
    if (deferred != deferred_operations_.end()) {
        deferred->second->cancelled.store(true, std::memory_order_release);
        cv_.notify_all();
        return;
    }
    for (const std::shared_ptr<IoWorkItem>& work : work_queue_) {
        if (work && work->id == operation_id) {
            work->cancelled.store(true, std::memory_order_release);
            if (work->sql_database) {
                sql_interrupt_database(work->sql_database);
            }
            cv_.notify_all();
            return;
        }
    }
    for (const std::shared_ptr<IoWorkItem>& work : active_operations_) {
        if (work && work->id == operation_id) {
            work->cancelled.store(true, std::memory_order_release);
            if (work->sql_database) {
                sql_interrupt_database(work->sql_database);
            }
            cv_.notify_all();
            return;
        }
    }
}

void AsyncIoService::cancel_task_operations(const std::shared_ptr<TaskData>& task) {
    if (!task || task->io_operation_id == 0) {
        return;
    }
    cancel_operation(task->io_operation_id);
}

void AsyncIoService::shutdown() {
    shutdown_requested_ = true;
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_ = false;
    std::lock_guard lock(mutex_);
    work_queue_.clear();
    active_operations_.clear();
    deferred_operations_.clear();
    completions_.clear();
    in_flight_count_ = 0;
}

bool AsyncIoService::run_socket_readiness_self_test() {
    TestSocketHandle sockets[2] = {kTestInvalidSocket, kTestInvalidSocket};
    if (!create_loopback_socket_pair(sockets)) {
        return false;
    }

    const char payload[] = "pscript-async-io";
    bool ok = false;
#ifdef _WIN32
    ok = send(sockets[0], payload, static_cast<int>(sizeof(payload) - 1), 0) == static_cast<int>(sizeof(payload) - 1);
#else
    ok = write(sockets[0], payload, sizeof(payload) - 1) == static_cast<ssize_t>(sizeof(payload) - 1);
#endif
    if (!ok) {
        close_socket(sockets[0]);
        close_socket(sockets[1]);
        return false;
    }

    const bool readable = poll_socket_readable(sockets[1], 1000);
    close_socket(sockets[0]);
    close_socket(sockets[1]);
    return readable;
}

}  // namespace pscript
