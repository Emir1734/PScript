#include "runtime/crypto_kdf_cipher.h"

#include "runtime/crypto_ops.h"
#include "runtime/runtime.h"

#include <cstring>
#include <limits>

#if defined(PSCRIPT_HAS_OPENSSL)
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/opensslv.h>
#include <openssl/params.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

#if defined(PSCRIPT_HAS_LIBARGON2)
#include <argon2.h>
#endif

namespace pscript {
namespace {

constexpr std::size_t kMaxKdfOutputLength = 1024 * 1024;
constexpr std::uint32_t kMaxPbkdf2Iterations = 10'000'000;
constexpr std::size_t kAesGcmNonceLength = 12;
constexpr std::size_t kAesGcmTagLength = 16;
constexpr std::size_t kChaChaKeyLength = 32;
constexpr std::size_t kChaChaNonceLength = 12;
constexpr std::size_t kChaChaTagLength = 16;

[[noreturn]] void crypto_fail(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Runtime);
}

[[noreturn]] void crypto_auth_fail() {
    throw RuntimeError("authentication failed", RuntimeErrorCategory::Value);
}

void require_output_length(std::size_t length) {
    if (length == 0 || length > kMaxKdfOutputLength) {
        crypto_fail("KDF output length must be between 1 and " + std::to_string(kMaxKdfOutputLength));
    }
}

void require_salt(const std::uint8_t* salt, std::size_t salt_length) {
    if (salt == nullptr || salt_length == 0 || salt_length > 1024) {
        crypto_fail("salt must be non-empty and at most 1024 bytes");
    }
}

void require_pbkdf2_iterations(std::uint32_t iterations) {
    if (iterations == 0 || iterations > kMaxPbkdf2Iterations) {
        crypto_fail("PBKDF2 iterations must be between 1 and " + std::to_string(kMaxPbkdf2Iterations));
    }
}

void validate_aes_gcm_key(const std::uint8_t* key, std::size_t key_length) {
    if (key_length != 16 && key_length != 24 && key_length != 32) {
        crypto_fail("AES-GCM key must be 16, 24, or 32 bytes");
    }
}

void validate_aes_gcm_nonce(const std::uint8_t* nonce, std::size_t nonce_length) {
    if (nonce_length != kAesGcmNonceLength) {
        crypto_fail("AES-GCM nonce must be exactly 12 bytes");
    }
}

void validate_aes_gcm_tag(std::size_t tag_length) {
    if (tag_length != kAesGcmTagLength) {
        crypto_fail("AES-GCM tag must be exactly 16 bytes");
    }
}

void validate_chacha_key(const std::uint8_t* key, std::size_t key_length) {
    if (key_length != kChaChaKeyLength) {
        crypto_fail("ChaCha20-Poly1305 key must be exactly 32 bytes");
    }
}

void validate_chacha_nonce(const std::uint8_t* nonce, std::size_t nonce_length) {
    if (nonce_length != kChaChaNonceLength) {
        crypto_fail("ChaCha20-Poly1305 nonce must be exactly 12 bytes");
    }
}

void validate_chacha_tag(std::size_t tag_length) {
    if (tag_length != kChaChaTagLength) {
        crypto_fail("ChaCha20-Poly1305 tag must be exactly 16 bytes");
    }
}

#if defined(PSCRIPT_HAS_OPENSSL)

bool openssl_has_argon2id() {
#if OPENSSL_VERSION_NUMBER >= 0x30200000L
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (kdf == nullptr) {
        return false;
    }
    EVP_KDF_free(kdf);
    return true;
#else
    return false;
#endif
}

std::vector<std::uint8_t> pbkdf2_openssl(const std::uint8_t* password, std::size_t password_length,
                                          const std::uint8_t* salt, std::size_t salt_length, std::uint32_t iterations,
                                          std::size_t output_length) {
    std::vector<std::uint8_t> out(output_length);
    if (PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password), static_cast<int>(password_length), salt,
                          static_cast<int>(salt_length), static_cast<int>(iterations), EVP_sha256(),
                          static_cast<int>(output_length), out.data()) != 1) {
        crypto_fail("PBKDF2 operation failed");
    }
    return out;
}

