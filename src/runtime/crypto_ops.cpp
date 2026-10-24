#include "runtime/crypto_ops.h"

#include "runtime/runtime.h"

#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <sstream>
#include <stdexcept>

#if defined(PSCRIPT_HAS_OPENSSL)
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#endif

namespace pscript {
namespace {

constexpr std::size_t kMaxRandomBytes = 1 * 1024 * 1024;

[[noreturn]] void crypto_fail(std::string message) {
    throw RuntimeError(std::move(message), RuntimeErrorCategory::Runtime);
}

void require_positive_length(std::size_t length, std::string_view what) {
    if (length > kMaxRandomBytes) {
        crypto_fail(std::string(what) + " length exceeds maximum of " + std::to_string(kMaxRandomBytes));
    }
}

#if defined(PSCRIPT_HAS_OPENSSL)

const EVP_MD* md_for(CryptoHashAlgorithm algorithm) {
    switch (algorithm) {
        case CryptoHashAlgorithm::Sha256:
            return EVP_sha256();
        case CryptoHashAlgorithm::Sha512:
            return EVP_sha512();
        case CryptoHashAlgorithm::Sha3_256:
            return EVP_sha3_256();
        case CryptoHashAlgorithm::Sha3_512:
            return EVP_sha3_512();
        case CryptoHashAlgorithm::Blake2b:
            return EVP_blake2b512();
        case CryptoHashAlgorithm::Blake2s:
            return EVP_blake2s256();
    }
    crypto_fail("unsupported hash algorithm");
}

const EVP_MD* md_for_hmac(CryptoHmacAlgorithm algorithm) {
    switch (algorithm) {
        case CryptoHmacAlgorithm::Sha256:
            return EVP_sha256();
        case CryptoHmacAlgorithm::Sha512:
            return EVP_sha512();
    }
    crypto_fail("unsupported HMAC algorithm");
}

std::vector<std::uint8_t> digest_openssl(CryptoHashAlgorithm algorithm, const std::uint8_t* data, std::size_t length) {
    const EVP_MD* md = md_for(algorithm);
    std::vector<std::uint8_t> out(EVP_MD_size(md));
    unsigned int out_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr) {
        crypto_fail("failed to allocate digest context");
    }
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1 || EVP_DigestUpdate(ctx, data, length) != 1 ||
        EVP_DigestFinal_ex(ctx, out.data(), &out_len) != 1) {
        EVP_MD_CTX_free(ctx);
        crypto_fail("digest operation failed");
    }
    EVP_MD_CTX_free(ctx);
    out.resize(out_len);
    return out;
}

std::vector<std::uint8_t> hmac_openssl(CryptoHmacAlgorithm algorithm, const std::uint8_t* key, std::size_t key_length,
                                       const std::uint8_t* data, std::size_t data_length) {
    const EVP_MD* md = md_for_hmac(algorithm);
    std::vector<std::uint8_t> out(EVP_MD_size(md));
    unsigned int out_len = 0;
    unsigned char* result =
        HMAC(md, key, static_cast<int>(key_length), data, data_length, out.data(), &out_len);
    if (result == nullptr) {
        crypto_fail("HMAC operation failed");
    }
    out.resize(out_len);
    return out;
}

#elif defined(_WIN32)

struct BcryptAlgorithm {
    LPCWSTR name = nullptr;
    DWORD object_length = 0;
    DWORD hash_length = 0;
    BCRYPT_ALG_HANDLE handle = nullptr;
};

BcryptAlgorithm& bcrypt_sha256() {
    static BcryptAlgorithm state;
    if (state.handle == nullptr) {
        state.name = BCRYPT_SHA256_ALGORITHM;
        if (BCryptOpenAlgorithmProvider(&state.handle, state.name, nullptr, 0) != 0) {
            crypto_fail("failed to open SHA-256 provider");
        }
        DWORD cb = 0;
        if (BCryptGetProperty(state.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&state.object_length),
                              sizeof(state.object_length), &cb, 0) != 0 ||
            BCryptGetProperty(state.handle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&state.hash_length),
                              sizeof(state.hash_length), &cb, 0) != 0) {
            crypto_fail("failed to query SHA-256 properties");
        }
    }
    return state;
}

