#include "runtime/sql_async.h"

#include "runtime/async_io.h"

namespace pscript {

Value submit_sql_async(Runtime& runtime, SqlAsyncOperation operation, const std::shared_ptr<NativeDatabaseData>& database,
                       const std::string& sql, const std::vector<Value>& params,
                       const std::shared_ptr<SqlTransactionState>& transaction, std::uint32_t statement_class_index,
                       std::uint32_t rows_class_index) {
    return runtime.async_io().submit_sql_operation(static_cast<std::uint32_t>(operation), database, sql, params,
                                                   transaction, statement_class_index, rows_class_index);
}

}  // namespace pscript
