#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"
#include "stdlib/stdlib.h"
#include "runtime/crypto_ops.h"
#include "runtime/crypto_kdf_cipher.h"

#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace pscript {
namespace {

constexpr std::uint32_t kCryptoHasherClassIndex = 0;

std::vector<std::uint8_t> data_bytes_from_value(const Value& value, std::string_view function) {
    if (value.kind == ValueKind::Bytes && value.bytes) {
        return value.bytes->data;
    }
    if (value.kind == ValueKind::String) {
        return std::vector<std::uint8_t>(value.string.begin(), value.string.end());
    }
    native_type_error(std::string(function) + " expects bytes or string data");
}

std::vector<std::uint8_t> key_bytes_from_value(const Value& value, std::string_view function) {
    return data_bytes_from_value(value, function);
}

Value digest_value(CryptoHashAlgorithm algorithm, const Value& data_value, std::string_view function) {
    const std::vector<std::uint8_t> data = data_bytes_from_value(data_value, function);
    return Runtime::make_bytes(crypto_digest(algorithm, data.data(), data.size()));
}

NativeCryptoHasherData& require_crypto_hasher(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_crypto) {
        native_type_error("invalid hasher operation");
    }
    return *object.class_value->native_crypto;
}

std::int64_t require_integer_arg(const std::vector<Value>& args, std::size_t index, std::string_view function,
                                 std::string_view what) {
    if (args[index].kind != ValueKind::Integer) {
        native_type_error(std::string(function) + " expects integer " + std::string(what));
    }
    return args[index].integer;
}

std::size_t require_output_length_arg(const std::vector<Value>& args, std::size_t index, std::string_view function) {
    const std::int64_t length = require_integer_arg(args, index, function, "length");
    if (length <= 0) {
        native_value_error(std::string(function) + " length must be positive");
    }
    return static_cast<std::size_t>(length);
}

const Value* optional_dict_arg(const std::vector<Value>& args, std::size_t index, std::string_view function) {
    if (args[index].kind == ValueKind::Null) {
        return nullptr;
    }
    if (args[index].kind != ValueKind::Dict || !args[index].dict) {
        native_type_error(std::string(function) + " options must be a dict or null");
    }
    return &args[index];
}

std::optional<std::int64_t> dict_integer_field(const DictData& dict, const std::string& key) {
    for (const auto& entry : dict.entries) {
        if (entry.first == key) {
            if (entry.second.kind != ValueKind::Integer) {
                return std::nullopt;
            }
            return entry.second.integer;
        }
    }
    return std::nullopt;
}

CryptoKdfScryptParams read_scrypt_params(const Value* options, std::string_view function) {
    CryptoKdfScryptParams params;
    if (options == nullptr) {
        return params;
    }
    if (!options->dict) {
        native_type_error(std::string(function) + " options must be a dict or null");
    }
    if (const std::optional<std::int64_t> n = dict_integer_field(*options->dict, "n")) {
        if (*n <= 0) {
            native_value_error("scrypt n must be positive");
        }
        params.n = static_cast<std::uint32_t>(*n);
    }
    if (const std::optional<std::int64_t> r = dict_integer_field(*options->dict, "r")) {
        if (*r <= 0) {
            native_value_error("scrypt r must be positive");
        }
        params.r = static_cast<std::uint32_t>(*r);
    }
    if (const std::optional<std::int64_t> p = dict_integer_field(*options->dict, "p")) {
        if (*p <= 0) {
            native_value_error("scrypt p must be positive");
        }
        params.p = static_cast<std::uint32_t>(*p);
    }
    return params;
}

CryptoKdfArgon2Params read_argon2_params(const Value* options, std::string_view function) {
    CryptoKdfArgon2Params params;
    if (options == nullptr) {
        return params;
    }
    if (!options->dict) {
        native_type_error(std::string(function) + " options must be a dict or null");
    }
    if (const std::optional<std::int64_t> memory = dict_integer_field(*options->dict, "memory_kib")) {
        if (*memory <= 0) {
            native_value_error("argon2id memory_kib must be positive");
        }
        params.memory_kib = static_cast<std::uint32_t>(*memory);
    }
    if (const std::optional<std::int64_t> iterations = dict_integer_field(*options->dict, "iterations")) {
        if (*iterations <= 0) {
            native_value_error("argon2id iterations must be positive");
        }
        params.iterations = static_cast<std::uint32_t>(*iterations);
    }
    if (const std::optional<std::int64_t> parallelism = dict_integer_field(*options->dict, "parallelism")) {
        if (*parallelism <= 0) {
            native_value_error("argon2id parallelism must be positive");
        }
        params.parallelism = static_cast<std::uint32_t>(*parallelism);
    }
    return params;
}