BcryptAlgorithm& bcrypt_sha512() {
    static BcryptAlgorithm state;
    if (state.handle == nullptr) {
        state.name = BCRYPT_SHA512_ALGORITHM;
        if (BCryptOpenAlgorithmProvider(&state.handle, state.name, nullptr, 0) != 0) {
            crypto_fail("failed to open SHA-512 provider");
        }
        DWORD cb = 0;
        if (BCryptGetProperty(state.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&state.object_length),
                              sizeof(state.object_length), &cb, 0) != 0 ||
            BCryptGetProperty(state.handle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&state.hash_length),
                              sizeof(state.hash_length), &cb, 0) != 0) {
            crypto_fail("failed to query SHA-512 properties");
        }
    }
    return state;
}

BcryptAlgorithm& bcrypt_sha3_256() {
    static BcryptAlgorithm state;
    if (state.handle == nullptr) {
        state.name = BCRYPT_SHA3_256_ALGORITHM;
        if (BCryptOpenAlgorithmProvider(&state.handle, state.name, nullptr, 0) != 0) {
            crypto_fail("failed to open SHA3-256 provider");
        }
        DWORD cb = 0;
        if (BCryptGetProperty(state.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&state.object_length),
                              sizeof(state.object_length), &cb, 0) != 0 ||
            BCryptGetProperty(state.handle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&state.hash_length),
                              sizeof(state.hash_length), &cb, 0) != 0) {
            crypto_fail("failed to query SHA3-256 properties");
        }
    }
    return state;
}

BcryptAlgorithm& bcrypt_sha3_512() {
    static BcryptAlgorithm state;
    if (state.handle == nullptr) {
        state.name = BCRYPT_SHA3_512_ALGORITHM;
        if (BCryptOpenAlgorithmProvider(&state.handle, state.name, nullptr, 0) != 0) {
            crypto_fail("failed to open SHA3-512 provider");
        }
        DWORD cb = 0;
        if (BCryptGetProperty(state.handle, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&state.object_length),
                              sizeof(state.object_length), &cb, 0) != 0 ||
            BCryptGetProperty(state.handle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&state.hash_length),
                              sizeof(state.hash_length), &cb, 0) != 0) {
            crypto_fail("failed to query SHA3-512 properties");
        }
    }
    return state;
}

BcryptAlgorithm& bcrypt_for(CryptoHashAlgorithm algorithm) {
    switch (algorithm) {
        case CryptoHashAlgorithm::Sha256:
            return bcrypt_sha256();
        case CryptoHashAlgorithm::Sha512:
            return bcrypt_sha512();
        case CryptoHashAlgorithm::Sha3_256:
            return bcrypt_sha3_256();
        case CryptoHashAlgorithm::Sha3_512:
            return bcrypt_sha3_512();
        case CryptoHashAlgorithm::Blake2b:
        case CryptoHashAlgorithm::Blake2s:
            crypto_fail("BLAKE2 requires an OpenSSL-enabled build");
    }
    crypto_fail("unsupported hash algorithm");
}

std::vector<std::uint8_t> digest_bcrypt(CryptoHashAlgorithm algorithm, const std::uint8_t* data, std::size_t length) {
    BcryptAlgorithm& alg = bcrypt_for(algorithm);
    std::vector<std::uint8_t> object(alg.object_length);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(alg.handle, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0) != 0) {
        crypto_fail("failed to create hash");
    }
    if (BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(length), 0) != 0) {
        BCryptDestroyHash(hash);
        crypto_fail("failed to hash data");
    }
    std::vector<std::uint8_t> out(alg.hash_length);
    if (BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0) != 0) {
        BCryptDestroyHash(hash);
        crypto_fail("failed to finalize hash");
    }
    BCryptDestroyHash(hash);
    return out;
}

