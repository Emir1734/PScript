#include "runtime/sql_ops.h"

#include "runtime/sql_session.h"

#include <sqlite3.h>

#include <cstring>
#include <string>

namespace pscript {
namespace {

constexpr std::uint32_t kNativeSqlRowsKind = 15;
constexpr std::uint32_t kNativeSqlStatementKind = 16;

[[noreturn]] void sql_fail(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Runtime);
}

[[noreturn]] void sql_value_error(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Value);
}

[[noreturn]] void sql_type_error(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Type);
}

std::string sqlite_message(sqlite3* db, int code) {
    if (db != nullptr) {
        const char* message = sqlite3_errmsg(db);
        if (message != nullptr && message[0] != '\0') {
            return std::string(message);
        }
    }
    return "SQLite error code " + std::to_string(code);
}

std::vector<std::string> read_column_names(sqlite3_stmt* stmt) {
    const int count = sqlite3_column_count(stmt);
    std::vector<std::string> columns;
    columns.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const char* name = sqlite3_column_name(stmt, i);
        columns.emplace_back(name != nullptr ? name : "");
    }
    return columns;
}

void check_prepare_rc(sqlite3* db, int rc, sqlite3_stmt* stmt) {
    if (rc != SQLITE_OK) {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
        sql_fail("failed to prepare SQL statement: " + sqlite_message(db, rc));
    }
}

void check_step_rc(sqlite3* db, int rc, const char* context) {
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
        return;
    }
    sql_fail(std::string(context) + ": " + sqlite_message(db, rc));
}

}  // namespace

void sql_bind_params(sqlite3_stmt* stmt, const std::vector<Value>& params) {
    const int expected = sqlite3_bind_parameter_count(stmt);
    if (static_cast<int>(params.size()) != expected) {
        sql_value_error("SQL parameter count mismatch: expected " + std::to_string(expected) + ", got " +
                        std::to_string(params.size()));
    }
    for (int i = 0; i < expected; ++i) {
        const int index = i + 1;
        const Value& param = params[static_cast<std::size_t>(i)];
        int rc = SQLITE_OK;
        switch (param.kind) {
            case ValueKind::Integer:
                rc = sqlite3_bind_int64(stmt, index, param.integer);
                break;
            case ValueKind::Float:
                rc = sqlite3_bind_double(stmt, index, param.floating);
                break;
            case ValueKind::String:
                rc = sqlite3_bind_text(stmt, index, param.string.c_str(), static_cast<int>(param.string.size()),
                                       SQLITE_TRANSIENT);
                break;
            case ValueKind::Bytes:
                if (!param.bytes) {
                    sql_type_error("invalid bytes SQL parameter");
                }
                rc = sqlite3_bind_blob(stmt, index, param.bytes->data.data(),
                                       static_cast<int>(param.bytes->data.size()), SQLITE_TRANSIENT);
                break;
            case ValueKind::Null:
                rc = sqlite3_bind_null(stmt, index);
                break;
            default:
                sql_type_error("unsupported SQL parameter type: " + std::string(Runtime::format(param)));
        }
        if (rc != SQLITE_OK) {
            sql_fail("failed to bind SQL parameter " + std::to_string(index));
        }
    }
}

