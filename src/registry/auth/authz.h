#pragma once

#include "package/package_identity.h"
#include "registry/db/registry_db.h"

#include <string>

namespace pscript::registry {

struct AuthenticatedPrincipal {
    std::string user_id;
    std::string username;
};

class RegistryAuthz {
public:
    explicit RegistryAuthz(RegistryDatabase& database);

    bool can_manage_package(const AuthenticatedPrincipal& principal, const package::ScopedPackageId& package_id,
                            std::string* error = nullptr) const;

private:
    RegistryDatabase& database_;
};

}  // namespace pscript::registry
