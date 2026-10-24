#pragma once

#include "runtime/runtime.h"
#include "runtime/sql_transaction.h"

#include <memory>
#include <string>
#include <vector>

namespace pscript {

enum class SqlAsyncOperation : std::uint8_t {
    Execute = 1,
    Query,
    QueryOne,
    Prepare,
};

Value submit_sql_async(Runtime& runtime, SqlAsyncOperation operation, const std::shared_ptr<NativeDatabaseData>& database,
                       const std::string& sql, const std::vector<Value>& params,
                       const std::shared_ptr<SqlTransactionState>& transaction, std::uint32_t statement_class_index,
                       std::uint32_t rows_class_index);

}  // namespace pscript