std::vector<std::uint8_t> optional_aad_bytes(const Value& value, std::string_view function) {
    if (value.kind == ValueKind::Null) {
        return {};
    }
    return data_bytes_from_value(value, function);
}

Value make_aead_result(const CryptoAeadResult& result) {
    return Runtime::make_dict({
        {"ciphertext", Runtime::make_bytes(result.ciphertext)},
        {"tag", Runtime::make_bytes(result.tag)},
    });
}

CryptoHashAlgorithm require_hash_algorithm_arg(const std::vector<Value>& args, std::size_t index,
                                                 std::string_view function) {
    const std::string& name = native_require_string_arg(args, index, function, "algorithm");
    CryptoHashAlgorithm algorithm = CryptoHashAlgorithm::Sha256;
    if (!parse_crypto_hash_algorithm(name, algorithm)) {
        native_value_error("unsupported hash algorithm '" + name + "'");
    }
    if (!crypto_algorithm_available(algorithm)) {
        native_value_error("hash algorithm '" + name + "' is not available in this build");
    }
    return algorithm;
}

}  // namespace

Value native_crypto_sha256(const std::vector<Value>& args) {
    native_require_argc("sha256", args, 1);
    return digest_value(CryptoHashAlgorithm::Sha256, args[0], "sha256");
}

Value native_crypto_sha512(const std::vector<Value>& args) {
    native_require_argc("sha512", args, 1);
    return digest_value(CryptoHashAlgorithm::Sha512, args[0], "sha512");
}

Value native_crypto_sha3_256(const std::vector<Value>& args) {
    native_require_argc("sha3_256", args, 1);
    return digest_value(CryptoHashAlgorithm::Sha3_256, args[0], "sha3_256");
}

Value native_crypto_sha3_512(const std::vector<Value>& args) {
    native_require_argc("sha3_512", args, 1);
    return digest_value(CryptoHashAlgorithm::Sha3_512, args[0], "sha3_512");
}

Value native_crypto_blake2b(const std::vector<Value>& args) {
    native_require_argc("blake2b", args, 1);
    return digest_value(CryptoHashAlgorithm::Blake2b, args[0], "blake2b");
}

Value native_crypto_blake2s(const std::vector<Value>& args) {
    native_require_argc("blake2s", args, 1);
    return digest_value(CryptoHashAlgorithm::Blake2s, args[0], "blake2s");
}

Value native_crypto_hmac_sha256(const std::vector<Value>& args) {
    native_require_argc("hmac_sha256", args, 2);
    const std::vector<std::uint8_t> key = key_bytes_from_value(args[0], "hmac_sha256");
    const std::vector<std::uint8_t> data = data_bytes_from_value(args[1], "hmac_sha256");
    return Runtime::make_bytes(
        crypto_hmac(CryptoHmacAlgorithm::Sha256, key.data(), key.size(), data.data(), data.size()));
}

Value native_crypto_hmac_sha512(const std::vector<Value>& args) {
    native_require_argc("hmac_sha512", args, 2);
    const std::vector<std::uint8_t> key = key_bytes_from_value(args[0], "hmac_sha512");
    const std::vector<std::uint8_t> data = data_bytes_from_value(args[1], "hmac_sha512");
    return Runtime::make_bytes(
        crypto_hmac(CryptoHmacAlgorithm::Sha512, key.data(), key.size(), data.data(), data.size()));
}

Value native_crypto_random_bytes(const std::vector<Value>& args) {
    native_require_argc("random_bytes", args, 1);
    if (args[0].kind != ValueKind::Integer) {
        native_type_error("'random_bytes' expects integer length");
    }
    const std::int64_t length = args[0].integer;
    if (length < 0) {
        native_value_error("'random_bytes' length must be non-negative");
    }
    return Runtime::make_bytes(crypto_random_bytes(static_cast<std::size_t>(length)));
}

Value native_crypto_compare(const std::vector<Value>& args) {
    native_require_argc("compare", args, 2);
    const std::vector<std::uint8_t> left = data_bytes_from_value(args[0], "compare");
    const std::vector<std::uint8_t> right = data_bytes_from_value(args[1], "compare");
    return Runtime::make_bool(crypto_secure_compare(left.data(), left.size(), right.data(), right.size()));
}

