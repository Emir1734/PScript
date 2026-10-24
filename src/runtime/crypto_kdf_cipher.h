#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pscript {

struct CryptoKdfArgon2Params {
    std::uint32_t memory_kib = 65536;
    std::uint32_t iterations = 2;
    std::uint32_t parallelism = 1;
};

struct CryptoKdfScryptParams {
    std::uint32_t n = 16384;
    std::uint32_t r = 8;
    std::uint32_t p = 1;
};

struct CryptoAeadResult {
    std::vector<std::uint8_t> ciphertext;
    std::vector<std::uint8_t> tag;
};

bool crypto_kdf_argon2id_available();
bool crypto_kdf_scrypt_available();
bool crypto_kdf_pbkdf2_sha256_available();
bool crypto_aead_aes_gcm_available();
bool crypto_aead_chacha20_poly1305_available();

std::vector<std::uint8_t> crypto_pbkdf2_sha256(const std::uint8_t* password, std::size_t password_length,
                                               const std::uint8_t* salt, std::size_t salt_length,
                                               std::uint32_t iterations, std::size_t output_length);

std::vector<std::uint8_t> crypto_scrypt(const std::uint8_t* password, std::size_t password_length,
                                        const std::uint8_t* salt, std::size_t salt_length,
                                        const CryptoKdfScryptParams& params, std::size_t output_length);

std::vector<std::uint8_t> crypto_argon2id(const std::uint8_t* password, std::size_t password_length,
                                           const std::uint8_t* salt, std::size_t salt_length,
                                           const CryptoKdfArgon2Params& params, std::size_t output_length);

CryptoAeadResult crypto_encrypt_aes_gcm(const std::uint8_t* key, std::size_t key_length, const std::uint8_t* nonce,
                                        std::size_t nonce_length, const std::uint8_t* plaintext,
                                        std::size_t plaintext_length, const std::uint8_t* aad, std::size_t aad_length);

std::vector<std::uint8_t> crypto_decrypt_aes_gcm(const std::uint8_t* key, std::size_t key_length,
                                                  const std::uint8_t* nonce, std::size_t nonce_length,
                                                  const std::uint8_t* ciphertext, std::size_t ciphertext_length,
                                                  const std::uint8_t* tag, std::size_t tag_length,
                                                  const std::uint8_t* aad, std::size_t aad_length);

CryptoAeadResult crypto_encrypt_chacha20_poly1305(const std::uint8_t* key, std::size_t key_length,
                                                  const std::uint8_t* nonce, std::size_t nonce_length,
                                                  const std::uint8_t* plaintext, std::size_t plaintext_length,
                                                  const std::uint8_t* aad, std::size_t aad_length);

std::vector<std::uint8_t> crypto_decrypt_chacha20_poly1305(const std::uint8_t* key, std::size_t key_length,
                                                            const std::uint8_t* nonce, std::size_t nonce_length,
                                                            const std::uint8_t* ciphertext,
                                                            std::size_t ciphertext_length, const std::uint8_t* tag,
                                                            std::size_t tag_length, const std::uint8_t* aad,
                                                            std::size_t aad_length);

void crypto_secure_clear(void* data, std::size_t length);

}  // namespace pscript