std::vector<std::uint8_t> hmac_from_digest(
    const std::function<std::vector<std::uint8_t>(const std::uint8_t*, std::size_t)>& digest, std::size_t block_size,
    const std::uint8_t* key, std::size_t key_length, const std::uint8_t* data, std::size_t data_length) {
    std::vector<std::uint8_t> key_block(block_size, 0);
    if (key_length > block_size) {
        const std::vector<std::uint8_t> hashed_key = digest(key, key_length);
        if (hashed_key.size() > block_size) {
            crypto_fail("HMAC key digest length exceeds block size");
        }
        std::memcpy(key_block.data(), hashed_key.data(), hashed_key.size());
    } else if (key_length > 0) {
        std::memcpy(key_block.data(), key, key_length);
    }
    std::vector<std::uint8_t> ipad(block_size);
    std::vector<std::uint8_t> opad(block_size);
    for (std::size_t i = 0; i < block_size; ++i) {
        ipad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(key_block[i] ^ 0x5c);
    }
    std::vector<std::uint8_t> inner;
    inner.reserve(block_size + data_length);
    inner.insert(inner.end(), ipad.begin(), ipad.end());
    inner.insert(inner.end(), data, data + data_length);
    const std::vector<std::uint8_t> inner_hash = digest(inner.data(), inner.size());
    std::vector<std::uint8_t> outer;
    outer.reserve(block_size + inner_hash.size());
    outer.insert(outer.end(), opad.begin(), opad.end());
    outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
    return digest(outer.data(), outer.size());
}

std::vector<std::uint8_t> hmac_bcrypt(CryptoHmacAlgorithm algorithm, const std::uint8_t* key, std::size_t key_length,
                                      const std::uint8_t* data, std::size_t data_length) {
    const CryptoHashAlgorithm hash_algorithm =
        algorithm == CryptoHmacAlgorithm::Sha256 ? CryptoHashAlgorithm::Sha256 : CryptoHashAlgorithm::Sha512;
    const std::size_t block_size = algorithm == CryptoHmacAlgorithm::Sha256 ? 64 : 128;
    const auto digest = [hash_algorithm](const std::uint8_t* chunk, std::size_t length) {
        return digest_bcrypt(hash_algorithm, chunk, length);
    };
    return hmac_from_digest(digest, block_size, key, key_length, data, data_length);
}

#endif

}  // namespace