std::vector<std::uint8_t> scrypt_openssl(const std::uint8_t* password, std::size_t password_length,
                                         const std::uint8_t* salt, std::size_t salt_length,
                                         const CryptoKdfScryptParams& params, std::size_t output_length) {
    std::vector<std::uint8_t> out(output_length);
    const uint64_t max_mem = 1024ULL * 1024ULL * 1024ULL;
    if (EVP_PBE_scrypt(reinterpret_cast<const char*>(password), password_length, salt, salt_length,
                       static_cast<uint64_t>(params.n), params.r, params.p, max_mem, out.data(), output_length) != 1) {
        crypto_fail("scrypt operation failed");
    }
    return out;
}

#if !defined(PSCRIPT_HAS_LIBARGON2)
std::vector<std::uint8_t> argon2id_openssl(const std::uint8_t* password, std::size_t password_length,
                                            const std::uint8_t* salt, std::size_t salt_length,
                                            const CryptoKdfArgon2Params& params, std::size_t output_length) {
#if OPENSSL_VERSION_NUMBER >= 0x30200000L && defined(OSSL_KDF_PARAM_SECRET)
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "ARGON2ID", nullptr);
    if (kdf == nullptr) {
        crypto_fail("Argon2id is not available in this OpenSSL build");
    }
    EVP_KDF_CTX* ctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (ctx == nullptr) {
        crypto_fail("failed to allocate Argon2id context");
    }
    std::uint32_t iterations = params.iterations;
    std::uint32_t parallelism = params.parallelism;
    std::uint32_t memory_kib = params.memory_kib;
    const OSSL_PARAM param[] = {
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SECRET, const_cast<std::uint8_t*>(password), password_length),
        OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, const_cast<std::uint8_t*>(salt), salt_length),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_ITER, &iterations),
        OSSL_PARAM_construct_uint32(OSSL_KDF_PARAM_THREADS, &parallelism),
        OSSL_PARAM_construct_uint32("memcost", &memory_kib),
        OSSL_PARAM_construct_end(),
    };
    std::vector<std::uint8_t> out(output_length);
    if (EVP_KDF_derive(ctx, out.data(), out.size(), param) != 1) {
        EVP_KDF_CTX_free(ctx);
        crypto_fail("Argon2id operation failed");
    }
    EVP_KDF_CTX_free(ctx);
    return out;
#else
    crypto_fail("Argon2id requires OpenSSL 3.2 or newer");
#endif
}
#endif  // !PSCRIPT_HAS_LIBARGON2

const EVP_CIPHER* aes_gcm_cipher_for_key_length(std::size_t key_length) {
    switch (key_length) {
        case 16:
            return EVP_aes_128_gcm();
        case 24:
            return EVP_aes_192_gcm();
        case 32:
            return EVP_aes_256_gcm();
        default:
            crypto_fail("invalid AES-GCM key length");
    }
}

