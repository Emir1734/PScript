#include "registry/db/registry_db.h"

#include "registry/db/sqlite_registry_db.h"

#ifdef PSCRIPT_HAS_LIBPQ
#include "registry/db/postgres_registry_db.h"
#endif

#include <filesystem>
#include <string>

namespace pscript::registry {
namespace {

std::filesystem::path sqlite_path_from_url(const std::string& database_url) {
    constexpr std::string_view kPrefix = "sqlite:";
    if (database_url.rfind(kPrefix, 0) == 0) {
        return std::filesystem::path(database_url.substr(kPrefix.size()));
    }
    return std::filesystem::path(database_url);
}

}  // namespace

std::unique_ptr<RegistryDatabase> make_registry_database(const std::string& database_url) {
    if (database_url.rfind("postgres://", 0) == 0 || database_url.rfind("postgresql://", 0) == 0) {
#ifdef PSCRIPT_HAS_LIBPQ
        return std::make_unique<PostgresRegistryDatabase>(database_url);
#else
        return nullptr;
#endif
    }
    return std::make_unique<SqliteRegistryDatabase>(sqlite_path_from_url(database_url));
}

}  // namespace pscript::registry