Value native_crypto_hasher(const std::vector<Value>& args) {
    native_require_argc("hasher", args, 1);
    const CryptoHashAlgorithm algorithm = require_hash_algorithm_arg(args, 0, "hasher");
    auto data = std::make_shared<NativeCryptoHasherData>();
    data->kind = static_cast<std::uint32_t>(NativeClassKind::CryptoHasher);
    data->algorithm = algorithm;
    data->hasher.reset(algorithm);
    Value value = Runtime::make_class(kCryptoHasherClassIndex, "Hasher", std::vector<std::string>{});
    value.class_value->native_crypto = std::move(data);
    return value;
}

Value native_crypto_hash_file(const std::vector<Value>& args) {
    native_require_argc("hash_file", args, 2);
    const std::string& path = native_require_string_arg(args, 0, "hash_file", "path");
    const CryptoHashAlgorithm algorithm = require_hash_algorithm_arg(args, 1, "hash_file");
    return Runtime::make_bytes(crypto_digest_file(algorithm, path));
}

Value native_crypto_hash_io(Runtime& runtime, const std::vector<Value>& args) {
    native_require_argc("hash_io", args, 2);
    if (args[0].kind != ValueKind::Class || !args[0].class_value || !args[0].class_value->native_file) {
        native_type_error("'hash_io' expects an open file handle");
    }
    const CryptoHashAlgorithm algorithm = require_hash_algorithm_arg(args, 1, "hash_io");
    NativeFileHandleData& handle = *args[0].class_value->native_file;
    if (handle.closed || !handle.stream || !handle.stream->is_open()) {
        native_value_error("file handle is closed");
    }
    (void)runtime;
    return Runtime::make_bytes(crypto_digest_stream(algorithm, *handle.stream));
}

Value call_native_crypto_hasher_method(const Value& object, std::string_view name, const std::vector<Value>& args) {
    NativeCryptoHasherData& data = require_crypto_hasher(object);
    if (name == "update") {
        native_require_argc("update", args, 1);
        if (data.hasher.finalized()) {
            native_value_error("cannot update a finalized hasher");
        }
        const std::vector<std::uint8_t> chunk = data_bytes_from_value(args[0], "update");
        data.hasher.update(chunk.data(), chunk.size());
        return Runtime::make_null();
    }
    if (name == "digest") {
        native_require_argc("digest", args, 0);
        return Runtime::make_bytes(data.hasher.digest());
    }
    native_type_error("Hasher has no method '" + std::string(name) + "'");
}

bool is_native_crypto_hasher(const Value& object) {
    return object.kind == ValueKind::Class && object.class_value && object.class_value->native_crypto;
}

Value native_crypto_pbkdf2_sha256(const std::vector<Value>& args) {
    native_require_argc("pbkdf2_sha256", args, 4);
    const std::vector<std::uint8_t> password = data_bytes_from_value(args[0], "pbkdf2_sha256");
    const std::vector<std::uint8_t> salt = data_bytes_from_value(args[1], "pbkdf2_sha256");
    const std::int64_t iterations = require_integer_arg(args, 2, "pbkdf2_sha256", "iterations");
    if (iterations <= 0) {
        native_value_error("pbkdf2_sha256 iterations must be positive");
    }
    const std::size_t length = require_output_length_arg(args, 3, "pbkdf2_sha256");
    return Runtime::make_bytes(crypto_pbkdf2_sha256(password.data(), password.size(), salt.data(), salt.size(),
                                                    static_cast<std::uint32_t>(iterations), length));
}

Value native_crypto_scrypt(const std::vector<Value>& args) {
    native_require_argc("scrypt", args, 4);
    if (!crypto_kdf_scrypt_available()) {
        native_value_error("scrypt requires an OpenSSL-enabled build");
    }
    const std::vector<std::uint8_t> password = data_bytes_from_value(args[0], "scrypt");
    const std::vector<std::uint8_t> salt = data_bytes_from_value(args[1], "scrypt");
    const std::size_t length = require_output_length_arg(args, 2, "scrypt");
    const CryptoKdfScryptParams params = read_scrypt_params(optional_dict_arg(args, 3, "scrypt"), "scrypt");
    return Runtime::make_bytes(
        crypto_scrypt(password.data(), password.size(), salt.data(), salt.size(), params, length));
}