CryptoAeadResult aes_gcm_encrypt_openssl(const std::uint8_t* key, std::size_t key_length, const std::uint8_t* nonce,
                                         std::size_t nonce_length, const std::uint8_t* plaintext,
                                         std::size_t plaintext_length, const std::uint8_t* aad, std::size_t aad_length) {
    validate_aes_gcm_key(key, key_length);
    validate_aes_gcm_nonce(nonce, nonce_length);
    const EVP_CIPHER* cipher = aes_gcm_cipher_for_key_length(key_length);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        crypto_fail("failed to allocate cipher context");
    }
    CryptoAeadResult result;
    result.ciphertext.resize(plaintext_length);
    result.tag.resize(kAesGcmTagLength);
    int out_len = 0;
    int final_len = 0;
    if (EVP_EncryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_length), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_fail("AES-GCM encrypt init failed");
    }
    if (aad_length > 0) {
        if (EVP_EncryptUpdate(ctx, nullptr, &out_len, aad, static_cast<int>(aad_length)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            crypto_fail("AES-GCM AAD update failed");
        }
    }
    if (plaintext_length > 0) {
        if (EVP_EncryptUpdate(ctx, result.ciphertext.data(), &out_len, plaintext,
                              static_cast<int>(plaintext_length)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            crypto_fail("AES-GCM encrypt failed");
        }
    }
    if (EVP_EncryptFinal_ex(ctx, result.ciphertext.data() + out_len, &final_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_fail("AES-GCM encrypt final failed");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, static_cast<int>(result.tag.size()), result.tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_fail("AES-GCM tag export failed");
    }
    EVP_CIPHER_CTX_free(ctx);
    return result;
}

std::vector<std::uint8_t> aes_gcm_decrypt_openssl(const std::uint8_t* key, std::size_t key_length,
                                                   const std::uint8_t* nonce, std::size_t nonce_length,
                                                   const std::uint8_t* ciphertext, std::size_t ciphertext_length,
                                                   const std::uint8_t* tag, std::size_t tag_length,
                                                   const std::uint8_t* aad, std::size_t aad_length) {
    validate_aes_gcm_key(key, key_length);
    validate_aes_gcm_nonce(nonce, nonce_length);
    validate_aes_gcm_tag(tag_length);
    const EVP_CIPHER* cipher = aes_gcm_cipher_for_key_length(key_length);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        crypto_fail("failed to allocate cipher context");
    }
    std::vector<std::uint8_t> plaintext(ciphertext_length);
    int out_len = 0;
    int final_len = 0;
    if (EVP_DecryptInit_ex(ctx, cipher, nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(nonce_length), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_fail("AES-GCM decrypt init failed");
    }
    if (aad_length > 0) {
        if (EVP_DecryptUpdate(ctx, nullptr, &out_len, aad, static_cast<int>(aad_length)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            crypto_fail("AES-GCM AAD update failed");
        }
    }
    if (ciphertext_length > 0) {
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, ciphertext, static_cast<int>(ciphertext_length)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            crypto_auth_fail();
        }
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag_length),
                            const_cast<std::uint8_t*>(tag)) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_auth_fail();
    }
    const int ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1) {
        crypto_auth_fail();
    }
    plaintext.resize(static_cast<std::size_t>(out_len + final_len));
    return plaintext;
}

CryptoAeadResult chacha_encrypt_openssl(const std::uint8_t* key, std::size_t key_length, const std::uint8_t* nonce,
                                        std::size_t nonce_length, const std::uint8_t* plaintext,
                                        std::size_t plaintext_length, const std::uint8_t* aad, std::size_t aad_length) {
    validate_chacha_key(key, key_length);
    validate_chacha_nonce(nonce, nonce_length);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        crypto_fail("failed to allocate cipher context");
    }
    CryptoAeadResult result;
    result.ciphertext.resize(plaintext_length);
    result.tag.resize(kChaChaTagLength);
    int out_len = 0;
    if (EVP_EncryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce_length), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_fail("ChaCha20-Poly1305 encrypt init failed");
    }
    if (aad_length > 0) {
        if (EVP_EncryptUpdate(ctx, nullptr, &out_len, aad, static_cast<int>(aad_length)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            crypto_fail("ChaCha20-Poly1305 AAD update failed");
        }
    }
    if (plaintext_length > 0) {
        if (EVP_EncryptUpdate(ctx, result.ciphertext.data(), &out_len, plaintext,
                              static_cast<int>(plaintext_length)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            crypto_fail("ChaCha20-Poly1305 encrypt failed");
        }
    }
    if (EVP_EncryptFinal_ex(ctx, result.ciphertext.data() + out_len, &out_len) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_fail("ChaCha20-Poly1305 encrypt final failed");
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, static_cast<int>(result.tag.size()), result.tag.data()) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_fail("ChaCha20-Poly1305 tag export failed");
    }
    EVP_CIPHER_CTX_free(ctx);
    return result;
}

