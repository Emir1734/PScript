#pragma once

#include "runtime/crypto_kdf_cipher.h"

#include <optional>
#include <string>
#include <string_view>

namespace pscript::registry {

struct PasswordPolicy {
    static constexpr std::size_t kMinLength = 12;
    static constexpr std::size_t kMaxLength = 128;
};

bool validate_password_policy(std::string_view password, std::string* error = nullptr);

std::optional<std::string> hash_password_argon2id(std::string_view password, const pscript::CryptoKdfArgon2Params& params,
                                                  std::string* error = nullptr);

bool verify_password_argon2id(std::string_view password, std::string_view encoded_hash);

bool is_disabled_password_hash(std::string_view encoded_hash);

}  // namespace pscript::registry
