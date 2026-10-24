#pragma once

#include "registry/auth/authz.h"
#include "registry/auth/rate_limiter.h"
#include "registry/db/registry_db.h"
#include "runtime/crypto_kdf_cipher.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace pscript::registry {

struct AuthSettings {
    std::uint32_t session_ttl_seconds = 30U * 24U * 3600U;
    std::size_t max_auth_body_bytes = 4096;
    std::uint32_t register_rate_limit = 10;
    std::uint32_t login_rate_limit = 30;
    std::chrono::seconds register_rate_window{3600};
    std::chrono::seconds login_rate_window{900};
    pscript::CryptoKdfArgon2Params argon2_params;
};

struct AuthTokenResponse {
    std::string username;
    std::string token;
    std::string expires_at;
};

enum class AuthError {
    Ok,
    InvalidInput,
    UsernameTaken,
    InvalidCredentials,
    Unauthorized,
    RateLimited,
    Unavailable,
};

class AuthService {
public:
    AuthService(RegistryDatabase& database, AuthSettings settings, RateLimiter& rate_limiter);

    void maintenance(std::string* error = nullptr);

    AuthError register_user(const std::string& client_key, const std::string& username, std::string_view password,
                            AuthTokenResponse* response, std::string* public_error = nullptr);

    AuthError login(const std::string& client_key, const std::string& username, std::string_view password,
                    AuthTokenResponse* response, std::string* public_error = nullptr);

    AuthError logout(std::string_view bearer_token, std::string* public_error = nullptr);

    std::optional<AuthenticatedPrincipal> authenticate_bearer(std::string_view bearer_token,
                                                            std::string* public_error = nullptr) const;

    std::string hash_bearer_token(std::string_view bearer_token) const;

    RegistryAuthz& authz() { return authz_; }

private:
    std::optional<std::string> issue_session(const UserRecord& user, AuthTokenResponse* response,
                                             std::string* public_error);

    RegistryDatabase& database_;
    AuthSettings settings_;
    RateLimiter& rate_limiter_;
    RegistryAuthz authz_;
};

}  // namespace pscript::registry