std::vector<std::uint8_t> chacha_decrypt_openssl(const std::uint8_t* key, std::size_t key_length,
                                                  const std::uint8_t* nonce, std::size_t nonce_length,
                                                  const std::uint8_t* ciphertext, std::size_t ciphertext_length,
                                                  const std::uint8_t* tag, std::size_t tag_length,
                                                  const std::uint8_t* aad, std::size_t aad_length) {
    validate_chacha_key(key, key_length);
    validate_chacha_nonce(nonce, nonce_length);
    validate_chacha_tag(tag_length);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        crypto_fail("failed to allocate cipher context");
    }
    std::vector<std::uint8_t> plaintext(ciphertext_length);
    int out_len = 0;
    int final_len = 0;
    if (EVP_DecryptInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, static_cast<int>(nonce_length), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_fail("ChaCha20-Poly1305 decrypt init failed");
    }
    if (aad_length > 0) {
        if (EVP_DecryptUpdate(ctx, nullptr, &out_len, aad, static_cast<int>(aad_length)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            crypto_fail("ChaCha20-Poly1305 AAD update failed");
        }
    }
    if (ciphertext_length > 0) {
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &out_len, ciphertext, static_cast<int>(ciphertext_length)) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            crypto_auth_fail();
        }
    }
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, static_cast<int>(tag_length),
                            const_cast<std::uint8_t*>(tag)) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        crypto_auth_fail();
    }
    const int ok = EVP_DecryptFinal_ex(ctx, plaintext.data() + out_len, &final_len);
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1) {
        crypto_auth_fail();
    }
    plaintext.resize(static_cast<std::size_t>(out_len + final_len));
    return plaintext;
}

#elif defined(_WIN32)

std::vector<std::uint8_t> pbkdf2_hmac_sha256(const std::uint8_t* password, std::size_t password_length,
                                              const std::uint8_t* salt, std::size_t salt_length, std::uint32_t iterations,
                                              std::size_t output_length) {
    std::vector<std::uint8_t> derived;
    derived.reserve(output_length);
    std::vector<std::uint8_t> block_input(salt_length + 4);
    std::memcpy(block_input.data(), salt, salt_length);
    for (std::uint32_t block_index = 1; derived.size() < output_length; ++block_index) {
        block_input[salt_length] = static_cast<std::uint8_t>((block_index >> 24) & 0xFF);
        block_input[salt_length + 1] = static_cast<std::uint8_t>((block_index >> 16) & 0xFF);
        block_input[salt_length + 2] = static_cast<std::uint8_t>((block_index >> 8) & 0xFF);
        block_input[salt_length + 3] = static_cast<std::uint8_t>(block_index & 0xFF);
        std::vector<std::uint8_t> u = crypto_hmac(CryptoHmacAlgorithm::Sha256, password, password_length,
                                                  block_input.data(), block_input.size());
        std::vector<std::uint8_t> block = u;
        for (std::uint32_t iter = 1; iter < iterations; ++iter) {
            u = crypto_hmac(CryptoHmacAlgorithm::Sha256, password, password_length, u.data(), u.size());
            for (std::size_t i = 0; i < block.size(); ++i) {
                block[i] ^= u[i];
            }
        }
        const std::size_t need = output_length - derived.size();
        derived.insert(derived.end(), block.begin(), block.begin() + std::min(need, block.size()));
    }
    return derived;
}

struct BcryptAeadKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    std::vector<std::uint8_t> object;

    ~BcryptAeadKey() {
        if (key != nullptr) {
            BCryptDestroyKey(key);
        }
        if (alg != nullptr) {
            BCryptCloseAlgorithmProvider(alg, 0);
        }
    }
};

BcryptAeadKey open_aes_gcm_key(const std::uint8_t* key, std::size_t key_length) {
    validate_aes_gcm_key(key, key_length);
    BcryptAeadKey state;
    if (BCryptOpenAlgorithmProvider(&state.alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) {
        crypto_fail("failed to open AES provider");
    }
    if (BCryptSetProperty(state.alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                          static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0) != 0) {
        crypto_fail("failed to set AES-GCM mode");
    }
    DWORD object_length = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(state.alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
                          &cb, 0) != 0) {
        crypto_fail("failed to query AES key object length");
    }
    state.object.resize(object_length);
    if (BCryptGenerateSymmetricKey(state.alg, &state.key, state.object.data(), static_cast<ULONG>(state.object.size()),
                                   const_cast<PUCHAR>(key), static_cast<ULONG>(key_length), 0) != 0) {
        crypto_fail("failed to import AES-GCM key");
    }
    return state;
}

