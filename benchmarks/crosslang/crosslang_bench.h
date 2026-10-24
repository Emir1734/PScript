#pragma once

#include "crosslang_bench_json.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <functional>
#include <unordered_map>

namespace crosslang {

inline std::uint64_t bench_iterations(int argc, char** argv, std::uint64_t default_iters) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--iters") {
            return static_cast<std::uint64_t>(std::strtoull(argv[i + 1], nullptr, 10));
        }
    }
    return default_iters;
}

inline std::string_view workload_name(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--workload") {
            return argv[i + 1];
        }
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg.empty() || arg.front() == '-') {
            if (arg == "--iters") {
                ++i;
            }
            continue;
        }
        return arg;
    }
    return "int_loop";
}

inline std::uint64_t default_workload_iterations(std::string_view workload) {
    if (workload == "direct_inc_call" || workload == "add_constant" || workload == "indirect_call") {
        return 400'000;
    }
    if (workload == "nested_call") {
        return 50'000;
    }
    if (workload == "closure_call") {
        return 200'000;
    }
    if (workload == "int_loop") {
        return 2'000'000;
    }
    if (workload == "list_index") {
        return 5'000;
    }
    if (workload == "string_eq") {
        return 8'000;
    }
    if (workload == "string_concat") {
        return 250;
    }
    if (workload == "arithmetic") {
        return 8'000;
    }
    if (workload == "dict_lookup") {
        return 400;
    }
    if (workload == "json_parse") {
        return 200;
    }
    if (workload == "json_stringify") {
        return 400;
    }
    if (workload == "json_pipeline") {
        return 100;
    }
    if (workload == "mixed_runtime") {
        return 50;
    }
    return 2'000'000;
}

inline std::int64_t run_int_loop(std::uint64_t n) {
    std::int64_t sum = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        sum += static_cast<std::int64_t>(i);
    }
    return sum;
}

inline std::int64_t run_string_eq(std::uint64_t repetitions) {
    const char* needle = "needle";
    const char* abc = "abc";
    const char* xyz = "xyz";
    std::int64_t hits = 0;
    for (std::uint64_t i = 0; i < repetitions; ++i) {
        if (std::string_view(needle) == "needle") {
            ++hits;
        }
        if (std::string_view(abc) != xyz) {
            ++hits;
        }
    }
    return hits;
}

inline std::int64_t run_string_concat(std::uint64_t repetitions) {
    std::string s;
    for (std::uint64_t i = 0; i < repetitions; ++i) {
        s.append("x");
    }
    return static_cast<std::int64_t>(s.size());
}

inline std::int64_t run_list_index(std::uint64_t repetitions) {
    std::int64_t total = 0;
    std::int64_t items[256];
    for (std::int64_t i = 0; i < 256; ++i) {
        items[i] = i;
    }
    for (std::uint64_t rep = 0; rep < repetitions; ++rep) {
        for (std::int64_t i = 0; i < 256; ++i) {
            total += items[i];
        }
    }
    return total;
}

inline std::int64_t run_direct_inc_call(std::uint64_t n) {
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        total += static_cast<std::int64_t>(i) + 1;
    }
    return total;
}

inline std::int64_t run_nested_call(std::uint64_t n) {
    auto leaf = [](std::int64_t x) { return x; };
    auto mid = [&](std::int64_t x) { return leaf(x) + leaf(x + 1); };
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        total += mid(static_cast<std::int64_t>(i));
    }
    return total;
}

inline std::int64_t run_closure_call(std::uint64_t n, std::int64_t capture) {
    auto callee = [capture](std::int64_t x) { return x + capture; };
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        total += callee(static_cast<std::int64_t>(i));
    }
    return total;
}

inline std::int64_t run_add_constant(std::uint64_t n, std::int64_t constant) {
    auto add = [constant](std::int64_t x) { return x + constant; };
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        total += add(static_cast<std::int64_t>(i));
    }
    return total;
}

