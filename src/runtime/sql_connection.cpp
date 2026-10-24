#include "runtime/sql_connection.h"

#include "runtime/runtime.h"

#include <filesystem>
#include <string>

#if defined(PSCRIPT_HAS_SQLITE)
#include <sqlite3.h>
#endif

namespace pscript {
namespace {

#if defined(PSCRIPT_HAS_SQLITE)

[[noreturn]] void sql_fail(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Runtime);
}

[[noreturn]] void sql_value_error(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Value);
}

std::string sqlite_error_message(sqlite3* db, int code) {
    if (db != nullptr) {
        const char* message = sqlite3_errmsg(db);
        if (message != nullptr && message[0] != '\0') {
            return std::string(message);
        }
    }
    return "SQLite error code " + std::to_string(code);
}

#endif

}  // namespace

struct SqlConnectionHandle::State {
#if defined(PSCRIPT_HAS_SQLITE)
    sqlite3* db = nullptr;
#endif
    bool closed = true;
    std::string path;
};

SqlConnectionHandle::SqlConnectionHandle() : state_(std::make_unique<State>()) {}

SqlConnectionHandle::~SqlConnectionHandle() {
    close();
}

bool sql_backend_available() {
#if defined(PSCRIPT_HAS_SQLITE)
    return true;
#else
    return false;
#endif
}

std::shared_ptr<SqlConnectionHandle> SqlConnectionHandle::open(std::string_view path) {
    if (!sql_backend_available()) {
        sql_fail("SQL support is not available in this build");
    }
#if defined(PSCRIPT_HAS_SQLITE)
    if (path.empty()) {
        sql_value_error("database path must not be empty");
    }
    const std::string path_string(path);
    if (path_string != ":memory:") {
        const std::filesystem::path file_path(path_string);
        const std::filesystem::path parent = file_path.parent_path();
        if (!parent.empty() && !std::filesystem::exists(parent)) {
            sql_fail("failed to open database '" + path_string + "': parent directory does not exist");
        }
        if (std::filesystem::exists(file_path) && std::filesystem::is_directory(file_path)) {
            sql_fail("failed to open database '" + path_string + "': path is a directory");
        }
    }
    auto handle = std::shared_ptr<SqlConnectionHandle>(new SqlConnectionHandle());
    handle->state_->path = path_string;
    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (path_string == ":memory:") {
        flags |= SQLITE_OPEN_MEMORY;
    }
    int rc = sqlite3_open_v2(path_string.c_str(), &handle->state_->db, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string detail = sqlite_error_message(handle->state_->db, rc);
        if (handle->state_->db != nullptr) {
            sqlite3_close(handle->state_->db);
            handle->state_->db = nullptr;
        }
        sql_fail("failed to open database '" + path_string + "': " + detail);
    }
    handle->state_->closed = false;
    return handle;
#else
    sql_fail("SQL support is not available in this build");
#endif
}

void SqlConnectionHandle::close() {
    if (state_->closed) {
        return;
    }
#if defined(PSCRIPT_HAS_SQLITE)
    if (state_->db != nullptr) {
        sqlite3_close(state_->db);
        state_->db = nullptr;
    }
#endif
    state_->closed = true;
}

bool SqlConnectionHandle::is_open() const {
    return !state_->closed;
}

sqlite3* SqlConnectionHandle::raw() const {
#if defined(PSCRIPT_HAS_SQLITE)
    return state_->closed ? nullptr : state_->db;
#else
    return nullptr;
#endif
}

}  // namespace pscript
