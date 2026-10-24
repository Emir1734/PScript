#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {

enum class CryptoHashAlgorithm {
    Sha256,
    Sha512,
    Sha3_256,
    Sha3_512,
    Blake2b,
    Blake2s,
};

enum class CryptoHmacAlgorithm {
    Sha256,
    Sha512,
};

bool crypto_backend_available();
bool crypto_algorithm_available(CryptoHashAlgorithm algorithm);

std::vector<std::uint8_t> crypto_digest(CryptoHashAlgorithm algorithm, const std::uint8_t* data, std::size_t length);
std::vector<std::uint8_t> crypto_hmac(CryptoHmacAlgorithm algorithm, const std::uint8_t* key, std::size_t key_length,
                                      const std::uint8_t* data, std::size_t data_length);
std::vector<std::uint8_t> crypto_random_bytes(std::size_t length);
bool crypto_secure_compare(const std::uint8_t* left, std::size_t left_length, const std::uint8_t* right,
                           std::size_t right_length);

std::vector<std::uint8_t> crypto_digest_file(CryptoHashAlgorithm algorithm, const std::string& path);
std::vector<std::uint8_t> crypto_digest_stream(CryptoHashAlgorithm algorithm, std::istream& stream);

class CryptoHasher {
public:
    CryptoHasher();
    ~CryptoHasher();

    CryptoHasher(const CryptoHasher&) = delete;
    CryptoHasher& operator=(const CryptoHasher&) = delete;
    CryptoHasher(CryptoHasher&& other) noexcept;
    CryptoHasher& operator=(CryptoHasher&& other) noexcept;

    void reset(CryptoHashAlgorithm algorithm);
    void update(const std::uint8_t* data, std::size_t length);
    std::vector<std::uint8_t> digest();

    bool finalized() const { return finalized_; }
    CryptoHashAlgorithm algorithm() const { return algorithm_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    CryptoHashAlgorithm algorithm_ = CryptoHashAlgorithm::Sha256;
    bool finalized_ = false;
};

bool parse_crypto_hash_algorithm(std::string_view name, CryptoHashAlgorithm& out);
std::string crypto_hash_algorithm_name(CryptoHashAlgorithm algorithm);

}  // namespace pscript
