#pragma once

#include <string>

struct sqlite3;

namespace pscript::registry {

bool run_registry_migrations(sqlite3* db, std::string* error);

}  // namespace pscript::registry
