#include "stdlib/native_common.h"
#include "stdlib/native_impl.h"

#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {
namespace {

std::mt19937_64& uuid_engine() {
    static std::mt19937_64 engine{std::random_device{}()};
    return engine;
}

[[noreturn]] void uuid_fail(std::string message) {
    native_value_error(std::move(message));
}

Value uuid_wrapper(std::string canonical, std::vector<std::uint8_t> bytes) {
    return Runtime::make_dict({{"string", Runtime::make_string(std::move(canonical))},
                               {"bytes", Runtime::make_bytes(std::move(bytes))}});
}

std::string format_uuid_bytes(const std::array<std::uint8_t, 16>& data) {
    static const char* hex = "0123456789ABCDEF";
    auto emit = [&](std::size_t start, std::size_t count) {
        std::string part;
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t b = data[start + i];
            part.push_back(hex[b >> 4]);
            part.push_back(hex[b & 0xF]);
        }
        return part;
    };
    return emit(0, 4) + '-' + emit(4, 2) + '-' + emit(6, 2) + '-' + emit(8, 2) + '-' + emit(10, 6);
}

}  // namespace

Value native_uuid_v4(const std::vector<Value>& args) {
    if (!args.empty()) {
        uuid_fail("'v4' expects 0 arguments");
    }
    std::array<std::uint8_t, 16> data{};
    for (std::uint8_t& b : data) {
        b = static_cast<std::uint8_t>(uuid_engine()() & 0xFF);
    }
    data[6] = static_cast<std::uint8_t>((data[6] & 0x0F) | 0x40);
    data[8] = static_cast<std::uint8_t>((data[8] & 0x3F) | 0x80);
    const std::string text = format_uuid_bytes(data);
    return uuid_wrapper(text, std::vector<std::uint8_t>(data.begin(), data.end()));
}

Value native_uuid_v7(const std::vector<Value>& args) {
    if (!args.empty()) {
        uuid_fail("'v7' expects 0 arguments");
    }
    const std::uint64_t ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
    std::array<std::uint8_t, 16> data{};
    data[0] = static_cast<std::uint8_t>((ms >> 40) & 0xFF);
    data[1] = static_cast<std::uint8_t>((ms >> 32) & 0xFF);
    data[2] = static_cast<std::uint8_t>((ms >> 24) & 0xFF);
    data[3] = static_cast<std::uint8_t>((ms >> 16) & 0xFF);
    data[4] = static_cast<std::uint8_t>((ms >> 8) & 0xFF);
    data[5] = static_cast<std::uint8_t>(ms & 0xFF);
    for (std::size_t i = 6; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(uuid_engine()() & 0xFF);
    }
    data[6] = static_cast<std::uint8_t>((data[6] & 0x0F) | 0x70);
    data[8] = static_cast<std::uint8_t>((data[8] & 0x3F) | 0x80);
    const std::string text = format_uuid_bytes(data);
    return uuid_wrapper(text, std::vector<std::uint8_t>(data.begin(), data.end()));
}

Value native_uuid_parse(const std::vector<Value>& args) {
    native_require_argc("parse", args, 1);
    std::string text = native_require_string(args[0], "parse", "uuid");
    std::string compact;
    compact.reserve(32);
    for (char ch : text) {
        if (ch == '-') {
            continue;
        }
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            uuid_fail("invalid uuid");
        }
        compact.push_back(ch);
    }
    if (compact.size() != 32) {
        uuid_fail("invalid uuid");
    }
    std::vector<std::uint8_t> bytes;
    bytes.reserve(16);
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return c - 'A' + 10;
    };
    for (std::size_t i = 0; i < compact.size(); i += 2) {
        bytes.push_back(static_cast<std::uint8_t>((hex(compact[i]) << 4) | hex(compact[i + 1])));
    }
    if (text.find('-') == std::string::npos) {
        std::array<std::uint8_t, 16> arr{};
        std::copy(bytes.begin(), bytes.end(), arr.begin());
        text = format_uuid_bytes(arr);
    }
    return uuid_wrapper(text, std::move(bytes));
}

}  // namespace pscript