Value sql_row_from_statement(sqlite3_stmt* stmt, const std::vector<std::string>& columns) {
    const int count = sqlite3_column_count(stmt);
    if (count != static_cast<int>(columns.size())) {
        sql_fail("SQL column metadata mismatch");
    }
    std::vector<std::string> field_names = columns;
    std::vector<Value> fields;
    fields.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_INTEGER:
                fields.push_back(Runtime::make_integer(sqlite3_column_int64(stmt, i)));
                break;
            case SQLITE_FLOAT:
                fields.push_back(Runtime::make_float(sqlite3_column_double(stmt, i)));
                break;
            case SQLITE_TEXT: {
                const unsigned char* text = sqlite3_column_text(stmt, i);
                const int bytes = sqlite3_column_bytes(stmt, i);
                if (text == nullptr || bytes == 0) {
                    fields.push_back(Runtime::make_string(""));
                } else {
                    fields.push_back(Runtime::make_string(
                        std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes))));
                }
                break;
            }
            case SQLITE_BLOB: {
                const void* blob = sqlite3_column_blob(stmt, i);
                const int bytes = sqlite3_column_bytes(stmt, i);
                std::vector<std::uint8_t> data;
                if (blob != nullptr && bytes > 0) {
                    const auto* begin = static_cast<const std::uint8_t*>(blob);
                    data.assign(begin, begin + bytes);
                }
                fields.push_back(Runtime::make_bytes(std::move(data)));
                break;
            }
            case SQLITE_NULL:
                fields.push_back(Runtime::make_null());
                break;
            default:
                sql_fail("unsupported SQL column type");
        }
    }
    Value row = Runtime::make_class(0, "Row", std::move(field_names));
    row.class_value->fields = std::move(fields);
    return row;
}

Value make_sql_row(sqlite3_stmt* stmt) {
    return sql_row_from_statement(stmt, read_column_names(stmt));
}

Value make_sql_execute_result(const SqlExecuteOutcome& outcome) {
    Value result = Runtime::make_class(0, "ExecuteResult", {"affected_rows", "last_insert_id"});
    result.class_value->fields[0] = Runtime::make_integer(outcome.affected_rows);
    result.class_value->fields[1] = Runtime::make_integer(outcome.last_insert_id);
    return result;
}

SqlRowsState::SqlRowsState(std::weak_ptr<NativeDatabaseData> owner, sqlite3* db, sqlite3_stmt* stmt,
                           std::vector<std::string> columns)
    : owner_(std::move(owner)), db_(db), stmt_(stmt), columns_(std::move(columns)) {}

SqlRowsState::~SqlRowsState() {
    close();
}

std::shared_ptr<SqlRowsState> SqlRowsState::create(const std::shared_ptr<NativeDatabaseData>& owner, sqlite3* db,
                                                  sqlite3_stmt* stmt, std::vector<std::string> columns) {
    std::weak_ptr<NativeDatabaseData> weak_owner = owner;
    return std::shared_ptr<SqlRowsState>(new SqlRowsState(std::move(weak_owner), db, stmt, std::move(columns)));
}

void SqlRowsState::close() {
    if (closed_) {
        return;
    }
    SqlDatabaseLock guard(owner_);
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
    closed_ = true;
    exhausted_ = true;
}

Value SqlRowsState::next_row() {
    SqlDatabaseLock guard(owner_);
    if (closed_ || stmt_ == nullptr) {
        sql_value_error("cannot read from a closed SQL result set");
    }
    if (exhausted_) {
        throw RuntimeError("StopIteration", RuntimeErrorCategory::Runtime);
    }
    sqlite3* db = guard.db();
    if (db == nullptr) {
        sql_value_error("cannot read from a closed SQL result set");
    }
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_DONE) {
        exhausted_ = true;
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
            stmt_ = nullptr;
        }
        closed_ = true;
        throw RuntimeError("StopIteration", RuntimeErrorCategory::Runtime);
    }
    check_step_rc(db, rc, "SQL row iteration");
    return sql_row_from_statement(stmt_, columns_);
}

SqlStatementState::~SqlStatementState() {
    close();
}

SqlStatementState::SqlStatementState(std::weak_ptr<NativeDatabaseData> owner, sqlite3* db, sqlite3_stmt* stmt)
    : owner_(std::move(owner)), db_(db), stmt_(stmt) {}