Value native_crypto_argon2id(const std::vector<Value>& args) {
    native_require_argc("argon2id", args, 4);
    if (!crypto_kdf_argon2id_available()) {
        native_value_error("argon2id requires OpenSSL 3.2 or newer in this build");
    }
    const std::vector<std::uint8_t> password = data_bytes_from_value(args[0], "argon2id");
    const std::vector<std::uint8_t> salt = data_bytes_from_value(args[1], "argon2id");
    const std::size_t length = require_output_length_arg(args, 2, "argon2id");
    const CryptoKdfArgon2Params params = read_argon2_params(optional_dict_arg(args, 3, "argon2id"), "argon2id");
    return Runtime::make_bytes(
        crypto_argon2id(password.data(), password.size(), salt.data(), salt.size(), params, length));
}

Value native_crypto_encrypt_aes_gcm(const std::vector<Value>& args) {
    native_require_argc("encrypt_aes_gcm", args, 4);
    const std::vector<std::uint8_t> key = key_bytes_from_value(args[0], "encrypt_aes_gcm");
    const std::vector<std::uint8_t> nonce = data_bytes_from_value(args[1], "encrypt_aes_gcm");
    const std::vector<std::uint8_t> plaintext = data_bytes_from_value(args[2], "encrypt_aes_gcm");
    const std::vector<std::uint8_t> aad = optional_aad_bytes(args[3], "encrypt_aes_gcm");
    return make_aead_result(crypto_encrypt_aes_gcm(key.data(), key.size(), nonce.data(), nonce.size(), plaintext.data(),
                                                     plaintext.size(), aad.data(), aad.size()));
}

Value native_crypto_decrypt_aes_gcm(const std::vector<Value>& args) {
    native_require_argc("decrypt_aes_gcm", args, 5);
    const std::vector<std::uint8_t> key = key_bytes_from_value(args[0], "decrypt_aes_gcm");
    const std::vector<std::uint8_t> nonce = data_bytes_from_value(args[1], "decrypt_aes_gcm");
    const std::vector<std::uint8_t> ciphertext = data_bytes_from_value(args[2], "decrypt_aes_gcm");
    const std::vector<std::uint8_t> tag = data_bytes_from_value(args[3], "decrypt_aes_gcm");
    const std::vector<std::uint8_t> aad = optional_aad_bytes(args[4], "decrypt_aes_gcm");
    return Runtime::make_bytes(crypto_decrypt_aes_gcm(key.data(), key.size(), nonce.data(), nonce.size(),
                                                      ciphertext.data(), ciphertext.size(), tag.data(), tag.size(),
                                                      aad.data(), aad.size()));
}

Value native_crypto_encrypt_chacha20_poly1305(const std::vector<Value>& args) {
    native_require_argc("encrypt_chacha20_poly1305", args, 4);
    const std::vector<std::uint8_t> key = key_bytes_from_value(args[0], "encrypt_chacha20_poly1305");
    const std::vector<std::uint8_t> nonce = data_bytes_from_value(args[1], "encrypt_chacha20_poly1305");
    const std::vector<std::uint8_t> plaintext = data_bytes_from_value(args[2], "encrypt_chacha20_poly1305");
    const std::vector<std::uint8_t> aad = optional_aad_bytes(args[3], "encrypt_chacha20_poly1305");
    return make_aead_result(crypto_encrypt_chacha20_poly1305(key.data(), key.size(), nonce.data(), nonce.size(),
                                                             plaintext.data(), plaintext.size(), aad.data(),
                                                             aad.size()));
}

Value native_crypto_decrypt_chacha20_poly1305(const std::vector<Value>& args) {
    native_require_argc("decrypt_chacha20_poly1305", args, 5);
    const std::vector<std::uint8_t> key = key_bytes_from_value(args[0], "decrypt_chacha20_poly1305");
    const std::vector<std::uint8_t> nonce = data_bytes_from_value(args[1], "decrypt_chacha20_poly1305");
    const std::vector<std::uint8_t> ciphertext = data_bytes_from_value(args[2], "decrypt_chacha20_poly1305");
    const std::vector<std::uint8_t> tag = data_bytes_from_value(args[3], "decrypt_chacha20_poly1305");
    const std::vector<std::uint8_t> aad = optional_aad_bytes(args[4], "decrypt_chacha20_poly1305");
    return Runtime::make_bytes(crypto_decrypt_chacha20_poly1305(key.data(), key.size(), nonce.data(), nonce.size(),
                                                              ciphertext.data(), ciphertext.size(), tag.data(),
                                                              tag.size(), aad.data(), aad.size()));
}

}  // namespace pscript
