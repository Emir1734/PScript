#pragma once

#include "runtime/runtime.h"

#include <memory>
#include <mutex>

struct sqlite3;

namespace pscript {

class SqlDatabaseLock {
public:
    explicit SqlDatabaseLock(const std::shared_ptr<NativeDatabaseData>& database);
    explicit SqlDatabaseLock(const std::weak_ptr<NativeDatabaseData>& database);

    sqlite3* db() const;
    const std::shared_ptr<NativeDatabaseData>& database() const { return database_; }
    bool holds_lock() const { return static_cast<bool>(database_); }

private:
    std::shared_ptr<NativeDatabaseData> database_;
    std::unique_lock<std::recursive_mutex> lock_;
};

}  // namespace pscript