std::shared_ptr<SqlStatementState> SqlStatementState::prepare(const std::shared_ptr<NativeDatabaseData>& owner,
                                                              const std::string& sql) {
    SqlDatabaseLock guard(owner);
    sqlite3* db = guard.db();
    if (db == nullptr) {
        sql_value_error("prepare on a closed database");
    }
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
    check_prepare_rc(db, rc, stmt);
    std::weak_ptr<NativeDatabaseData> weak_owner = owner;
    return std::shared_ptr<SqlStatementState>(new SqlStatementState(std::move(weak_owner), db, stmt));
}

void SqlStatementState::close() {
    if (closed_) {
        return;
    }
    SqlDatabaseLock guard(owner_);
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
    closed_ = true;
}

void SqlStatementState::reset_and_bind(const std::vector<Value>& params) {
    if (closed_ || stmt_ == nullptr) {
        sql_value_error("cannot use a closed prepared statement");
    }
    sqlite3* db = db_;
    if (db == nullptr) {
        sql_value_error("cannot use a closed prepared statement");
    }
    int rc = sqlite3_reset(stmt_);
    if (rc != SQLITE_OK && rc != SQLITE_DONE) {
        sql_fail("failed to reset prepared statement: " + sqlite_message(db_, rc));
    }
    rc = sqlite3_clear_bindings(stmt_);
    if (rc != SQLITE_OK) {
        sql_fail("failed to clear SQL bindings: " + sqlite_message(db_, rc));
    }
    sql_bind_params(stmt_, params);
}

SqlExecuteOutcome SqlStatementState::execute(const std::vector<Value>& params) {
    SqlDatabaseLock guard(owner_);
    reset_and_bind(params);
    int rc = sqlite3_step(stmt_);
    while (rc == SQLITE_ROW) {
        rc = sqlite3_step(stmt_);
    }
    check_step_rc(db_, rc, "SQL execute");
    SqlExecuteOutcome outcome;
    outcome.affected_rows = static_cast<std::int64_t>(sqlite3_changes(db_));
    outcome.last_insert_id = static_cast<std::int64_t>(sqlite3_last_insert_rowid(db_));
    return outcome;
}

std::shared_ptr<SqlRowsState> SqlStatementState::query(const std::vector<Value>& params) {
    SqlDatabaseLock guard(owner_);
    reset_and_bind(params);
    const std::vector<std::string> columns = read_column_names(stmt_);
    sqlite3_stmt* detached = stmt_;
    stmt_ = nullptr;
    closed_ = true;
    const std::shared_ptr<NativeDatabaseData> owner = owner_.lock();
    if (!owner) {
        if (detached != nullptr) {
            sqlite3_finalize(detached);
        }
        sql_value_error("cannot query from a closed database");
    }
    return SqlRowsState::create(owner, db_, detached, columns);
}

Value SqlStatementState::query_one(const std::vector<Value>& params) {
    SqlDatabaseLock guard(owner_);
    reset_and_bind(params);
    const std::vector<std::string> columns = read_column_names(stmt_);
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_DONE) {
        int reset_rc = sqlite3_reset(stmt_);
        if (reset_rc != SQLITE_OK && reset_rc != SQLITE_DONE) {
            sql_fail("failed to reset prepared statement: " + sqlite_message(db_, reset_rc));
        }
        sqlite3_clear_bindings(stmt_);
        return Runtime::make_null();
    }
    check_step_rc(db_, rc, "SQL query_one");
    Value row = sql_row_from_statement(stmt_, columns);
    int drain_rc = sqlite3_step(stmt_);
    while (drain_rc == SQLITE_ROW) {
        drain_rc = sqlite3_step(stmt_);
    }
    check_step_rc(db_, drain_rc, "SQL query_one");
    int reset_rc = sqlite3_reset(stmt_);
    if (reset_rc != SQLITE_OK && reset_rc != SQLITE_DONE) {
        sql_fail("failed to reset prepared statement: " + sqlite_message(db_, reset_rc));
    }
    sqlite3_clear_bindings(stmt_);
    return row;
}