inline std::int64_t run_indirect_call(std::uint64_t n) {
    auto inc = [](std::int64_t x) { return x + 1; };
    const auto& callee = inc;
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        total += callee(static_cast<std::int64_t>(i));
    }
    return total;
}

inline std::int64_t run_arithmetic(std::uint64_t n) {
    std::int64_t total = 0;
    for (std::uint64_t i = 0; i < n; ++i) {
        total += static_cast<std::int64_t>(i);
    }
    return total;
}

// PScript jit.exec workloads (fixed sizes; checksums match pscript_1_0 gates).
inline std::int64_t run_pscript_loops() {
    const std::int64_t n = 60'000;
    std::int64_t s = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        s += 1;
    }
    return s;
}

inline std::int64_t run_pscript_num_lu_divmod() {
    const std::int64_t n = 12'000;
    std::int64_t total = 0;
    for (std::int64_t i = 1; i < n; ++i) {
        total += (i / 3) + (i % 7);
    }
    return total;
}

inline std::int64_t run_pscript_closure_call() {
    const std::int64_t n = 25'000;
    const std::int64_t base = 100;
    const auto add = [base](std::int64_t x) { return base + x; };
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        total += add(i);
    }
    return total;
}

inline std::int64_t run_pscript_closure_nested() {
    const std::int64_t n = 25'000;
    const auto outer = [](std::int64_t x) {
        return [x](std::int64_t y) {
            return [x, y](std::int64_t z) { return x + y + z; };
        };
    };
    const auto g = outer(10)(20);
    std::int64_t total = 0;
    for (std::int64_t i = 0; i < n; ++i) {
        total += g(i);
    }
    return total;
}

inline std::int64_t run_pscript_closure_recursive() {
    std::function<std::int64_t(std::int64_t)> sum;
    sum = [&sum](std::int64_t n) -> std::int64_t {
        if (n <= 0) {
            return 0;
        }
        return n + sum(n - 1);
    };
    return sum(200);
}

// Same numeric result as recursive sum(200); matches PScript OPT iterative lowering.
inline std::int64_t run_pscript_closure_recursive_iter() {
    std::int64_t acc = 0;
    for (std::int64_t n = 200; n > 0; --n) {
        acc += n;
    }
    return acc;
}

inline std::int64_t run_dict_lookup(std::uint64_t repetitions) {
    std::unordered_map<std::string, std::int64_t> map;
    for (std::int64_t i = 0; i < 512; ++i) {
        map.emplace("k" + std::to_string(i), i);
    }
    std::int64_t total = 0;
    for (std::uint64_t rep = 0; rep < repetitions; ++rep) {
        for (std::int64_t j = 0; j < 512; ++j) {
            total += map.at("k" + std::to_string(j));
        }
    }
    return total;
}

inline void print_timing(const char* runtime, std::string_view workload, double median_ms, std::int64_t checksum) {
    std::cout << runtime << '\t' << workload << '\t';
    std::cout.setf(std::ios::fixed);
    std::cout.precision(6);
    std::cout << median_ms << '\t' << checksum << '\n';
}

template <typename Fn>
double median_ms(Fn&& fn, int repeats, int warmup = 3) {
    for (int w = 0; w < warmup; ++w) {
        fn();
    }
    double samples[16];
    const int count = repeats < 1 ? 1 : (repeats > 16 ? 16 : repeats);
    for (int i = 0; i < count; ++i) {
        const auto start = std::chrono::steady_clock::now();
        fn();
        const auto end = std::chrono::steady_clock::now();
        samples[i] = std::chrono::duration<double, std::milli>(end - start).count();
    }
    for (int i = 1; i < count; ++i) {
        for (int j = i; j > 0 && samples[j] < samples[j - 1]; --j) {
            const double tmp = samples[j];
            samples[j] = samples[j - 1];
            samples[j - 1] = tmp;
        }
    }
    return samples[count / 2];
}

}  // namespace crosslang
