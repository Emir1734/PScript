#pragma once

#include "runtime/runtime.h"

#include <memory>
#include <string>

struct sqlite3;

namespace pscript {

enum class SqlTransactionStatus : std::uint8_t {
    Active = 0,
    Committed,
    RolledBack,
};

class SqlTransactionState {
public:
    static std::shared_ptr<SqlTransactionState> begin(const std::shared_ptr<NativeDatabaseData>& database);

    SqlTransactionStatus status() const { return status_; }
    bool is_active() const { return status_ == SqlTransactionStatus::Active; }

    void commit();
    void rollback();
    void savepoint(const std::string& name);
    void rollback_to(const std::string& name);
    void release(const std::string& name);

    std::shared_ptr<NativeDatabaseData> database() const;

    void require_active(std::string_view operation) const;
    void invalidate_if_database_closed();

private:
    explicit SqlTransactionState(std::shared_ptr<NativeDatabaseData> database);

    void exec_control_sql(const std::string& sql);
    static std::string validate_savepoint_name(const std::string& name);

    std::shared_ptr<NativeDatabaseData> database_;
    SqlTransactionStatus status_ = SqlTransactionStatus::Active;
};

void sql_validate_savepoint_name(const std::string& name);

}  // namespace pscript
