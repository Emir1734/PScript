#include "runtime/sql_session.h"

#include "runtime/sql_connection.h"
#include "runtime/sql_ops.h"

namespace pscript {

SqlDatabaseLock::SqlDatabaseLock(const std::shared_ptr<NativeDatabaseData>& database)
    : database_(database) {
    if (database_) {
        lock_ = std::unique_lock<std::recursive_mutex>(database_->exec_mutex);
    }
}

SqlDatabaseLock::SqlDatabaseLock(const std::weak_ptr<NativeDatabaseData>& database) : SqlDatabaseLock(database.lock()) {}

sqlite3* SqlDatabaseLock::db() const {
    if (!database_ || database_->closed || !database_->connection) {
        return nullptr;
    }
    return database_->connection->raw();
}

}  // namespace pscript
