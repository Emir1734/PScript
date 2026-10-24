#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"
#include "stdlib/stdlib.h"
#include "runtime/iterator.h"
#include "runtime/sql_async.h"
#include "runtime/sql_connection.h"
#include "runtime/sql_ops.h"
#include "runtime/sql_transaction.h"

namespace pscript {
namespace {

constexpr std::uint32_t kDatabaseClassIndex = 0;
constexpr std::uint32_t kRowsClassIndex = 1;
constexpr std::uint32_t kStatementClassIndex = 2;
constexpr std::uint32_t kTransactionClassIndex = 5;

NativeDatabaseData& require_database(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_database) {
        native_type_error("invalid database operation");
    }
    return *object.class_value->native_database;
}

NativeDatabaseData& require_open_database(const Value& object, std::string_view operation) {
    NativeDatabaseData& data = require_database(object);
    if (data.closed || !data.connection || !data.connection->is_open()) {
        native_value_error(std::string(operation) + " on a closed database");
    }
    return data;
}

std::shared_ptr<NativeDatabaseData> database_ptr(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value) {
        return nullptr;
    }
    return object.class_value->native_database;
}

std::vector<Value> sql_params_from_arg(const Value& value, std::string_view function) {
    if (value.kind == ValueKind::Null) {
        return {};
    }
    if (value.kind != ValueKind::List || !value.list) {
        native_type_error(std::string(function) + " expects parameter list or null");
    }
    return value.list->items;
}

std::vector<Value> optional_sql_params(const std::vector<Value>& args, std::size_t index, std::string_view function) {
    if (args.size() <= index) {
        return {};
    }
    return sql_params_from_arg(args[index], function);
}

Value make_database_value(Runtime& runtime, std::shared_ptr<SqlConnectionHandle> connection, std::string path) {
    auto data = std::make_shared<NativeDatabaseData>();
    data->kind = static_cast<std::uint32_t>(NativeClassKind::Database);
    data->path = std::move(path);
    data->connection = std::move(connection);
    data->closed = false;
    runtime.register_sql_database(data);
    Value value = Runtime::make_class(kDatabaseClassIndex, "Database", std::vector<std::string>{});
    value.class_value->native_database = std::move(data);
    return value;
}

Value make_rows_value(const std::shared_ptr<NativeDatabaseData>& database, std::shared_ptr<SqlRowsState> state) {
    return sql_attach_rows(kRowsClassIndex, database, std::move(state));
}

Value make_statement_value(const std::shared_ptr<NativeDatabaseData>& database,
                          std::shared_ptr<SqlStatementState> state) {
    return sql_attach_statement(kStatementClassIndex, database, std::move(state));
}

Value make_transaction_value(const std::shared_ptr<NativeDatabaseData>& database,
                             std::shared_ptr<SqlTransactionState> state) {
    auto transaction_data = std::make_shared<NativeSqlTransactionData>();
    transaction_data->kind = static_cast<std::uint32_t>(NativeClassKind::SqlTransaction);
    transaction_data->state = std::move(state);
    transaction_data->owner_database = database;
    Value value = Runtime::make_class(kTransactionClassIndex, "Transaction", std::vector<std::string>{});
    value.class_value->native_sql_transaction = std::move(transaction_data);
    return value;
}

NativeSqlStatementData& require_statement(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_sql_statement) {
        native_type_error("invalid statement operation");
    }
    return *object.class_value->native_sql_statement;
}

NativeSqlStatementData& require_open_statement(const Value& object, std::string_view operation) {
    NativeSqlStatementData& data = require_statement(object);
    if (!data.state || data.state->is_closed()) {
        native_value_error(std::string(operation) + " on a closed statement");
    }
    return data;
}

NativeSqlRowsData& require_rows(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_sql_rows) {
        native_type_error("invalid rows operation");
    }
    return *object.class_value->native_sql_rows;
}

NativeSqlTransactionData& require_transaction(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_sql_transaction) {
        native_type_error("invalid transaction operation");
    }
    return *object.class_value->native_sql_transaction;
}

std::shared_ptr<SqlTransactionState> require_active_transaction(const Value& object, std::string_view operation) {
    NativeSqlTransactionData& data = require_transaction(object);
    if (!data.state) {
        native_value_error(std::string(operation) + " on an invalid transaction");
    }
    data.state->invalidate_if_database_closed();
    data.state->require_active(operation);
    return data.state;
}

std::shared_ptr<NativeDatabaseData> transaction_database(const Value& object, std::string_view operation) {
    NativeSqlTransactionData& data = require_transaction(object);
    const std::shared_ptr<NativeDatabaseData> owner = data.owner_database.lock();
    if (!owner || owner->closed) {
        native_value_error(std::string(operation) + " on a closed database");
    }
    return owner;
}

Value run_execute(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                  const std::vector<Value>& params) {
    return make_sql_execute_result(sql_execute_sql(database, sql, params));
}

Value run_query(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                const std::vector<Value>& params) {
    return make_rows_value(database, sql_query_sql(database, sql, params));
}