CryptoAeadResult aes_gcm_encrypt_bcrypt(const std::uint8_t* key, std::size_t key_length, const std::uint8_t* nonce,
                                        std::size_t nonce_length, const std::uint8_t* plaintext,
                                        std::size_t plaintext_length, const std::uint8_t* aad, std::size_t aad_length) {
    validate_aes_gcm_nonce(nonce, nonce_length);
    BcryptAeadKey state = open_aes_gcm_key(key, key_length);
    CryptoAeadResult result;
    result.ciphertext.resize(plaintext_length);
    result.tag.resize(kAesGcmTagLength);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(nonce);
    auth_info.cbNonce = static_cast<ULONG>(nonce_length);
    auth_info.pbTag = result.tag.data();
    auth_info.cbTag = static_cast<ULONG>(result.tag.size());
    if (aad_length > 0) {
        auth_info.pbAuthData = const_cast<PUCHAR>(aad);
        auth_info.cbAuthData = static_cast<ULONG>(aad_length);
    }
    ULONG produced = 0;
    const NTSTATUS status = BCryptEncrypt(state.key, const_cast<PUCHAR>(plaintext), static_cast<ULONG>(plaintext_length),
                                          &auth_info, nullptr, 0, result.ciphertext.data(),
                                          static_cast<ULONG>(result.ciphertext.size()), &produced, 0);
    if (status != 0) {
        crypto_fail("AES-GCM encrypt failed");
    }
    result.ciphertext.resize(produced);
    return result;
}

std::vector<std::uint8_t> aes_gcm_decrypt_bcrypt(const std::uint8_t* key, std::size_t key_length,
                                                  const std::uint8_t* nonce, std::size_t nonce_length,
                                                  const std::uint8_t* ciphertext, std::size_t ciphertext_length,
                                                  const std::uint8_t* tag, std::size_t tag_length,
                                                  const std::uint8_t* aad, std::size_t aad_length) {
    validate_aes_gcm_nonce(nonce, nonce_length);
    validate_aes_gcm_tag(tag_length);
    BcryptAeadKey state = open_aes_gcm_key(key, key_length);
    std::vector<std::uint8_t> plaintext(ciphertext_length);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(nonce);
    auth_info.cbNonce = static_cast<ULONG>(nonce_length);
    auth_info.pbTag = const_cast<PUCHAR>(tag);
    auth_info.cbTag = static_cast<ULONG>(tag_length);
    if (aad_length > 0) {
        auth_info.pbAuthData = const_cast<PUCHAR>(aad);
        auth_info.cbAuthData = static_cast<ULONG>(aad_length);
    }
    ULONG produced = 0;
    const NTSTATUS status =
        BCryptDecrypt(state.key, const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(ciphertext_length), &auth_info,
                      nullptr, 0, plaintext.data(), static_cast<ULONG>(plaintext.size()), &produced, 0);
    if (status != 0) {
        crypto_auth_fail();
    }
    plaintext.resize(produced);
    return plaintext;
}

BcryptAeadKey open_chacha_key(const std::uint8_t* key, std::size_t key_length) {
    validate_chacha_key(key, key_length);
    BcryptAeadKey state;
    if (BCryptOpenAlgorithmProvider(&state.alg, BCRYPT_CHACHA20_POLY1305_ALGORITHM, nullptr, 0) != 0) {
        crypto_fail("failed to open ChaCha20-Poly1305 provider");
    }
    DWORD object_length = 0;
    DWORD cb = 0;
    if (BCryptGetProperty(state.alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length),
                          &cb, 0) != 0) {
        crypto_fail("failed to query ChaCha key object length");
    }
    state.object.resize(object_length);
    if (BCryptGenerateSymmetricKey(state.alg, &state.key, state.object.data(), static_cast<ULONG>(state.object.size()),
                                   const_cast<PUCHAR>(key), static_cast<ULONG>(key_length), 0) != 0) {
        crypto_fail("failed to import ChaCha20-Poly1305 key");
    }
    return state;
}

bool bcrypt_chacha_available() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_CHACHA20_POLY1305_ALGORITHM, nullptr, 0) != 0) {
        return false;
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return true;
}

