#pragma once

#include "runtime/runtime.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace pscript {

struct SqlExecuteOutcome {
    std::int64_t affected_rows = 0;
    std::int64_t last_insert_id = 0;
};

class SqlRowsState {
public:
    ~SqlRowsState();

    SqlRowsState(const SqlRowsState&) = delete;
    SqlRowsState& operator=(const SqlRowsState&) = delete;

    static std::shared_ptr<SqlRowsState> create(const std::shared_ptr<NativeDatabaseData>& owner, sqlite3* db,
                                                sqlite3_stmt* stmt, std::vector<std::string> columns);

    bool is_closed() const { return closed_; }
    void close();
    Value next_row();
    const std::vector<std::string>& columns() const { return columns_; }

private:
    SqlRowsState(std::weak_ptr<NativeDatabaseData> owner, sqlite3* db, sqlite3_stmt* stmt,
                 std::vector<std::string> columns);

    std::weak_ptr<NativeDatabaseData> owner_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    std::vector<std::string> columns_;
    bool exhausted_ = false;
    bool closed_ = false;
};

class SqlStatementState {
public:
    ~SqlStatementState();

    SqlStatementState(const SqlStatementState&) = delete;
    SqlStatementState& operator=(const SqlStatementState&) = delete;

    static std::shared_ptr<SqlStatementState> prepare(const std::shared_ptr<NativeDatabaseData>& owner,
                                                     const std::string& sql);

    bool is_closed() const { return closed_; }
    void close();

    SqlExecuteOutcome execute(const std::vector<Value>& params);
    std::shared_ptr<SqlRowsState> query(const std::vector<Value>& params);
    Value query_one(const std::vector<Value>& params);

private:
    SqlStatementState(std::weak_ptr<NativeDatabaseData> owner, sqlite3* db, sqlite3_stmt* stmt);

    void reset_and_bind(const std::vector<Value>& params);

    std::weak_ptr<NativeDatabaseData> owner_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
    bool closed_ = false;
};

void sql_bind_params(sqlite3_stmt* stmt, const std::vector<Value>& params);
Value sql_row_from_statement(sqlite3_stmt* stmt, const std::vector<std::string>& columns);

SqlExecuteOutcome sql_execute_sql_unlocked(sqlite3* db, const std::string& sql, const std::vector<Value>& params);
std::shared_ptr<SqlRowsState> sql_query_sql_unlocked(const std::shared_ptr<NativeDatabaseData>& owner, sqlite3* db,
                                                     const std::string& sql, const std::vector<Value>& params);
Value sql_query_one_sql_unlocked(const std::shared_ptr<NativeDatabaseData>& owner, sqlite3* db, const std::string& sql,
                                 const std::vector<Value>& params);

SqlExecuteOutcome sql_execute_sql(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                                  const std::vector<Value>& params);
std::shared_ptr<SqlRowsState> sql_query_sql(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                                            const std::vector<Value>& params);
Value sql_query_one_sql(const std::shared_ptr<NativeDatabaseData>& database, const std::string& sql,
                        const std::vector<Value>& params);

std::shared_ptr<SqlStatementState> sql_prepare_statement(const std::shared_ptr<NativeDatabaseData>& database,
                                                         const std::string& sql);

Value make_sql_row(sqlite3_stmt* stmt);
Value make_sql_execute_result(const SqlExecuteOutcome& outcome);

void sql_interrupt_database(const std::shared_ptr<NativeDatabaseData>& database);

Value sql_attach_rows(std::uint32_t class_index, const std::shared_ptr<NativeDatabaseData>& database,
                      const std::shared_ptr<SqlRowsState>& state);
Value sql_attach_statement(std::uint32_t class_index, const std::shared_ptr<NativeDatabaseData>& database,
                           const std::shared_ptr<SqlStatementState>& state);

}  // namespace pscript
