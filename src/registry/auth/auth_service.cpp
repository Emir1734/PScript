#include "registry/auth/auth_service.h"

#include "package/package_identity.h"
#include "registry/auth/password_hash.h"
#include "runtime/crypto_ops.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace pscript::registry {
namespace {

std::string expires_at_from_ttl(std::uint32_t ttl_seconds) {
    const std::time_t expires = std::time(nullptr) + static_cast<std::time_t>(ttl_seconds);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &expires);
#else
    gmtime_r(&expires, &utc);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string generate_bearer_token() {
    const std::vector<std::uint8_t> bytes = pscript::crypto_random_bytes(32);
    std::ostringstream out;
    out << "pscr_";
    out << std::hex << std::setfill('0');
    for (std::uint8_t byte : bytes) {
        out.width(2);
        out << static_cast<int>(byte);
    }
    return out.str();
}

std::string bytes_to_hex_lower(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::uint8_t byte : bytes) {
        out.width(2);
        out << static_cast<int>(byte);
    }
    return out.str();
}

}  // namespace

AuthService::AuthService(RegistryDatabase& database, AuthSettings settings, RateLimiter& rate_limiter)
    : database_(database), settings_(settings), rate_limiter_(rate_limiter), authz_(database) {}

void AuthService::maintenance(std::string* error) {
    database_.purge_expired_sessions(error);
}

AuthError AuthService::register_user(const std::string& client_key, const std::string& username,
                                     std::string_view password, AuthTokenResponse* response,
                                     std::string* public_error) {
    if (!rate_limiter_.allow("register:" + client_key, settings_.register_rate_limit, settings_.register_rate_window)) {
        if (public_error) {
            *public_error = "too many requests";
        }
        return AuthError::RateLimited;
    }
    if (!package::is_valid_username(username)) {
        if (public_error) {
            *public_error = "invalid username";
        }
        return AuthError::InvalidInput;
    }
    const std::optional<std::string> password_hash = hash_password_argon2id(password, settings_.argon2_params, public_error);
    if (!password_hash) {
        if (public_error && public_error->empty()) {
            *public_error = "registration is unavailable";
        }
        return AuthError::Unavailable;
    }
    std::string user_id;
    std::string db_error;
    const RegisterUserResult result = database_.register_user(username, *password_hash, &user_id, &db_error);
    if (result == RegisterUserResult::UsernameTaken) {
        if (public_error) {
            *public_error = "username already taken";
        }
        return AuthError::UsernameTaken;
    }
    if (result != RegisterUserResult::Ok) {
        if (public_error) {
            *public_error = "registration failed";
        }
        return AuthError::Unavailable;
    }
    const std::optional<UserRecord> user = database_.find_user_by_username(username, &db_error);
    if (!user) {
        if (public_error) {
            *public_error = "registration failed";
        }
        return AuthError::Unavailable;
    }
    if (!issue_session(*user, response, public_error)) {
        return AuthError::Unavailable;
    }
    return AuthError::Ok;
}

AuthError AuthService::login(const std::string& client_key, const std::string& username, std::string_view password,
                             AuthTokenResponse* response, std::string* public_error) {
    if (!rate_limiter_.allow("login:" + client_key, settings_.login_rate_limit, settings_.login_rate_window)) {
        if (public_error) {
            *public_error = "too many requests";
        }
        return AuthError::RateLimited;
    }
    if (!package::is_valid_username(username)) {
        if (public_error) {
            *public_error = "invalid credentials";
        }
        return AuthError::InvalidCredentials;
    }
    std::string db_error;
    const std::optional<UserRecord> user = database_.find_user_by_username(username, &db_error);
    if (!user || is_disabled_password_hash(user->password_hash) ||
        !verify_password_argon2id(password, user->password_hash)) {
        if (public_error) {
            *public_error = "invalid credentials";
        }
        return AuthError::InvalidCredentials;
    }
    if (!issue_session(*user, response, public_error)) {
        return AuthError::Unavailable;
    }
    return AuthError::Ok;
}

AuthError AuthService::logout(std::string_view bearer_token, std::string* public_error) {
    if (bearer_token.empty()) {
        if (public_error) {
            *public_error = "unauthorized";
        }
        return AuthError::Unauthorized;
    }
    const std::string token_hash = hash_bearer_token(bearer_token);
    std::string db_error;
    if (!database_.revoke_session_by_token_hash(token_hash, &db_error)) {
        if (public_error) {
            *public_error = "logout failed";
        }
        return AuthError::Unavailable;
    }
    return AuthError::Ok;
}

std::optional<AuthenticatedPrincipal> AuthService::authenticate_bearer(std::string_view bearer_token,
                                                                       std::string* public_error) const {
    if (bearer_token.empty()) {
        if (public_error) {
            *public_error = "unauthorized";
        }
        return std::nullopt;
    }
    const std::string token_hash = hash_bearer_token(bearer_token);
    std::string db_error;
    const std::optional<AuthSessionRecord> session = database_.find_active_session_by_token_hash(token_hash, &db_error);
    if (!session) {
        if (public_error) {
            *public_error = "unauthorized";
        }
        return std::nullopt;
    }
    AuthenticatedPrincipal principal;
    principal.user_id = session->user_id;
    principal.username = session->username;
    return principal;
}

std::string AuthService::hash_bearer_token(std::string_view bearer_token) const {
    const std::vector<std::uint8_t> digest = pscript::crypto_digest(
        pscript::CryptoHashAlgorithm::Sha256, reinterpret_cast<const std::uint8_t*>(bearer_token.data()),
        bearer_token.size());
    return bytes_to_hex_lower(digest);
}

std::optional<std::string> AuthService::issue_session(const UserRecord& user, AuthTokenResponse* response,
                                                      std::string* public_error) {
    const std::string token = generate_bearer_token();
    const std::string token_hash = hash_bearer_token(token);
    const std::string expires_at = expires_at_from_ttl(settings_.session_ttl_seconds);
    std::string session_id;
    std::string db_error;
    if (!database_.create_auth_session(user.id, token_hash, expires_at, &session_id, &db_error)) {
        if (public_error) {
            *public_error = "failed to create session";
        }
        return std::nullopt;
    }
    if (response) {
        response->username = user.username;
        response->token = token;
        response->expires_at = expires_at;
    }
    return token;
}

}  // namespace pscript::registry