Value run_query_one(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                    const std::vector<Value>& params) {
    return sql_query_one_sql(database, sql, params);
}

Value run_prepare(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql) {
    return make_statement_value(database, sql_prepare_statement(database, sql));
}

Value run_async(Runtime& runtime, SqlAsyncOperation operation, const std::shared_ptr<NativeDatabaseData>& database,
                const std::string& sql, const std::vector<Value>& params,
                const std::shared_ptr<SqlTransactionState>& transaction) {
    return submit_sql_async(runtime, operation, database, sql, params, transaction, kStatementClassIndex,
                            kRowsClassIndex);
}

void dispatch_sql_query_methods(Runtime& runtime, const std::shared_ptr<NativeDatabaseData>& database,
                                const std::shared_ptr<SqlTransactionState>& transaction, std::string_view name,
                                const std::vector<Value>& args, Value& out, bool on_transaction) {
    if (name == "execute") {
        native_require_min_argc("execute", args, 1);
        if (args.size() > 2) {
            native_value_error("execute expects at most 2 arguments");
        }
        const std::string& sql = native_require_string_arg(args, 0, "execute", "sql");
        const std::vector<Value> params = optional_sql_params(args, 1, "execute");
        out = run_execute(database, sql, params);
        return;
    }
    if (name == "query") {
        native_require_min_argc("query", args, 1);
        if (args.size() > 2) {
            native_value_error("query expects at most 2 arguments");
        }
        const std::string& sql = native_require_string_arg(args, 0, "query", "sql");
        const std::vector<Value> params = optional_sql_params(args, 1, "query");
        out = run_query(database, sql, params);
        return;
    }
    if (name == "query_one") {
        native_require_min_argc("query_one", args, 1);
        if (args.size() > 2) {
            native_value_error("query_one expects at most 2 arguments");
        }
        const std::string& sql = native_require_string_arg(args, 0, "query_one", "sql");
        const std::vector<Value> params = optional_sql_params(args, 1, "query_one");
        out = run_query_one(database, sql, params);
        return;
    }
    if (name == "prepare") {
        native_require_argc("prepare", args, 1);
        const std::string& sql = native_require_string_arg(args, 0, "prepare", "sql");
        out = run_prepare(database, sql);
        return;
    }
    if (name == "async_execute") {
        native_require_min_argc("async_execute", args, 1);
        if (args.size() > 2) {
            native_value_error("async_execute expects at most 2 arguments");
        }
        const std::string& sql = native_require_string_arg(args, 0, "async_execute", "sql");
        const std::vector<Value> params = optional_sql_params(args, 1, "async_execute");
        out = run_async(runtime, SqlAsyncOperation::Execute, database, sql, params, transaction);
        return;
    }
    if (name == "async_query") {
        native_require_min_argc("async_query", args, 1);
        if (args.size() > 2) {
            native_value_error("async_query expects at most 2 arguments");
        }
        const std::string& sql = native_require_string_arg(args, 0, "async_query", "sql");
        const std::vector<Value> params = optional_sql_params(args, 1, "async_query");
        out = run_async(runtime, SqlAsyncOperation::Query, database, sql, params, transaction);
        return;
    }
    if (name == "async_query_one") {
        native_require_min_argc("async_query_one", args, 1);
        if (args.size() > 2) {
            native_value_error("async_query_one expects at most 2 arguments");
        }
        const std::string& sql = native_require_string_arg(args, 0, "async_query_one", "sql");
        const std::vector<Value> params = optional_sql_params(args, 1, "async_query_one");
        out = run_async(runtime, SqlAsyncOperation::QueryOne, database, sql, params, transaction);
        return;
    }
    if (name == "async_prepare") {
        native_require_argc("async_prepare", args, 1);
        const std::string& sql = native_require_string_arg(args, 0, "async_prepare", "sql");
        out = run_async(runtime, SqlAsyncOperation::Prepare, database, sql, {}, transaction);
        return;
    }
    native_type_error(std::string(on_transaction ? "Transaction" : "Database") + " has no method '" +
                      std::string(name) + "'");
}

}  // namespace

Value native_sql_open(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("open", args, 1);
    const std::string& path = native_require_string_arg(args, 0, "open", "path");
    std::shared_ptr<SqlConnectionHandle> connection = SqlConnectionHandle::open(path);
    return make_database_value(runtime, std::move(connection), path);
}

Value call_native_database_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                  Runtime& runtime) {
    NativeDatabaseData& data = require_database(object);
    if (name == "close") {
        native_require_argc("close", args, 0);
        if (!data.closed) {
            data.finalize_sql_resources();
            if (data.connection) {
                data.connection->close();
            }
            data.closed = true;
        }
        return Runtime::make_null();
    }
    if (name == "is_open") {
        native_require_argc("is_open", args, 0);
        const bool open = !data.closed && data.connection && data.connection->is_open();
        return Runtime::make_bool(open);
    }
    if (name == "begin") {
        native_require_argc("begin", args, 0);
        require_open_database(object, name);
        const std::shared_ptr<NativeDatabaseData> database = database_ptr(object);
        return make_transaction_value(database, SqlTransactionState::begin(database));
    }
    require_open_database(object, name);
    const std::shared_ptr<NativeDatabaseData> database = database_ptr(object);
    Value result = Runtime::make_null();
    dispatch_sql_query_methods(runtime, database, nullptr, name, args, result, false);
    return result;
}

