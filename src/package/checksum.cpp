#include "package/checksum.h"

#include "runtime/crypto_ops.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace pscript::package {
namespace {

std::string bytes_to_hex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const std::uint8_t byte : bytes) {
        out << std::setw(2) << static_cast<int>(byte);
    }
    return out.str();
}

}  // namespace

std::string sha256_directory(const std::string& path, std::string* error) {
    CryptoHasher hasher;
    hasher.reset(CryptoHashAlgorithm::Sha256);
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(path, ec)) {
        if (ec) {
            if (error) {
                *error = ec.message();
            }
            return {};
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string relative = std::filesystem::relative(entry.path(), path).string();
        hasher.update(reinterpret_cast<const std::uint8_t*>(relative.data()), relative.size());
        const std::uint8_t separator = 0;
        hasher.update(&separator, 1);
        std::ifstream in(entry.path(), std::ios::binary);
        if (!in) {
            if (error) {
                *error = "cannot read " + entry.path().string();
            }
            return {};
        }
        std::vector<char> buffer(4096);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = in.gcount();
            if (count > 0) {
                hasher.update(reinterpret_cast<const std::uint8_t*>(buffer.data()),
                              static_cast<std::size_t>(count));
            }
        }
    }
    return bytes_to_hex(hasher.digest());
}

bool verify_directory_checksum(const std::string& path, const std::string& expected_hex, std::string* error) {
    const std::string actual = sha256_directory(path, error);
    if (actual.empty() && error && !error->empty()) {
        return false;
    }
    if (actual != expected_hex) {
        if (error) {
            *error = "checksum mismatch for " + path;
        }
        return false;
    }
    return true;
}

}  // namespace pscript::package
