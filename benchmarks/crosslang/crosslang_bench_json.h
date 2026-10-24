#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace crosslang {

inline constexpr std::size_t kJsonBenchItemCount = 500;
inline constexpr std::size_t kJsonStringifyItemCount = 200;

inline const std::string& json_benchmark_payload() {
    static const std::string payload = []() {
        std::string s = R"({"items":[)";
        for (std::size_t i = 0; i < kJsonBenchItemCount; ++i) {
            if (i > 0) {
                s.push_back(',');
            }
            s.append(std::to_string(i));
        }
        s.append("]}");
        return s;
    }();
    return payload;
}

inline std::int64_t json_count_items_array(std::string_view json) {
    constexpr std::string_view marker = R"("items":[)";
    const std::size_t start = json.find(marker);
    if (start == std::string_view::npos) {
        return -1;
    }
    std::size_t pos = start + marker.size();
    std::int64_t count = 0;
    while (pos < json.size()) {
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
            ++pos;
        }
        if (pos >= json.size()) {
            return -1;
        }
        if (json[pos] == ']') {
            return count;
        }
        if (json[pos] == '-' || std::isdigit(static_cast<unsigned char>(json[pos]))) {
            if (json[pos] == '-') {
                ++pos;
            }
            while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
                ++pos;
            }
            ++count;
        } else {
            return -1;
        }
        while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < json.size() && json[pos] == ']') {
            return count;
        }
        return -1;
    }
    return -1;
}

inline std::string json_build_items_object(std::size_t count) {
    std::string s = R"({"items":[)";
    for (std::size_t i = 0; i < count; ++i) {
        if (i > 0) {
            s.push_back(',');
        }
        s.append(std::to_string(i));
    }
    s.push_back(']');
    s.push_back('}');
    return s;
}

inline std::int64_t run_json_parse(std::uint64_t repetitions) {
    const std::string& payload = json_benchmark_payload();
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < repetitions; ++i) {
        total += json_count_items_array(payload);
    }
    return total;
}

inline std::int64_t run_json_stringify(std::uint64_t repetitions) {
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < repetitions; ++i) {
        const std::string encoded = json_build_items_object(kJsonStringifyItemCount);
        total += static_cast<std::int64_t>(encoded.size());
    }
    return total;
}

inline std::int64_t run_json_pipeline(std::uint64_t repetitions) {
    const std::string& payload = json_benchmark_payload();
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < repetitions; ++i) {
        const std::int64_t count = json_count_items_array(payload);
        std::int64_t sum = 0;
        for (std::int64_t j = 0; j < count; ++j) {
            sum += j;
        }
        const std::string encoded = json_build_items_object(static_cast<std::size_t>(count));
        total += sum + static_cast<std::int64_t>(encoded.size());
    }
    return total;
}

inline std::int64_t run_mixed_runtime(std::uint64_t repetitions) {
    const std::string& payload = json_benchmark_payload();
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < repetitions; ++i) {
        const std::int64_t count = json_count_items_array(payload);
        std::int64_t sum = 0;
        for (std::int64_t j = 0; j < count; ++j) {
            sum += j;
        }
        const std::string encoded = json_build_items_object(static_cast<std::size_t>(count));
        total += sum + static_cast<std::int64_t>(encoded.size());
        total += json_count_items_array(encoded);
    }
    return total;
}

}  // namespace crosslang