CryptoAeadResult chacha_encrypt_bcrypt(const std::uint8_t* key, std::size_t key_length, const std::uint8_t* nonce,
                                       std::size_t nonce_length, const std::uint8_t* plaintext,
                                       std::size_t plaintext_length, const std::uint8_t* aad, std::size_t aad_length) {
    if (!bcrypt_chacha_available()) {
        crypto_fail("ChaCha20-Poly1305 is not available on this platform");
    }
    validate_chacha_nonce(nonce, nonce_length);
    BcryptAeadKey state = open_chacha_key(key, key_length);
    CryptoAeadResult result;
    result.ciphertext.resize(plaintext_length);
    result.tag.resize(kChaChaTagLength);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(nonce);
    auth_info.cbNonce = static_cast<ULONG>(nonce_length);
    auth_info.pbTag = result.tag.data();
    auth_info.cbTag = static_cast<ULONG>(result.tag.size());
    if (aad_length > 0) {
        auth_info.pbAuthData = const_cast<PUCHAR>(aad);
        auth_info.cbAuthData = static_cast<ULONG>(aad_length);
    }
    ULONG produced = 0;
    const NTSTATUS status = BCryptEncrypt(state.key, const_cast<PUCHAR>(plaintext), static_cast<ULONG>(plaintext_length),
                                          &auth_info, nullptr, 0, result.ciphertext.data(),
                                          static_cast<ULONG>(result.ciphertext.size()), &produced, 0);
    if (status != 0) {
        crypto_fail("ChaCha20-Poly1305 encrypt failed");
    }
    result.ciphertext.resize(produced);
    return result;
}

std::vector<std::uint8_t> chacha_decrypt_bcrypt(const std::uint8_t* key, std::size_t key_length,
                                                 const std::uint8_t* nonce, std::size_t nonce_length,
                                                 const std::uint8_t* ciphertext, std::size_t ciphertext_length,
                                                 const std::uint8_t* tag, std::size_t tag_length,
                                                 const std::uint8_t* aad, std::size_t aad_length) {
    if (!bcrypt_chacha_available()) {
        crypto_fail("ChaCha20-Poly1305 is not available on this platform");
    }
    validate_chacha_nonce(nonce, nonce_length);
    validate_chacha_tag(tag_length);
    BcryptAeadKey state = open_chacha_key(key, key_length);
    std::vector<std::uint8_t> plaintext(ciphertext_length);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info;
    BCRYPT_INIT_AUTH_MODE_INFO(auth_info);
    auth_info.pbNonce = const_cast<PUCHAR>(nonce);
    auth_info.cbNonce = static_cast<ULONG>(nonce_length);
    auth_info.pbTag = const_cast<PUCHAR>(tag);
    auth_info.cbTag = static_cast<ULONG>(tag_length);
    if (aad_length > 0) {
        auth_info.pbAuthData = const_cast<PUCHAR>(aad);
        auth_info.cbAuthData = static_cast<ULONG>(aad_length);
    }
    ULONG produced = 0;
    const NTSTATUS status =
        BCryptDecrypt(state.key, const_cast<PUCHAR>(ciphertext), static_cast<ULONG>(ciphertext_length), &auth_info,
                      nullptr, 0, plaintext.data(), static_cast<ULONG>(plaintext.size()), &produced, 0);
    if (status != 0) {
        crypto_auth_fail();
    }
    plaintext.resize(produced);
    return plaintext;
}

#endif

void validate_scrypt_params(const CryptoKdfScryptParams& params) {
    if (params.n < 2 || params.n > (1u << 20) || (params.n & (params.n - 1)) != 0) {
        crypto_fail("scrypt N must be a power of two between 2 and 1048576");
    }
    if (params.r == 0 || params.r > 256) {
        crypto_fail("scrypt r must be between 1 and 256");
    }
    if (params.p == 0 || params.p > 256) {
        crypto_fail("scrypt p must be between 1 and 256");
    }
    const std::uint64_t memory_need = static_cast<std::uint64_t>(params.n) * static_cast<std::uint64_t>(params.r) * 128ULL;
    if (memory_need > 1024ULL * 1024ULL * 1024ULL) {
        crypto_fail("scrypt parameters exceed memory limit");
    }
}

