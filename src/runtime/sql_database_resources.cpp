#include "runtime/sql_connection.h"
#include "runtime/sql_ops.h"
#include "runtime/sql_transaction.h"

namespace pscript {

void NativeDatabaseData::track_row_result(const std::shared_ptr<SqlRowsState>& state) {
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    row_results.push_back(state);
}

void NativeDatabaseData::track_prepared_statement(const std::shared_ptr<SqlStatementState>& state) {
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    prepared_statements.push_back(state);
}

void NativeDatabaseData::track_transaction(const std::shared_ptr<SqlTransactionState>& state) {
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> lock(resource_mutex);
    transactions.push_back(state);
}

void NativeDatabaseData::rollback_active_transactions() {
    std::vector<std::shared_ptr<SqlTransactionState>> active;
    {
        std::lock_guard<std::mutex> lock(resource_mutex);
        for (auto it = transactions.begin(); it != transactions.end();) {
            if (const std::shared_ptr<SqlTransactionState> state = it->lock()) {
                if (state->is_active()) {
                    active.push_back(state);
                }
            }
            it = transactions.erase(it);
        }
    }
    for (const std::shared_ptr<SqlTransactionState>& transaction : active) {
        try {
            transaction->rollback();
        } catch (...) {
        }
    }
}

void NativeDatabaseData::finalize_sql_resources() {
    rollback_active_transactions();
    std::lock_guard<std::mutex> lock(resource_mutex);
    for (auto it = row_results.begin(); it != row_results.end();) {
        if (const std::shared_ptr<SqlRowsState> state = it->lock()) {
            state->close();
        }
        it = row_results.erase(it);
    }
    for (auto it = prepared_statements.begin(); it != prepared_statements.end();) {
        if (const std::shared_ptr<SqlStatementState> state = it->lock()) {
            state->close();
        }
        it = prepared_statements.erase(it);
    }
}

}  // namespace pscript
