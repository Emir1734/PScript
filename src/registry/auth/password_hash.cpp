#include "registry/auth/password_hash.h"

#include "runtime/crypto_ops.h"

#include <cctype>

#if defined(PSCRIPT_HAS_LIBARGON2)
#include <argon2.h>
#endif

namespace pscript::registry {
namespace {

constexpr const char* kDisabledMarker = "$pscript$disabled$";

}  // namespace

bool validate_password_policy(std::string_view password, std::string* error) {
    if (password.size() < PasswordPolicy::kMinLength || password.size() > PasswordPolicy::kMaxLength) {
        if (error) {
            *error = "password must be between 12 and 128 characters";
        }
        return false;
    }
    for (unsigned char ch : password) {
        if (ch < 0x20 || ch == 0x7f) {
            if (error) {
                *error = "password contains invalid characters";
            }
            return false;
        }
    }
    return true;
}

std::optional<std::string> hash_password_argon2id(std::string_view password, const pscript::CryptoKdfArgon2Params& params,
                                                  std::string* error) {
    if (!pscript::crypto_kdf_argon2id_available()) {
        if (error) {
            *error = "password hashing is not available in this build";
        }
        return std::nullopt;
    }
    std::string policy_error;
    if (!validate_password_policy(password, &policy_error)) {
        if (error) {
            *error = policy_error;
        }
        return std::nullopt;
    }
#if defined(PSCRIPT_HAS_LIBARGON2)
    constexpr std::size_t kEncodedSize = 128;
    constexpr std::size_t kHashLength = 32;
    constexpr std::size_t kSaltLength = 16;
    const std::vector<std::uint8_t> salt = pscript::crypto_random_bytes(kSaltLength);
    std::string encoded(kEncodedSize, '\0');
    const int rc = argon2id_hash_encoded(params.iterations, params.memory_kib, params.parallelism, password.data(),
                                         password.size(), salt.data(), salt.size(), kHashLength, encoded.data(),
                                         encoded.size());
    if (rc != ARGON2_OK) {
        if (error) {
            *error = "password hashing failed";
        }
        return std::nullopt;
    }
    const std::size_t len = encoded.find('\0');
    if (len != std::string::npos) {
        encoded.resize(len);
    }
    return encoded;
#else
    const std::vector<std::uint8_t> salt = pscript::crypto_random_bytes(16);
    constexpr std::size_t kHashLength = 32;
    const std::vector<std::uint8_t> hash =
        pscript::crypto_argon2id(reinterpret_cast<const std::uint8_t*>(password.data()), password.size(), salt.data(),
                               salt.size(), params, kHashLength);
    std::ostringstream out;
    out << "$argon2id$v=19$m=" << params.memory_kib << ",t=" << params.iterations << ",p=" << params.parallelism << '$';
    // OpenSSL path keeps legacy encoder in password_hash - minimal for non-libargon2
    if (error) {
        *error = "password hashing requires bundled libargon2";
    }
    return std::nullopt;
#endif
}

bool verify_password_argon2id(std::string_view password, std::string_view encoded_hash) {
    if (is_disabled_password_hash(encoded_hash)) {
        return false;
    }
#if defined(PSCRIPT_HAS_LIBARGON2)
    if (encoded_hash.rfind("$argon2id$", 0) != 0) {
        return false;
    }
    return argon2id_verify(encoded_hash.data(), password.data(), password.size()) == ARGON2_OK;
#else
    if (encoded_hash.rfind("$argon2id$", 0) != 0) {
        return false;
    }
    // Fallback verification via raw argon2 when only OpenSSL path exists (legacy tests).
    return false;
#endif
}

bool is_disabled_password_hash(std::string_view encoded_hash) {
    return encoded_hash == kDisabledMarker;
}

}  // namespace pscript::registry