void validate_argon2_params(const CryptoKdfArgon2Params& params) {
    if (params.memory_kib < 8 || params.memory_kib > 512 * 1024) {
        crypto_fail("Argon2id memory_kib must be between 8 and 524288");
    }
    if (params.iterations == 0 || params.iterations > 1000) {
        crypto_fail("Argon2id iterations must be between 1 and 1000");
    }
    if (params.parallelism == 0 || params.parallelism > 64) {
        crypto_fail("Argon2id parallelism must be between 1 and 64");
    }
}

}  // namespace

void crypto_secure_clear(void* data, std::size_t length) {
    if (data == nullptr || length == 0) {
        return;
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    OPENSSL_cleanse(data, length);
#elif defined(_WIN32)
    SecureZeroMemory(data, length);
#else
    volatile std::uint8_t* p = static_cast<volatile std::uint8_t*>(data);
    while (length-- > 0) {
        *p++ = 0;
    }
#endif
}

bool crypto_kdf_argon2id_available() {
#if defined(PSCRIPT_HAS_LIBARGON2)
    return true;
#elif defined(PSCRIPT_HAS_OPENSSL)
    return openssl_has_argon2id();
#else
    return false;
#endif
}

bool crypto_kdf_scrypt_available() {
#if defined(PSCRIPT_HAS_OPENSSL)
    return true;
#else
    return false;
#endif
}

bool crypto_kdf_pbkdf2_sha256_available() {
#if defined(PSCRIPT_HAS_OPENSSL) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool crypto_aead_aes_gcm_available() {
#if defined(PSCRIPT_HAS_OPENSSL) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool crypto_aead_chacha20_poly1305_available() {
#if defined(PSCRIPT_HAS_OPENSSL)
    return true;
#elif defined(_WIN32)
    return bcrypt_chacha_available();
#else
    return false;
#endif
}

std::vector<std::uint8_t> crypto_pbkdf2_sha256(const std::uint8_t* password, std::size_t password_length,
                                               const std::uint8_t* salt, std::size_t salt_length,
                                               std::uint32_t iterations, std::size_t output_length) {
    if (!crypto_kdf_pbkdf2_sha256_available()) {
        crypto_fail("PBKDF2-SHA256 is not available in this build");
    }
    require_salt(salt, salt_length);
    require_pbkdf2_iterations(iterations);
    require_output_length(output_length);
#if defined(PSCRIPT_HAS_OPENSSL)
    return pbkdf2_openssl(password, password_length, salt, salt_length, iterations, output_length);
#elif defined(_WIN32)
    return pbkdf2_hmac_sha256(password, password_length, salt, salt_length, iterations, output_length);
#else
    crypto_fail("PBKDF2-SHA256 is not available in this build");
#endif
}

std::vector<std::uint8_t> crypto_scrypt(const std::uint8_t* password, std::size_t password_length,
                                        const std::uint8_t* salt, std::size_t salt_length,
                                        const CryptoKdfScryptParams& params, std::size_t output_length) {
    if (!crypto_kdf_scrypt_available()) {
        crypto_fail("scrypt requires an OpenSSL-enabled build");
    }
    require_salt(salt, salt_length);
    require_output_length(output_length);
    validate_scrypt_params(params);
#if defined(PSCRIPT_HAS_OPENSSL)
    return scrypt_openssl(password, password_length, salt, salt_length, params, output_length);
#else
    crypto_fail("scrypt requires an OpenSSL-enabled build");
#endif
}

std::vector<std::uint8_t> crypto_argon2id(const std::uint8_t* password, std::size_t password_length,
                                           const std::uint8_t* salt, std::size_t salt_length,
                                           const CryptoKdfArgon2Params& params, std::size_t output_length) {
    if (!crypto_kdf_argon2id_available()) {
        crypto_fail("Argon2id is not available in this build");
    }
    require_salt(salt, salt_length);
    require_output_length(output_length);
    validate_argon2_params(params);
#if defined(PSCRIPT_HAS_LIBARGON2)
    std::vector<std::uint8_t> out(output_length);
    if (argon2id_hash_raw(params.iterations, params.memory_kib, params.parallelism, password, password_length, salt,
                          salt_length, out.data(), out.size()) != ARGON2_OK) {
        crypto_fail("Argon2id operation failed");
    }
    return out;
#elif defined(PSCRIPT_HAS_OPENSSL)
    return argon2id_openssl(password, password_length, salt, salt_length, params, output_length);
#else
    crypto_fail("Argon2id is not available in this build");
#endif
}

CryptoAeadResult crypto_encrypt_aes_gcm(const std::uint8_t* key, std::size_t key_length, const std::uint8_t* nonce,
                                        std::size_t nonce_length, const std::uint8_t* plaintext,
                                        std::size_t plaintext_length, const std::uint8_t* aad, std::size_t aad_length) {
    if (!crypto_aead_aes_gcm_available()) {
        crypto_fail("AES-GCM is not available in this build");
    }
    if (aad == nullptr) {
        aad_length = 0;
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    return aes_gcm_encrypt_openssl(key, key_length, nonce, nonce_length, plaintext, plaintext_length, aad, aad_length);
#elif defined(_WIN32)
    return aes_gcm_encrypt_bcrypt(key, key_length, nonce, nonce_length, plaintext, plaintext_length, aad, aad_length);
#else
    crypto_fail("AES-GCM is not available in this build");
#endif
}

std::vector<std::uint8_t> crypto_decrypt_aes_gcm(const std::uint8_t* key, std::size_t key_length,
                                                  const std::uint8_t* nonce, std::size_t nonce_length,
                                                  const std::uint8_t* ciphertext, std::size_t ciphertext_length,
                                                  const std::uint8_t* tag, std::size_t tag_length,
                                                  const std::uint8_t* aad, std::size_t aad_length) {
    if (!crypto_aead_aes_gcm_available()) {
        crypto_fail("AES-GCM is not available in this build");
    }
    if (aad == nullptr) {
        aad_length = 0;
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    return aes_gcm_decrypt_openssl(key, key_length, nonce, nonce_length, ciphertext, ciphertext_length, tag, tag_length,
                                   aad, aad_length);
#elif defined(_WIN32)
    return aes_gcm_decrypt_bcrypt(key, key_length, nonce, nonce_length, ciphertext, ciphertext_length, tag, tag_length,
                                  aad, aad_length);
#else
    crypto_fail("AES-GCM is not available in this build");
#endif
}

CryptoAeadResult crypto_encrypt_chacha20_poly1305(const std::uint8_t* key, std::size_t key_length,
                                                  const std::uint8_t* nonce, std::size_t nonce_length,
                                                  const std::uint8_t* plaintext, std::size_t plaintext_length,
                                                  const std::uint8_t* aad, std::size_t aad_length) {
    if (!crypto_aead_chacha20_poly1305_available()) {
        crypto_fail("ChaCha20-Poly1305 is not available in this build");
    }
    if (aad == nullptr) {
        aad_length = 0;
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    return chacha_encrypt_openssl(key, key_length, nonce, nonce_length, plaintext, plaintext_length, aad, aad_length);
#elif defined(_WIN32)
    return chacha_encrypt_bcrypt(key, key_length, nonce, nonce_length, plaintext, plaintext_length, aad, aad_length);
#else
    crypto_fail("ChaCha20-Poly1305 is not available in this build");
#endif
}

std::vector<std::uint8_t> crypto_decrypt_chacha20_poly1305(const std::uint8_t* key, std::size_t key_length,
                                                            const std::uint8_t* nonce, std::size_t nonce_length,
                                                            const std::uint8_t* ciphertext,
                                                            std::size_t ciphertext_length, const std::uint8_t* tag,
                                                            std::size_t tag_length, const std::uint8_t* aad,
                                                            std::size_t aad_length) {
    if (!crypto_aead_chacha20_poly1305_available()) {
        crypto_fail("ChaCha20-Poly1305 is not available in this build");
    }
    if (aad == nullptr) {
        aad_length = 0;
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    return chacha_decrypt_openssl(key, key_length, nonce, nonce_length, ciphertext, ciphertext_length, tag, tag_length,
                                  aad, aad_length);
#elif defined(_WIN32)
    return chacha_decrypt_bcrypt(key, key_length, nonce, nonce_length, ciphertext, ciphertext_length, tag, tag_length,
                                 aad, aad_length);
#else
    crypto_fail("ChaCha20-Poly1305 is not available in this build");
#endif
}

}  // namespace pscript
