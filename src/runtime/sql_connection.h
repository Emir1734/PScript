#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct sqlite3;

namespace pscript {

// V1 SQLite-backed connection; driver boundary for future backends.
class SqlConnectionHandle {
public:
    SqlConnectionHandle();
    ~SqlConnectionHandle();

    SqlConnectionHandle(const SqlConnectionHandle&) = delete;
    SqlConnectionHandle& operator=(const SqlConnectionHandle&) = delete;

    static std::shared_ptr<SqlConnectionHandle> open(std::string_view path);

    void close();
    bool is_open() const;

    sqlite3* raw() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

bool sql_backend_available();

}  // namespace pscript
