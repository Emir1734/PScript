#pragma once

#include <string>

struct pg_conn;

using PGconn = pg_conn;

namespace pscript::registry {

bool run_registry_postgres_migrations(PGconn* conn, std::string* error);

}  // namespace pscript::registry