SqlExecuteOutcome sql_execute_sql_unlocked(sqlite3* db, const std::string& sql, const std::vector<Value>& params) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr);
    check_prepare_rc(db, rc, stmt);
    sql_bind_params(stmt, params);
    int step_rc = sqlite3_step(stmt);
    while (step_rc == SQLITE_ROW) {
        step_rc = sqlite3_step(stmt);
    }
    check_step_rc(db, step_rc, "SQL execute");
    SqlExecuteOutcome outcome;
    outcome.affected_rows = static_cast<std::int64_t>(sqlite3_changes(db));
    outcome.last_insert_id = static_cast<std::int64_t>(sqlite3_last_insert_rowid(db));
    sqlite3_finalize(stmt);
    return outcome;
}

std::shared_ptr<SqlRowsState> sql_query_sql_unlocked(const std::shared_ptr<NativeDatabaseData>& owner, sqlite3* db,
                                                     const std::string& sql, const std::vector<Value>& params) {
    (void)db;
    auto statement = SqlStatementState::prepare(owner, sql);
    return statement->query(params);
}

Value sql_query_one_sql_unlocked(const std::shared_ptr<NativeDatabaseData>& owner, sqlite3* db, const std::string& sql,
                                 const std::vector<Value>& params) {
    (void)db;
    auto statement = SqlStatementState::prepare(owner, sql);
    return statement->query_one(params);
}

SqlExecuteOutcome sql_execute_sql(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                                  const std::vector<Value>& params) {
    SqlDatabaseLock guard(database);
    if (!guard.db()) {
        sql_value_error("execute on a closed database");
    }
    return sql_execute_sql_unlocked(guard.db(), sql, params);
}

std::shared_ptr<SqlRowsState> sql_query_sql(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                                            const std::vector<Value>& params) {
    SqlDatabaseLock guard(database);
    if (!guard.db()) {
        sql_value_error("query on a closed database");
    }
    return sql_query_sql_unlocked(database, guard.db(), sql, params);
}

Value sql_query_one_sql(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                        const std::vector<Value>& params) {
    SqlDatabaseLock guard(database);
    if (!guard.db()) {
        sql_value_error("query_one on a closed database");
    }
    return sql_query_one_sql_unlocked(database, guard.db(), sql, params);
}

std::shared_ptr<SqlStatementState> sql_prepare_statement(const std::shared_ptr<NativeDatabaseData>& database,
                                                         const std::string& sql) {
    return SqlStatementState::prepare(database, sql);
}

Value sql_attach_rows(std::uint32_t class_index, const std::shared_ptr<NativeDatabaseData>& database,
                      const std::shared_ptr<SqlRowsState>& state) {
    auto rows_data = std::make_shared<NativeSqlRowsData>();
    rows_data->kind = kNativeSqlRowsKind;
    rows_data->state = state;
    if (database) {
        database->track_row_result(state);
    }
    Value value = Runtime::make_class(class_index, "Rows", std::vector<std::string>{});
    value.class_value->native_sql_rows = std::move(rows_data);
    return value;
}

Value sql_attach_statement(std::uint32_t class_index, const std::shared_ptr<NativeDatabaseData>& database,
                           const std::shared_ptr<SqlStatementState>& state) {
    auto statement_data = std::make_shared<NativeSqlStatementData>();
    statement_data->kind = kNativeSqlStatementKind;
    statement_data->state = state;
    statement_data->owner_database = database;
    if (database) {
        database->track_prepared_statement(state);
    }
    Value value = Runtime::make_class(class_index, "Statement", std::vector<std::string>{});
    value.class_value->native_sql_statement = std::move(statement_data);
    return value;
}

void sql_interrupt_database(const std::shared_ptr<NativeDatabaseData>& database) {
    if (!database || !database->connection) {
        return;
    }
    SqlDatabaseLock guard(database);
    if (sqlite3* db = guard.db()) {
        sqlite3_interrupt(db);
    }
}

}  // namespace pscript
