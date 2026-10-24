#include "registry/auth/authz.h"

namespace pscript::registry {

RegistryAuthz::RegistryAuthz(RegistryDatabase& database) : database_(database) {}

bool RegistryAuthz::can_manage_package(const AuthenticatedPrincipal& principal,
                                       const package::ScopedPackageId& package_id, std::string* error) const {
    return database_.user_owns_package(principal.user_id, package_id.username, package_id.package, error);
}

}  // namespace pscript::registry