Value call_native_sql_transaction_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                         Runtime& runtime) {
    if (name == "commit") {
        native_require_argc("commit", args, 0);
        const std::shared_ptr<SqlTransactionState> transaction = require_active_transaction(object, name);
        transaction->commit();
        return Runtime::make_null();
    }
    if (name == "rollback") {
        native_require_argc("rollback", args, 0);
        NativeSqlTransactionData& data = require_transaction(object);
        if (!data.state) {
            native_value_error("rollback on an invalid transaction");
        }
        data.state->invalidate_if_database_closed();
        if (data.state->status() == SqlTransactionStatus::RolledBack) {
            return Runtime::make_null();
        }
        if (data.state->status() == SqlTransactionStatus::Committed) {
            native_value_error("rollback on a committed transaction");
        }
        data.state->rollback();
        return Runtime::make_null();
    }
    if (name == "savepoint") {
        native_require_argc("savepoint", args, 1);
        const std::shared_ptr<SqlTransactionState> transaction = require_active_transaction(object, name);
        const std::string& point = native_require_string_arg(args, 0, "savepoint", "name");
        transaction->savepoint(point);
        return Runtime::make_null();
    }
    if (name == "rollback_to") {
        native_require_argc("rollback_to", args, 1);
        const std::shared_ptr<SqlTransactionState> transaction = require_active_transaction(object, name);
        const std::string& point = native_require_string_arg(args, 0, "rollback_to", "name");
        transaction->rollback_to(point);
        return Runtime::make_null();
    }
    if (name == "release") {
        native_require_argc("release", args, 1);
        const std::shared_ptr<SqlTransactionState> transaction = require_active_transaction(object, name);
        const std::string& point = native_require_string_arg(args, 0, "release", "name");
        transaction->release(point);
        return Runtime::make_null();
    }
    const std::shared_ptr<NativeDatabaseData> database = transaction_database(object, name);
    require_active_transaction(object, name);
    const std::shared_ptr<SqlTransactionState> transaction = require_transaction(object).state;
    Value result = Runtime::make_null();
    dispatch_sql_query_methods(runtime, database, transaction, name, args, result, true);
    return result;
}

Value call_native_sql_rows_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    NativeSqlRowsData& data = require_rows(object);
    if (name == "close") {
        native_require_argc("close", args, 0);
        if (data.state) {
            data.state->close();
        }
        return Runtime::make_null();
    }
    if (name == "iter") {
        native_require_argc("iter", args, 0);
        if (!data.state || data.state->is_closed()) {
            native_value_error("cannot iterate a closed SQL result set");
        }
        return runtime_get_iter(object);
    }
    native_type_error("Rows has no method '" + std::string(name) + "'");
}

Value call_native_sql_statement_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    NativeSqlStatementData& data = require_statement(object);
    if (name == "close") {
        native_require_argc("close", args, 0);
        if (data.state) {
            data.state->close();
        }
        return Runtime::make_null();
    }
    NativeSqlStatementData& open_data = require_open_statement(object, name);
    if (name == "execute") {
        if (args.size() > 1) {
            native_value_error("execute expects at most 1 argument");
        }
        const std::vector<Value> params = optional_sql_params(args, 0, "execute");
        return make_sql_execute_result(open_data.state->execute(params));
    }
    if (name == "query") {
        if (args.size() > 1) {
            native_value_error("query expects at most 1 argument");
        }
        const std::vector<Value> params = optional_sql_params(args, 0, "query");
        const std::shared_ptr<NativeDatabaseData> owner = open_data.owner_database.lock();
        if (!owner) {
            native_value_error("cannot query from a statement whose database is closed");
        }
        return make_rows_value(owner, open_data.state->query(params));
    }
    if (name == "query_one") {
        if (args.size() > 1) {
            native_value_error("query_one expects at most 1 argument");
        }
        const std::vector<Value> params = optional_sql_params(args, 0, "query_one");
        return open_data.state->query_one(params);
    }
    native_type_error("Statement has no method '" + std::string(name) + "'");
}

bool is_native_database(const Value& object) {
    return object.kind == ValueKind::Class && object.class_value && object.class_value->native_database;
}

bool is_native_sql_statement(const Value& object) {
    return object.kind == ValueKind::Class && object.class_value && object.class_value->native_sql_statement;
}

bool is_native_sql_transaction(const Value& object) {
    return object.kind == ValueKind::Class && object.class_value && object.class_value->native_sql_transaction;
}

}  // namespace pscript
