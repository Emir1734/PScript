#pragma once

#include "runtime/crypto_ops.h"
#include "runtime/sql_connection.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <iosfwd>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <functional>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime/builtin_id.h"
#include "runtime/profiler.h"
#include "runtime/task_group.h"

namespace pscript {

struct NativeAsyncStreamData;

struct Bytecode;

enum class ValueKind {
    Integer,
    Float,
    String,
    Bool,
    Null,
    Bytes,
    List,
    Dict,
    Tuple,
    Set,
    Struct,
    Class,
    Enum,
    Range,
    Iterator,
    Function,
    Module,
    Task,
};

struct Value;
struct NativeHttpServerData;

struct ListData {
    std::vector<Value> items;
};

struct DictKeyHash {
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(const std::string& key) const noexcept {
        return std::hash<std::string>{}(key);
    }
    [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

struct DictData {
    std::vector<std::pair<std::string, Value>> entries;
    std::unordered_map<std::string, std::size_t, DictKeyHash, std::equal_to<>> key_index;
};

struct TupleData {
    std::vector<Value> items;
};

struct SetData {
    std::vector<Value> items;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> buckets;
};

struct NativeCollectionData {
    std::uint32_t kind = 0;
    std::vector<Value> items;
};

struct NativeFileHandleData {
    std::uint32_t kind = 0;
    std::string path;
    std::string mode;
    bool closed = false;
    std::shared_ptr<std::fstream> stream;
    ~NativeFileHandleData();
};

struct NativeIteratorData;

struct NativeDateTimeData {
    std::uint32_t kind = 0;
    std::int64_t utc_micros = 0;
    bool use_utc_fields = false;
};

struct NativeDurationData {
    std::uint32_t kind = 0;
    std::int64_t micros = 0;
};

struct NativeNetworkData {
    std::uint32_t kind = 0;
    std::uintptr_t socket = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
    bool closed = false;
    ~NativeNetworkData();
};

void close_native_file_handle(NativeFileHandleData& data);
void close_native_network(NativeNetworkData& data);

struct NativeCryptoHasherData {
    std::uint32_t kind = 0;
    CryptoHashAlgorithm algorithm = CryptoHashAlgorithm::Sha256;
    CryptoHasher hasher;
};

struct SqlRowsState;
struct SqlStatementState;
struct SqlTransactionState;
struct NativeDatabaseData;
struct NativeArchiveData;

struct NativeSqlRowsData {
    std::uint32_t kind = 0;
    std::shared_ptr<SqlRowsState> state;
};

struct NativeSqlStatementData {
    std::uint32_t kind = 0;
    std::shared_ptr<SqlStatementState> state;
    std::weak_ptr<NativeDatabaseData> owner_database;
};

struct NativeSqlTransactionData {
    std::uint32_t kind = 0;
    std::shared_ptr<SqlTransactionState> state;
    std::weak_ptr<NativeDatabaseData> owner_database;
};

struct NativeDatabaseData {
    std::uint32_t kind = 0;
    std::string path;
    bool closed = true;
    std::shared_ptr<SqlConnectionHandle> connection;
    std::recursive_mutex exec_mutex;
    std::mutex resource_mutex;
    std::vector<std::weak_ptr<SqlRowsState>> row_results;
    std::vector<std::weak_ptr<SqlStatementState>> prepared_statements;
    std::vector<std::weak_ptr<SqlTransactionState>> transactions;

    void track_row_result(const std::shared_ptr<SqlRowsState>& state);
    void track_prepared_statement(const std::shared_ptr<SqlStatementState>& state);
    void track_transaction(const std::shared_ptr<SqlTransactionState>& state);
    void rollback_active_transactions();
    void finalize_sql_resources();
};

struct BytesData {
    std::vector<std::uint8_t> data;
};

struct StructData {
    std::uint32_t type_index = 0;
    std::string type_name;
    std::vector<std::string> field_names;
    std::vector<Value> fields;
};

struct ClassData {
    std::uint32_t type_index = 0;
    std::uint32_t module_index = 0xFFFFFFFFu;
    std::string type_name;
    std::vector<std::string> field_names;
    std::unordered_map<std::string, std::uint32_t> field_name_index;
    std::vector<Value> fields;
    std::shared_ptr<NativeCollectionData> native_collection;
    std::shared_ptr<NativeFileHandleData> native_file;
    std::shared_ptr<NativeIteratorData> native_iterator;
    std::shared_ptr<NativeDateTimeData> native_datetime;
    std::shared_ptr<NativeDurationData> native_duration;
    std::shared_ptr<NativeNetworkData> native_network;
    std::shared_ptr<NativeHttpServerData> native_http;
    std::shared_ptr<NativeCryptoHasherData> native_crypto;
    std::shared_ptr<NativeDatabaseData> native_database;
    std::shared_ptr<NativeSqlRowsData> native_sql_rows;
    std::shared_ptr<NativeSqlStatementData> native_sql_statement;
    std::shared_ptr<TaskGroupData> native_task_group;
    std::shared_ptr<NativeAsyncStreamData> native_async_stream;
    std::shared_ptr<NativeSqlTransactionData> native_sql_transaction;
    std::shared_ptr<NativeArchiveData> native_archive;
};

struct RangeData {
    std::int64_t start = 0;
    std::int64_t stop = 0;
    std::int64_t step = 1;
};

struct TaskData;

struct ClosureData;
struct ModuleNamespaceData {
    std::uint32_t module_index = 0;
};

struct EnumData;

struct Value {
    ValueKind kind = ValueKind::Null;
    std::int64_t integer = 0;
    double floating = 0.0;
    std::string string;
    bool boolean = false;
    std::shared_ptr<BytesData> bytes;
    std::shared_ptr<ListData> list;
    std::shared_ptr<DictData> dict;
    std::shared_ptr<TupleData> tuple;
    std::shared_ptr<SetData> set;
    std::shared_ptr<StructData> struct_value;
    std::shared_ptr<ClassData> class_value;
    std::shared_ptr<EnumData> enum_value;
    std::shared_ptr<RangeData> range;
    std::shared_ptr<NativeIteratorData> iterator_data;
    std::shared_ptr<ClosureData> function;
    std::shared_ptr<ModuleNamespaceData> module_namespace;
    std::shared_ptr<TaskData> task;
};

struct EnumData {
    std::uint32_t enum_index = 0;
    std::uint32_t member_index = 0;
    std::string enum_name;
    std::string member_name;
    Value payload{};
};

struct NativeHttpServerData {
    std::shared_ptr<NativeNetworkData> listener;
    bool closed = false;
    bool running = false;
    struct Route {
        std::string method;
        std::string path;
        Value handler;
    };
    std::vector<Route> routes;
    std::thread worker;
    std::atomic<bool> stop_requested{false};
    struct PendingHandler {
        std::uintptr_t client = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
        std::shared_ptr<std::promise<Value>> promise;
        std::future<Value> future;
    };
    std::mutex pending_mutex;
    std::vector<PendingHandler> pending_handlers;
};

struct Cell {
    std::optional<Value> value;
};

enum class TaskState {
    Pending,
    Running,
    Completed,
    Failed,
    Cancelled,
};

struct AsyncExecutionState;

struct AsyncSchedulerHooks {
    std::function<void(const std::shared_ptr<TaskData>&)> track_task;
    std::function<bool()> scheduler_active;
    std::function<Value(const Value& callable, const std::vector<Value>& args)> invoke_callable;
    std::function<bool()> has_pending_callable_invokes;
    std::function<void(const std::shared_ptr<TaskGroupData>& group)> push_task_group;
    std::function<void(const std::shared_ptr<TaskGroupData>& group)> pop_task_group;
    std::function<void(const std::shared_ptr<TaskData>& child)> track_task_group_child;
    std::function<std::shared_ptr<TaskData>(const std::vector<Value>& tasks)> create_wait_all_task;
};

class AsyncIoService;
class CliState;

struct TestingRunnerHooks {
    std::function<int(std::optional<std::string> filter)> run_tests;
};

struct TaskData {
    TaskState state = TaskState::Pending;
    Value result;
    Value exception;
    std::shared_ptr<AsyncExecutionState> saved_state;
    std::vector<std::weak_ptr<TaskData>> waiters;
    std::weak_ptr<TaskData> awaiting_task;

    std::uint32_t function_index = 0xFFFFFFFFu;
    std::uint32_t module_index = 0xFFFFFFFFu;
    std::vector<Value> args;
    std::vector<std::shared_ptr<Cell>> captures;
    std::optional<Value> implicit_this;
    std::uint32_t return_dest = 0;
    bool is_root = false;
    bool exception_consumed = false;
    bool cancel_requested = false;
    bool is_race_meta = false;
    bool is_timeout_meta = false;
    bool is_wait_all_meta = false;
    bool timeout_expired = false;

    std::vector<std::shared_ptr<TaskData>> race_contestants;
    std::vector<std::shared_ptr<TaskData>> wait_all_contestants;
    std::vector<Value> wait_all_results;
    std::size_t wait_all_completed_count = 0;
    std::weak_ptr<TaskGroupData> task_group_member;
    std::shared_ptr<TaskData> timeout_target;
    std::shared_ptr<TaskData> timeout_timer_task;
    std::weak_ptr<TaskData> timeout_meta_owner;

    std::optional<std::chrono::steady_clock::time_point> wake_time;
    std::uint64_t io_operation_id = 0;
};

struct AsyncExecutionState {
    std::size_t ip = 0;
    std::size_t register_base = 0;
    std::vector<std::optional<Value>> registers;
    std::vector<std::optional<Value>> variables;
    std::vector<std::shared_ptr<Cell>> locals;
    std::vector<std::shared_ptr<Cell>> upvalues;
    struct SavedCallFrame {
        std::size_t return_ip = 0;
        std::uint32_t dest = 0;
        std::uint32_t function_index = 0;
        std::uint32_t module_index = 0xFFFFFFFFu;
        const Bytecode* saved_code = nullptr;
        std::vector<std::optional<Value>> saved_variables;
        std::size_t register_base = 0;
        std::vector<std::optional<Value>> registers;
        std::vector<std::shared_ptr<Cell>> locals;
        std::vector<std::shared_ptr<Cell>> upvalues;
        std::vector<Value> defer_stack;
    };
    std::vector<SavedCallFrame> frames;
    struct SavedHandlerFrame {
        std::uint32_t handler_index = 0;
        std::size_t frame_depth = 0;
        std::uint32_t next_catch = 0;
        const Bytecode* owner_code = nullptr;
    };
    std::vector<SavedHandlerFrame> handler_stack;
    std::optional<Value> pending_exception;
    std::uint32_t pending_control_kind = 0;
    Value pending_control_return;
    std::uint32_t pending_control_jump = 0;
    const Bytecode* execution_bytecode = nullptr;
    std::uint32_t await_dest = 0;
    std::size_t suspend_ip = 0;
};

struct ClosureData {
    std::uint32_t function_index = 0;
    std::string name;
    std::vector<std::shared_ptr<Cell>> captures;
    std::uint32_t module_index = 0xFFFFFFFFu;
    bool module_class_ctor = false;
    bool module_struct_ctor = false;
    std::uint32_t class_index = 0xFFFFFFFFu;
    std::uint32_t struct_index = 0xFFFFFFFFu;
};

enum class RuntimeErrorCategory {
    Runtime,
    Type,
    Name,
    Index,
    Value,
    Network,
};

class RuntimeError : public std::runtime_error {
public:
    explicit RuntimeError(std::string message);
    RuntimeError(std::string message, RuntimeErrorCategory category);

    const std::string& message() const { return message_; }
    RuntimeErrorCategory category() const { return category_; }
    static RuntimeErrorCategory infer_category(const std::string& message);

private:
    std::string message_;
    RuntimeErrorCategory category_ = RuntimeErrorCategory::Runtime;
};

class ProcessExit : public std::runtime_error {
public:
    explicit ProcessExit(int code);

    int code() const { return code_; }

private:
    int code_ = 0;
};

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

struct NativeCallSite {
    std::string file;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

class Runtime;

using Builtin = std::function<Value(Runtime&, const std::vector<Value>&)>;

class Runtime {
    friend bool dispatch_runtime_builtin(Runtime& runtime, BuiltinId id, const Value* args, std::uint32_t argc,
                                         Value& out);

public:
    explicit Runtime(std::ostream& output);
    Runtime(std::ostream& output, std::istream& input);
    Runtime(std::ostream& output, std::istream& input, std::ostream& error_output);
    ~Runtime();

    static Value make_integer(std::int64_t value);
    static Value make_float(double value);
    static Value make_string(std::string value);
    static Value concat_strings(const std::string& left, const std::string& right);
    static Value concat_strings(std::string&& left, const std::string& right);
    static Value make_bool(bool value);
    static Value make_null();
    static Value make_bytes(std::vector<std::uint8_t> data);
    static Value make_list(std::vector<Value> items);
    static Value make_dict(std::vector<std::pair<std::string, Value>> entries);
    static Value make_tuple(std::vector<Value> items);
    static Value make_set(std::vector<Value> items);
    static Value make_struct(std::uint32_t type_index, std::string type_name, std::vector<std::string> field_names);
    static Value make_class(std::uint32_t type_index, std::string type_name, std::vector<std::string> field_names);
    static Value make_enum(std::uint32_t enum_index, std::uint32_t member_index, std::string enum_name,
                           std::string member_name);
    static Value make_enum(std::uint32_t enum_index, std::uint32_t member_index, std::string enum_name,
                           std::string member_name, Value payload);
    static Value get_field(const Value& object, std::uint32_t field_index);
    static void set_field(const Value& object, std::uint32_t field_index, Value value);
    static Value make_range(std::int64_t start, std::int64_t stop, std::int64_t step);
    static Value make_function(std::shared_ptr<ClosureData> closure);
    // Deep-copy closure capture cells so nested JIT scratch locals can be recycled safely.
    static Value clone_closure_value(const Value& value);
    static Value make_task(std::shared_ptr<TaskData> task);
    static Value make_module(std::uint32_t module_index);
    static std::string format(const Value& value);
    static bool is_truthy(const Value& value);
    static Value get_index(const Value& object, const Value& index);
    static void set_index(const Value& object, const Value& index, Value value);
    static void check_destructure(const Value& value, std::uint32_t expected_count, std::uint32_t kind);
    static bool test_destructure_shape(const Value& value, std::uint32_t expected_count, std::uint32_t kind);
    static Value destructure_rest(const Value& value, std::uint32_t start_index);
    static Value destructure_dict_rest(const Value& value, const std::vector<std::string>& excluded_keys);
    static bool equals(const Value& left, const Value& right);
    static bool strings_equal(const std::string& left, const std::string& right);
    static Value contains(const Value& value, const Value& container);
    static Value call_method(const Value& object, std::string_view name, const std::vector<Value>& args);
    static Value iter_len(const Value& collection);
    static Value iter_get(const Value& collection, const Value& index);

    static Value list_append(const Value& list, Value value);
    static Value list_prepend(const Value& list, Value value);
    static Value list_first(const Value& list);
    static Value list_last(const Value& list);
    static Value list_find(const Value& list, const Value& value);
    static Value list_contains(const Value& list, const Value& value);
    static Value list_count(const Value& list, const Value& value);
    static Value list_remove(const Value& list, const Value& value);
    static Value list_remove_at(const Value& list, const Value& index);
    static Value list_clear(const Value& list);
    static Value list_is_empty(const Value& list);
    static Value list_length(const Value& list);

    static Value tuple_first(const Value& tuple);
    static Value tuple_last(const Value& tuple);
    static Value tuple_find(const Value& tuple, const Value& value);
    static Value tuple_contains(const Value& tuple, const Value& value);
    static Value tuple_length(const Value& tuple);

    static Value set_add(const Value& set, Value value);
    static Value set_remove(const Value& set, const Value& value);
    static Value set_contains(const Value& set, const Value& value);
    static Value set_length(const Value& set);
    static Value set_is_empty(const Value& set);
    static Value set_clear(const Value& set);
    static Value set_union(const Value& set, const Value& other);
    static Value set_intersection(const Value& set, const Value& other);
    static Value set_difference(const Value& set, const Value& other);
    static Value list_sort(const Value& list);
    static Value list_reverse(const Value& list);

    static const Value* dict_lookup(const DictData& dict, std::string_view key);
    static Value* dict_lookup_mut(DictData& dict, std::string_view key);
    static Value dict_get(const Value& dict, const Value& key);
    static Value dict_contains_key(const Value& dict, const Value& key);
    static Value dict_remove(const Value& dict, const Value& key);
    static Value dict_keys(const Value& dict);
    static Value dict_values(const Value& dict);
    static Value dict_clear(const Value& dict);
    static Value dict_is_empty(const Value& dict);
    static Value dict_length(const Value& dict);

    static Value string_find(const Value& text, const Value& needle);
    static Value string_contains(const Value& text, const Value& needle);
    static Value string_replace(const Value& text, const Value& from, const Value& to);
    static Value string_starts_with(const Value& text, const Value& prefix);
    static Value string_ends_with(const Value& text, const Value& suffix);
    static Value string_first(const Value& text);
    static Value string_last(const Value& text);
    static Value string_is_empty(const Value& text);
    static Value string_length(const Value& text);
    static Value string_trim(const Value& text);
    static Value string_lower(const Value& text);
    static Value string_upper(const Value& text);
    static Value string_split(const Value& text, const Value& separator);

    void register_builtin(std::string name, Builtin builtin);
    bool has_builtin(std::string_view name) const;
    Value call(std::string_view name, const std::vector<Value>& args);
    Value print(const std::vector<Value>& args);
    std::ostream& output();
    std::ostream& error_output();

    LogLevel log_level() const { return log_level_; }
    void set_log_level(LogLevel level) { log_level_ = level; }
    static bool parse_log_level(std::string_view text, LogLevel& out);

    void set_native_call_site(std::string_view file, std::uint32_t line, std::uint32_t column);
    void clear_native_call_site();
    const std::optional<NativeCallSite>& native_call_site() const { return native_call_site_; }

    void set_program_args(std::vector<std::string> args);
    const std::vector<std::string>& program_args() const { return program_args_; }
    static const char* version_string();
    [[noreturn]] void request_process_exit(int code);
    void signal_process_exit(int code);
    bool process_exit_requested() const { return process_exit_requested_; }
    int process_exit_code() const { return process_exit_code_; }

    void set_async_scheduler_hooks(AsyncSchedulerHooks hooks);
    void clear_async_scheduler_hooks();
    AsyncIoService& async_io();
    const AsyncIoService& async_io() const;
    bool async_scheduler_active() const;
    const AsyncSchedulerHooks& async_hooks() const { return async_hooks_; }

    using CallableInvoker = std::function<Value(const Value& callable, const std::vector<Value>& args)>;
    void set_callable_invoker(CallableInvoker invoker);
    void clear_callable_invoker();
    Value invoke_callable(const Value& callable, const std::vector<Value>& args);
    using ParallelThreadInvokerFactory = std::function<CallableInvoker()>;
    void set_parallel_thread_invoker_factory(ParallelThreadInvokerFactory factory);
    void clear_parallel_thread_invoker_factory();
    bool has_parallel_thread_invoker_factory() const;
    CallableInvoker create_parallel_thread_invoker();
    // Serializes worker VM invocations that share this Runtime (heap, builtins, profiler).
    std::mutex& parallel_worker_invoke_mutex() { return parallel_worker_invoke_mutex_; }
    void queue_callable_invoke(const Value& callable, std::vector<Value> args,
                               const std::shared_ptr<std::promise<Value>>& promise);
    bool has_pending_callable_invokes() const;
    void drain_callable_invokes();
    void register_sql_database(const std::shared_ptr<NativeDatabaseData>& database);
    void close_all_sql_databases();

    CliState& cli_state();
    const CliState& cli_state() const;

    void set_testing_runner_hooks(TestingRunnerHooks hooks);
    void clear_testing_runner_hooks();
    int run_tests(std::optional<std::string> filter);
    std::istream* input_stream() { return input_; }
    const std::istream* input_stream() const { return input_; }

    RuntimeProfiler& profiler() { return profiler_; }
    const RuntimeProfiler& profiler() const { return profiler_; }

private:
    static Value builtin_print(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_range(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_input(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_int(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_float(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_str(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_bool(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_type(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_abs(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_min(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_max(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_round(Runtime& runtime, const std::vector<Value>& args);
    static Value builtin_clamp(Runtime& runtime, const std::vector<Value>& args);
    static Value convert_int(const Value& value);
    static Value convert_float(const Value& value);
    static Value convert_bool(const Value& value);
    static std::string trim_copy(std::string text);
    static std::string ascii_lower(std::string text);
    static std::string ascii_upper(std::string text);
    static std::int64_t range_length(const RangeData& range);
    static std::string format_impl(const Value& value, std::vector<const void*>& seen);
    static const char* kind_name(ValueKind kind);
    static std::string runtime_type_name(const Value& value);
    static Value call_list_method(const Value& object, std::string_view name, const std::vector<Value>& args);
    static Value call_dict_method(const Value& object, std::string_view name, const std::vector<Value>& args);
    static Value call_tuple_method(const Value& object, std::string_view name, const std::vector<Value>& args);
    static Value call_set_method(const Value& object, std::string_view name, const std::vector<Value>& args);
    static Value call_string_method(const Value& object, std::string_view name, const std::vector<Value>& args);
    static void expect_argc(std::string_view method, const std::vector<Value>& args, std::size_t count);
    static const Value& expect_string_arg(std::string_view method, const Value& value, std::string_view what);
    static std::int64_t expect_int_arg(std::string_view method, const Value& value, std::string_view what);
    [[noreturn]] static void fail(const std::string& message);

    std::ostream* output_ = nullptr;
    std::ostream* error_output_ = nullptr;
    std::istream* input_ = nullptr;
    LogLevel log_level_ = LogLevel::Info;
    std::optional<NativeCallSite> native_call_site_;
    std::vector<std::string> program_args_;
    bool process_exit_requested_ = false;
    int process_exit_code_ = 0;
    std::unordered_map<std::string, Builtin> builtins_;
    AsyncSchedulerHooks async_hooks_;
    CallableInvoker callable_invoker_;
    ParallelThreadInvokerFactory parallel_thread_invoker_factory_;
    struct PendingCallableInvoke {
        Value callable;
        std::vector<Value> args;
        std::shared_ptr<std::promise<Value>> promise;
    };
    mutable std::mutex callable_mutex_;
    std::mutex parallel_worker_invoke_mutex_;
    std::vector<PendingCallableInvoke> pending_callables_;
    std::mutex sql_databases_mutex_;
    std::vector<std::shared_ptr<NativeDatabaseData>> sql_databases_;
    std::unique_ptr<AsyncIoService> async_io_;
    std::unique_ptr<CliState> cli_state_;
    TestingRunnerHooks testing_hooks_;
    RuntimeProfiler profiler_;
};

}  // namespace pscript
