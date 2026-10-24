#include "runtime/sql_transaction.h"

#include "runtime/sql_ops.h"
#include "runtime/sql_session.h"

#include <cctype>

namespace pscript {
namespace {

[[noreturn]] void tx_fail(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Runtime);
}

[[noreturn]] void tx_value_error(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Value);
}

}  // namespace

void sql_validate_savepoint_name(const std::string& name) {
    if (name.empty()) {
        tx_value_error("savepoint name must not be empty");
    }
    if (name.size() > 128) {
        tx_value_error("savepoint name is too long");
    }
    const unsigned char first = static_cast<unsigned char>(name[0]);
    if (!(std::isalpha(first) != 0 || name[0] == '_')) {
        tx_value_error("invalid savepoint name");
    }
    for (std::size_t i = 1; i < name.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(name[i]);
        if (!(std::isalnum(ch) != 0 || name[i] == '_')) {
            tx_value_error("invalid savepoint name");
        }
    }
}

SqlTransactionState::SqlTransactionState(std::shared_ptr<NativeDatabaseData> database)
    : database_(std::move(database)) {}

std::shared_ptr<SqlTransactionState> SqlTransactionState::begin(const std::shared_ptr<NativeDatabaseData>& database) {
    if (!database || database->closed || !database->connection || !database->connection->is_open()) {
        tx_value_error("begin on a closed database");
    }
    auto transaction = std::shared_ptr<SqlTransactionState>(new SqlTransactionState(database));
    transaction->exec_control_sql("BEGIN");
    database->track_transaction(transaction);
    return transaction;
}

std::shared_ptr<NativeDatabaseData> SqlTransactionState::database() const {
    return database_;
}

void SqlTransactionState::require_active(std::string_view operation) const {
    if (!database_ || database_->closed) {
        tx_value_error(std::string(operation) + " on a closed database");
    }
    if (status_ == SqlTransactionStatus::Committed) {
        tx_value_error(std::string(operation) + " on a committed transaction");
    }
    if (status_ == SqlTransactionStatus::RolledBack) {
        tx_value_error(std::string(operation) + " on a rolled-back transaction");
    }
}

void SqlTransactionState::invalidate_if_database_closed() {
    if (!database_ || database_->closed) {
        if (status_ == SqlTransactionStatus::Active) {
            status_ = SqlTransactionStatus::RolledBack;
        }
    }
}

void SqlTransactionState::exec_control_sql(const std::string& sql) {
    require_active("transaction control");
    SqlDatabaseLock guard(database_);
    if (!guard.db()) {
        tx_value_error("transaction control on a closed database");
    }
    sql_execute_sql_unlocked(guard.db(), sql, {});
}

void SqlTransactionState::commit() {
    if (status_ == SqlTransactionStatus::Committed) {
        return;
    }
    if (status_ == SqlTransactionStatus::RolledBack) {
        tx_value_error("commit on a rolled-back transaction");
    }
    exec_control_sql("COMMIT");
    status_ = SqlTransactionStatus::Committed;
}

void SqlTransactionState::rollback() {
    if (status_ == SqlTransactionStatus::RolledBack) {
        return;
    }
    if (status_ == SqlTransactionStatus::Committed) {
        tx_value_error("rollback on a committed transaction");
    }
    exec_control_sql("ROLLBACK");
    status_ = SqlTransactionStatus::RolledBack;
}

void SqlTransactionState::savepoint(const std::string& name) {
    sql_validate_savepoint_name(name);
    require_active("savepoint");
    exec_control_sql("SAVEPOINT " + name);
}

void SqlTransactionState::rollback_to(const std::string& name) {
    sql_validate_savepoint_name(name);
    require_active("rollback_to");
    exec_control_sql("ROLLBACK TO " + name);
}

void SqlTransactionState::release(const std::string& name) {
    sql_validate_savepoint_name(name);
    require_active("release");
    exec_control_sql("RELEASE " + name);
}

}  // namespace pscript