bool crypto_backend_available() {
#if defined(PSCRIPT_HAS_OPENSSL) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

bool crypto_algorithm_available(CryptoHashAlgorithm algorithm) {
#if !defined(PSCRIPT_HAS_OPENSSL) && defined(_WIN32)
    if (algorithm == CryptoHashAlgorithm::Blake2b || algorithm == CryptoHashAlgorithm::Blake2s) {
        return false;
    }
#endif
    (void)algorithm;
    return crypto_backend_available();
}

bool parse_crypto_hash_algorithm(std::string_view name, CryptoHashAlgorithm& out) {
    if (name == "sha256") {
        out = CryptoHashAlgorithm::Sha256;
        return true;
    }
    if (name == "sha512") {
        out = CryptoHashAlgorithm::Sha512;
        return true;
    }
    if (name == "sha3_256") {
        out = CryptoHashAlgorithm::Sha3_256;
        return true;
    }
    if (name == "sha3_512") {
        out = CryptoHashAlgorithm::Sha3_512;
        return true;
    }
    if (name == "blake2b") {
        out = CryptoHashAlgorithm::Blake2b;
        return true;
    }
    if (name == "blake2s") {
        out = CryptoHashAlgorithm::Blake2s;
        return true;
    }
    return false;
}

std::string crypto_hash_algorithm_name(CryptoHashAlgorithm algorithm) {
    switch (algorithm) {
        case CryptoHashAlgorithm::Sha256:
            return "sha256";
        case CryptoHashAlgorithm::Sha512:
            return "sha512";
        case CryptoHashAlgorithm::Sha3_256:
            return "sha3_256";
        case CryptoHashAlgorithm::Sha3_512:
            return "sha3_512";
        case CryptoHashAlgorithm::Blake2b:
            return "blake2b";
        case CryptoHashAlgorithm::Blake2s:
            return "blake2s";
    }
    return "sha256";
}

std::vector<std::uint8_t> crypto_digest(CryptoHashAlgorithm algorithm, const std::uint8_t* data, std::size_t length) {
    if (!crypto_backend_available()) {
        crypto_fail("crypto support is not available in this build");
    }
    if (!crypto_algorithm_available(algorithm)) {
        crypto_fail("hash algorithm is not available in this build");
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    return digest_openssl(algorithm, data, length);
#elif defined(_WIN32)
    return digest_bcrypt(algorithm, data, length);
#else
    crypto_fail("crypto support is not available in this build");
#endif
}

std::vector<std::uint8_t> crypto_hmac(CryptoHmacAlgorithm algorithm, const std::uint8_t* key, std::size_t key_length,
                                      const std::uint8_t* data, std::size_t data_length) {
    if (!crypto_backend_available()) {
        crypto_fail("crypto support is not available in this build");
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    return hmac_openssl(algorithm, key, key_length, data, data_length);
#elif defined(_WIN32)
    return hmac_bcrypt(algorithm, key, key_length, data, data_length);
#else
    crypto_fail("crypto support is not available in this build");
#endif
}

std::vector<std::uint8_t> crypto_random_bytes(std::size_t length) {
    require_positive_length(length, "random_bytes");
    if (!crypto_backend_available()) {
        crypto_fail("crypto support is not available in this build");
    }
    std::vector<std::uint8_t> out(length);
#if defined(PSCRIPT_HAS_OPENSSL)
    if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
        crypto_fail("random_bytes failed");
    }
#elif defined(_WIN32)
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        crypto_fail("random_bytes failed");
    }
#else
    crypto_fail("crypto support is not available in this build");
#endif
    return out;
}

bool crypto_secure_compare(const std::uint8_t* left, std::size_t left_length, const std::uint8_t* right,
                           std::size_t right_length) {
    if (left_length != right_length) {
        return false;
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    return CRYPTO_memcmp(left, right, left_length) == 0;
#else
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < left_length; ++i) {
        diff |= static_cast<std::uint8_t>(left[i] ^ right[i]);
    }
    return diff == 0;
#endif
}

std::vector<std::uint8_t> crypto_digest_file(CryptoHashAlgorithm algorithm, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        crypto_fail("failed to open file for hashing: " + path);
    }
    return crypto_digest_stream(algorithm, in);
}

std::vector<std::uint8_t> crypto_digest_stream(CryptoHashAlgorithm algorithm, std::istream& stream) {
    CryptoHasher hasher;
    hasher.reset(algorithm);
    std::vector<char> buffer(64 * 1024);
    while (stream) {
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = stream.gcount();
        if (got > 0) {
            hasher.update(reinterpret_cast<const std::uint8_t*>(buffer.data()), static_cast<std::size_t>(got));
        }
        if (!stream.good() && stream.eof()) {
            break;
        }
        if (!stream.good()) {
            crypto_fail("failed while reading stream for hashing");
        }
    }
    return hasher.digest();
}

struct CryptoHasher::Impl {
#if defined(PSCRIPT_HAS_OPENSSL)
    EVP_MD_CTX* ctx = nullptr;
#elif defined(_WIN32)
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> object;
    DWORD hash_length = 0;
    bool owns_hash = false;
#endif
};

