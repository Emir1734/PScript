#include "stdlib/stdlib.h"
#include "stdlib/native_boundary.h"
#include "stdlib/native_impl.h"
#include "stdlib/native_async_stream.h"
#include "stdlib/native_archive.h"
#include "runtime/iterator.h"

namespace pscript {

bool is_native_class_object(const Value& object) {
    return is_native_collection(object) || is_native_datetime(object) || is_native_duration(object) ||
           (object.kind == ValueKind::Class && object.class_value && object.class_value->native_file) ||
           is_native_network(object) || is_native_http_server(object) || is_native_crypto_hasher(object) ||
           is_native_database(object) || is_native_sql_rows(object) || is_native_sql_statement(object) ||
           is_native_sql_transaction(object) ||            is_native_task_group(object) || is_native_async_stream(object) || is_native_archive(object);
}

NativeClassKind native_class_kind(const Value& object) {
    if (is_native_collection(object)) {
        return native_collection_kind(object);
    }
    if (is_native_datetime(object)) {
        return NativeClassKind::DateTime;
    }
    if (is_native_duration(object)) {
        return NativeClassKind::Duration;
    }
    if (object.kind == ValueKind::Class && object.class_value && object.class_value->native_file) {
        return static_cast<NativeClassKind>(object.class_value->native_file->kind);
    }
    if (is_native_network(object)) {
        return static_cast<NativeClassKind>(object.class_value->native_network->kind);
    }
    if (is_native_http_server(object)) {
        return NativeClassKind::HttpServer;
    }
    if (is_native_crypto_hasher(object)) {
        return NativeClassKind::CryptoHasher;
    }
    if (is_native_database(object)) {
        return NativeClassKind::Database;
    }
    if (is_native_sql_rows(object)) {
        return NativeClassKind::SqlRows;
    }
    if (is_native_sql_statement(object)) {
        return NativeClassKind::SqlStatement;
    }
    if (is_native_sql_transaction(object)) {
        return NativeClassKind::SqlTransaction;
    }
    if (is_native_task_group(object)) {
        return NativeClassKind::TaskGroup;
    }
    if (is_native_async_stream(object)) {
        return NativeClassKind::AsyncStream;
    }
    if (is_native_archive(object)) {
        return NativeClassKind::Archive;
    }
    return NativeClassKind::None;
}

Value call_native_class_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                               Runtime& runtime) try {
    if (native_boundary::test_fault_hook != nullptr) {
        native_boundary::test_fault_hook();
    }
    if (is_native_collection(object)) {
        return call_native_collection_method(object, name, args);
    }
    if (is_native_datetime(object)) {
        if (name == "format") {
            return native_datetime_format_method(object, args);
        }
        throw RuntimeError("DateTime has no method '" + std::string(name) + "'", RuntimeErrorCategory::Type);
    }
    if (object.kind == ValueKind::Class && object.class_value && object.class_value->native_file) {
        return call_native_file_handle_method(object, name, args, runtime);
    }
    if (is_native_network(object)) {
        return call_native_network_method(object, name, args, runtime);
    }
    if (is_native_http_server(object)) {
        return call_native_http_method(object, name, args, runtime);
    }
    if (is_native_crypto_hasher(object)) {
        return call_native_crypto_hasher_method(object, name, args);
    }
    if (is_native_database(object)) {
        return call_native_database_method(object, name, args, runtime);
    }
    if (is_native_sql_rows(object)) {
        return call_native_sql_rows_method(object, name, args);
    }
    if (is_native_task_group(object)) {
        return call_native_task_group_method(object, name, args, runtime);
    }
    if (is_native_async_stream(object)) {
        return call_native_async_stream_method(object, name, args, runtime);
    }
    if (is_native_sql_statement(object)) {
        return call_native_sql_statement_method(object, name, args);
    }
    if (is_native_sql_transaction(object)) {
        return call_native_sql_transaction_method(object, name, args, runtime);
    }
    if (is_native_archive(object)) {
        return call_native_archive_method(object, name, args, runtime);
    }
    throw RuntimeError("invalid native class method call");
} catch (const RuntimeError&) {
    throw;
} catch (const ProcessExit&) {
    throw;
} catch (const std::bad_alloc&) {
    throw RuntimeError("out of memory");
} catch (const std::exception& ex) {
    throw RuntimeError(std::string(ex.what()));
}

}  // namespace pscript
