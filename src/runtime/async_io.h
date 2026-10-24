#pragma once

#include "runtime/net_ops.h"
#include "runtime/runtime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pscript {

struct SqlTransactionState;
struct NativeDatabaseData;

struct AsyncIoCompletion {
    std::shared_ptr<TaskData> task;
    Value result = Runtime::make_null();
    RuntimeErrorCategory error_category = RuntimeErrorCategory::Runtime;
    std::string error_message;
    bool success = true;
    bool cancelled = false;
};

class AsyncIoService {
public:
    explicit AsyncIoService(Runtime& runtime);
    ~AsyncIoService();

    AsyncIoService(const AsyncIoService&) = delete;
    AsyncIoService& operator=(const AsyncIoService&) = delete;

    void set_hooks(AsyncSchedulerHooks hooks);
    void clear_hooks();

    Value submit_fs_read(const std::string& path);
    Value submit_fs_write(const std::string& path, std::string content, bool append);
    Value submit_handle_read(const std::shared_ptr<NativeFileHandleData>& handle);
    Value submit_handle_write(const std::shared_ptr<NativeFileHandleData>& handle, std::string content);
    Value submit_stdin_line(std::optional<std::string> prompt);

    Value submit_net_connect(const std::string& host, int port, std::uint32_t connection_class_index);
    Value submit_net_read(const std::shared_ptr<NativeNetworkData>& connection);
    Value submit_net_write(const std::shared_ptr<NativeNetworkData>& connection, std::vector<std::uint8_t> data);
    Value submit_net_accept(const std::shared_ptr<NativeNetworkData>& server, std::uint32_t connection_class_index);
    Value submit_net_udp_send(const std::shared_ptr<NativeNetworkData>& socket, std::vector<std::uint8_t> data,
                              const std::string& host, int port);
    Value submit_net_udp_receive(const std::shared_ptr<NativeNetworkData>& socket, std::uint32_t packet_class_index);
    Value submit_net_resolve(const std::string& host);

    Value submit_http_request(const std::string& method, const std::string& url, std::vector<std::uint8_t> body,
                              const std::map<std::string, std::string>& headers, std::uint32_t response_class_index);

    Value submit_sql_operation(std::uint32_t operation, const std::shared_ptr<NativeDatabaseData>& database,
                               const std::string& sql, const std::vector<Value>& params,
                               const std::shared_ptr<SqlTransactionState>& transaction,
                               std::uint32_t statement_class_index, std::uint32_t rows_class_index);

    bool has_pending_completions() const;
    std::vector<AsyncIoCompletion> drain_completions();
    void wait_until(std::optional<std::chrono::steady_clock::time_point> deadline) const;
    void wake_waiters();
    bool has_in_flight_operations() const;

    void cancel_operation(std::uint64_t operation_id);
    void cancel_task_operations(const std::shared_ptr<TaskData>& task);
    /** Start stdin-line I/O when its task is first scheduled (not at task creation). */
    void ensure_io_started(std::uint64_t operation_id);
    void shutdown();

    static bool run_socket_readiness_self_test();

private:
    enum class IoKind {
        FsRead,
        FsWrite,
        FsAppend,
        HandleRead,
        HandleWrite,
        StdinLine,
        NetConnect,
        NetRead,
        NetWrite,
        NetAccept,
        NetUdpSend,
        NetUdpReceive,
        NetResolve,
        HttpRequest,
        SqlOperation,
    };

    struct IoWorkItem {
        std::uint64_t id = 0;
        std::weak_ptr<TaskData> task;
        IoKind kind = IoKind::FsRead;
        std::string path;
        std::string content;
        std::shared_ptr<NativeFileHandleData> handle;
        std::optional<std::string> prompt;
        std::shared_ptr<NativeNetworkData> network;
        std::vector<std::uint8_t> bytes;
        int port = 0;
        std::uint32_t class_index = 0;
        std::map<std::string, std::string> headers;
        std::shared_ptr<NativeDatabaseData> sql_database;
        std::shared_ptr<SqlTransactionState> sql_transaction;
        std::vector<Value> sql_params;
        std::uint32_t sql_operation = 0;
        std::uint32_t sql_statement_class_index = 0;
        std::uint32_t sql_rows_class_index = 0;
        std::atomic<bool> cancelled{false};
    };

    Value submit_task(IoKind kind, const std::shared_ptr<IoWorkItem>& work, bool defer_until_scheduled = true);
    void ensure_worker_started();
    void worker_main();
    void enqueue_work(std::shared_ptr<IoWorkItem> work);
    void dispatch_work(std::shared_ptr<IoWorkItem> work);
    void run_work_item(const std::shared_ptr<IoWorkItem>& work);
    void push_completion(AsyncIoCompletion completion);
    void execute_work(const std::shared_ptr<IoWorkItem>& work, AsyncIoCompletion& completion);
    bool uses_dedicated_thread(IoKind kind) const;

    Runtime& runtime_;
    AsyncSchedulerHooks hooks_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::deque<std::shared_ptr<IoWorkItem>> work_queue_;
    std::deque<AsyncIoCompletion> completions_;
    std::vector<std::shared_ptr<IoWorkItem>> active_operations_;
    std::atomic<std::uint64_t> next_operation_id_{1};
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_requested_{false};
    std::thread worker_;
    std::size_t in_flight_count_ = 0;
    std::unordered_map<std::uint64_t, std::shared_ptr<IoWorkItem>> deferred_operations_;
};

}  // namespace pscript