CryptoHasher::CryptoHasher() : impl_(std::make_unique<Impl>()) {}

CryptoHasher::CryptoHasher(CryptoHasher&& other) noexcept
    : impl_(std::move(other.impl_)), algorithm_(other.algorithm_), finalized_(other.finalized_) {
    other.impl_ = std::make_unique<Impl>();
    other.finalized_ = true;
}

CryptoHasher& CryptoHasher::operator=(CryptoHasher&& other) noexcept {
    if (this != &other) {
        impl_.swap(other.impl_);
        std::swap(algorithm_, other.algorithm_);
        std::swap(finalized_, other.finalized_);
    }
    return *this;
}

CryptoHasher::~CryptoHasher() {
#if defined(PSCRIPT_HAS_OPENSSL)
    if (impl_->ctx != nullptr) {
        EVP_MD_CTX_free(impl_->ctx);
    }
#elif defined(_WIN32)
    if (impl_->owns_hash && impl_->hash != nullptr) {
        BCryptDestroyHash(impl_->hash);
    }
#endif
}

void CryptoHasher::reset(CryptoHashAlgorithm algorithm) {
    algorithm_ = algorithm;
    finalized_ = false;
    if (!crypto_algorithm_available(algorithm)) {
        crypto_fail("hash algorithm is not available in this build");
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    if (impl_->ctx == nullptr) {
        impl_->ctx = EVP_MD_CTX_new();
    }
    if (impl_->ctx == nullptr || EVP_DigestInit_ex(impl_->ctx, md_for(algorithm), nullptr) != 1) {
        crypto_fail("failed to initialize hasher");
    }
#elif defined(_WIN32)
    if (impl_->owns_hash && impl_->hash != nullptr) {
        BCryptDestroyHash(impl_->hash);
        impl_->hash = nullptr;
        impl_->owns_hash = false;
    }
    BcryptAlgorithm& base = bcrypt_for(algorithm);
    impl_->hash_length = base.hash_length;
    impl_->object.assign(base.object_length, 0);
    if (BCryptCreateHash(base.handle, &impl_->hash, impl_->object.data(), static_cast<ULONG>(impl_->object.size()),
                         nullptr, 0, 0) != 0) {
        crypto_fail("failed to create streaming hash");
    }
    impl_->owns_hash = true;
#endif
}

void CryptoHasher::update(const std::uint8_t* data, std::size_t length) {
    if (finalized_) {
        crypto_fail("cannot update a finalized hasher");
    }
#if defined(PSCRIPT_HAS_OPENSSL)
    if (EVP_DigestUpdate(impl_->ctx, data, length) != 1) {
        crypto_fail("failed to update hasher");
    }
#elif defined(_WIN32)
    if (BCryptHashData(impl_->hash, const_cast<PUCHAR>(data), static_cast<ULONG>(length), 0) != 0) {
        crypto_fail("failed to update hasher");
    }
#endif
}

std::vector<std::uint8_t> CryptoHasher::digest() {
    if (finalized_) {
        crypto_fail("hasher already finalized");
    }
    finalized_ = true;
#if defined(PSCRIPT_HAS_OPENSSL)
    std::vector<std::uint8_t> out(EVP_MD_CTX_size(impl_->ctx));
    unsigned int out_len = 0;
    if (EVP_DigestFinal_ex(impl_->ctx, out.data(), &out_len) != 1) {
        crypto_fail("failed to finalize hasher");
    }
    out.resize(out_len);
    return out;
#elif defined(_WIN32)
    std::vector<std::uint8_t> out(impl_->hash_length);
    if (BCryptFinishHash(impl_->hash, out.data(), static_cast<ULONG>(out.size()), 0) != 0) {
        crypto_fail("failed to finalize hasher");
    }
    return out;
#else
    crypto_fail("crypto support is not available in this build");
#endif
}

}  // namespace pscript
