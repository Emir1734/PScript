#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef small
#undef small
#endif
#ifdef medium
#undef medium
#endif
#ifdef large
#undef large
#endif
#endif

namespace pscript::bench {

inline std::int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline const char* build_configuration() {
#if defined(NDEBUG)
    return "Release";
#else
    return "Debug";
#endif
}

inline const char* platform_name() {
#if defined(_WIN64)
    return "windows_x64";
#elif defined(__linux__) && defined(__x86_64__)
    return "linux_x64";
#else
    return "unknown";
#endif
}

enum class MeasurementQuality {
    Reliable,
    Noisy,
    InsufficientSamples,
    SingleSample,
};

inline const char* measurement_quality_name(MeasurementQuality q) {
    switch (q) {
        case MeasurementQuality::Reliable:
            return "reliable";
        case MeasurementQuality::Noisy:
            return "noisy";
        case MeasurementQuality::InsufficientSamples:
            return "insufficient_samples";
        case MeasurementQuality::SingleSample:
            return "single_sample";
    }
    return "unknown";
}

struct SampleStats {
    std::int64_t min_ns = 0;
    std::int64_t max_ns = 0;
    std::int64_t median_ns = 0;
    std::int64_t q1_ns = 0;
    std::int64_t q3_ns = 0;
    std::int64_t iqr_ns = 0;
    std::int64_t total_ns = 0;
    std::size_t count = 0;
    MeasurementQuality quality = MeasurementQuality::InsufficientSamples;
};

inline std::int64_t percentile_from_sorted(const std::vector<std::int64_t>& sorted, double p) {
    if (sorted.empty()) {
        return 0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double rank = p * static_cast<double>(sorted.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(std::floor(rank));
    const std::size_t hi = static_cast<std::size_t>(std::ceil(rank));
    if (lo == hi) {
        return sorted[lo];
    }
    const double weight = rank - static_cast<double>(lo);
    return static_cast<std::int64_t>(
        std::llround(static_cast<double>(sorted[lo]) * (1.0 - weight) + static_cast<double>(sorted[hi]) * weight));
}

inline MeasurementQuality classify_sample_quality(const SampleStats& stats) {
    if (stats.count == 0) {
        return MeasurementQuality::InsufficientSamples;
    }
    if (stats.count == 1) {
        return MeasurementQuality::SingleSample;
    }
    if (stats.count < 4) {
        return MeasurementQuality::InsufficientSamples;
    }
    if (stats.median_ns <= 0) {
        return MeasurementQuality::Noisy;
    }
    const double spread = static_cast<double>(stats.iqr_ns) / static_cast<double>(stats.median_ns);
    const double max_ratio = static_cast<double>(stats.max_ns) / static_cast<double>(stats.median_ns);
    if (spread > 0.35 || (max_ratio > 3.0 && stats.max_ns - stats.min_ns > stats.iqr_ns * 2)) {
        return MeasurementQuality::Noisy;
    }
    return MeasurementQuality::Reliable;
}

inline SampleStats summarize_samples(std::vector<std::int64_t> samples) {
    SampleStats stats;
    stats.count = samples.size();
    if (samples.empty()) {
        stats.quality = MeasurementQuality::InsufficientSamples;
        return stats;
    }
    std::sort(samples.begin(), samples.end());
    stats.min_ns = samples.front();
    stats.max_ns = samples.back();
    stats.total_ns = 0;
    for (std::int64_t v : samples) {
        stats.total_ns += v;
    }
    stats.median_ns = samples[samples.size() / 2];
    if (samples.size() >= 2) {
        stats.q1_ns = percentile_from_sorted(samples, 0.25);
        stats.q3_ns = percentile_from_sorted(samples, 0.75);
        stats.iqr_ns = stats.q3_ns - stats.q1_ns;
        if (stats.iqr_ns < 0) {
            stats.iqr_ns = 0;
        }
    }
    stats.quality = classify_sample_quality(stats);
    return stats;
}

struct HostCpuSnapshot {
    bool valid = false;
    std::uint64_t idle_ticks = 0;
    std::uint64_t kernel_ticks = 0;
    std::uint64_t user_ticks = 0;
};

inline HostCpuSnapshot capture_host_cpu_snapshot() {
    HostCpuSnapshot snap;
#if defined(_WIN32)
    FILETIME idle_ft{};
    FILETIME kernel_ft{};
    FILETIME user_ft{};
    if (!GetSystemTimes(&idle_ft, &kernel_ft, &user_ft)) {
        return snap;
    }
    auto to_u64 = [](const FILETIME& ft) -> std::uint64_t {
        ULARGE_INTEGER uli{};
        uli.LowPart = ft.dwLowDateTime;
        uli.HighPart = ft.dwHighDateTime;
        return uli.QuadPart;
    };
    snap.idle_ticks = to_u64(idle_ft);
    snap.kernel_ticks = to_u64(kernel_ft);
    snap.user_ticks = to_u64(user_ft);
    snap.valid = true;
#endif
    return snap;
}

inline std::optional<double> host_cpu_busy_fraction(const HostCpuSnapshot& start, const HostCpuSnapshot& end) {
    if (!start.valid || !end.valid) {
        return std::nullopt;
    }
    const std::uint64_t idle_delta = end.idle_ticks - start.idle_ticks;
    const std::uint64_t kernel_delta = end.kernel_ticks - start.kernel_ticks;
    const std::uint64_t user_delta = end.user_ticks - start.user_ticks;
    const std::uint64_t total_delta = kernel_delta + user_delta + idle_delta;
    if (total_delta == 0) {
        return std::nullopt;
    }
    const double busy = 1.0 - static_cast<double>(idle_delta) / static_cast<double>(total_delta);
    if (busy < 0.0) {
        return 0.0;
    }
    if (busy > 1.0) {
        return 1.0;
    }
    return busy;
}

struct BenchRunEnvironment {
    HostCpuSnapshot cpu_start;
    HostCpuSnapshot cpu_end;
};

struct BenchOptions {
    int warmup = 1;
    int iterations = 5;
    std::string json_path;
    std::string filter_prefix;
    bool jit_ic_dump = false;
    bool jit_ic_profile = false;
    bool continue_on_checksum_mismatch = false;
};

struct BenchRecord {
    std::string name;
    std::string category;
    std::string tier;
    std::string phase;
    SampleStats stats;
    std::optional<double> throughput_ops_per_sec;
    std::optional<std::uint64_t> memory_allocation_count;
    std::optional<std::uint64_t> memory_allocation_bytes;
    std::optional<std::int64_t> compile_median_ns;
};

inline void json_escape(std::ostream& out, std::string_view value) {
    out << '"';
    for (char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            default:
                out << ch;
                break;
        }
    }
    out << '"';
}

inline void write_json_report(const BenchOptions& options, const std::vector<BenchRecord>& records, std::ostream& out,
                              const char* pscript_version, const BenchRunEnvironment* environment = nullptr) {
    const auto wall_time = std::chrono::system_clock::now();
    const std::time_t wall_tt = std::chrono::system_clock::to_time_t(wall_time);
    out << "{\n";
    out << "  \"benchmark_version\": 2,\n";
    out << "  \"pscript_version\": \"" << pscript_version << "\",\n";
    out << "  \"platform\": \"" << platform_name() << "\",\n";
    out << "  \"configuration\": \"" << build_configuration() << "\",\n";
    out << "  \"warmup\": " << options.warmup << ",\n";
    out << "  \"iterations\": " << options.iterations << ",\n";
    out << "  \"logical_processor_count\": "
        << std::max(1u, std::thread::hardware_concurrency()) << ",\n";
    if (environment != nullptr) {
        const std::optional<double> busy = host_cpu_busy_fraction(environment->cpu_start, environment->cpu_end);
        out << "  \"environment\": {\n";
        out << "    \"host_cpu_busy_fraction_during_run\": ";
        if (busy) {
            out << std::fixed << std::setprecision(4) << *busy;
        } else {
            out << "null";
        }
        out << ",\n    \"host_cpu_snapshot_available\": "
            << (environment->cpu_start.valid && environment->cpu_end.valid ? "true" : "false") << "\n  },\n";
    }
    std::tm utc_tm{};
#if defined(_WIN32)
    gmtime_s(&utc_tm, &wall_tt);
#else
    gmtime_r(&wall_tt, &utc_tm);
#endif
    out << "  \"timestamp_utc\": \"" << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ") << "\",\n";
    out << "  \"results\": [\n";
    for (std::size_t i = 0; i < records.size(); ++i) {
        const BenchRecord& r = records[i];
        out << "    {\n";
        out << "      \"name\": ";
        json_escape(out, r.name);
        out << ",\n      \"category\": ";
        json_escape(out, r.category);
        out << ",\n      \"tier\": ";
        json_escape(out, r.tier);
        out << ",\n      \"phase\": ";
        json_escape(out, r.phase);
        out << ",\n      \"median_ns\": " << r.stats.median_ns << ",\n";
        out << "      \"min_ns\": " << r.stats.min_ns << ",\n";
        out << "      \"max_ns\": " << r.stats.max_ns << ",\n";
        out << "      \"q1_ns\": " << r.stats.q1_ns << ",\n";
        out << "      \"q3_ns\": " << r.stats.q3_ns << ",\n";
        out << "      \"iqr_ns\": " << r.stats.iqr_ns << ",\n";
        out << "      \"total_ns\": " << r.stats.total_ns << ",\n";
        out << "      \"sample_count\": " << r.stats.count << ",\n";
        out << "      \"measurement_quality\": \"" << measurement_quality_name(r.stats.quality) << "\"";
        if (r.throughput_ops_per_sec) {
            out << ",\n      \"throughput_ops_per_sec\": " << std::fixed << std::setprecision(3)
                << *r.throughput_ops_per_sec;
        }
        if (r.memory_allocation_count) {
            out << ",\n      \"memory_allocation_count\": " << *r.memory_allocation_count;
        }
        if (r.memory_allocation_bytes) {
            out << ",\n      \"memory_allocation_bytes\": " << *r.memory_allocation_bytes;
        }
        if (r.compile_median_ns) {
            out << ",\n      \"compile_median_ns\": " << *r.compile_median_ns;
        }
        out << "\n    }";
        if (i + 1 < records.size()) {
            out << ',';
        }
        out << '\n';
    }
    out << "  ]\n}\n";
}

inline bool bench_record_matches_filter(std::string_view name, std::string_view filter) {
    if (filter.empty()) {
        return true;
    }
    if (name == filter) {
        return true;
    }
    if (name.size() > filter.size() && name.rfind(filter, 0) == 0 &&
        (name[filter.size()] == '.' || filter.back() == '.')) {
        return true;
    }
    std::string child_prefix(filter);
    if (child_prefix.back() != '.') {
        child_prefix.push_back('.');
    }
    return name.size() > child_prefix.size() && name.rfind(child_prefix, 0) == 0;
}

inline void print_human_report(const BenchOptions& options, const std::vector<BenchRecord>& records) {
    std::cout << "PScript benchmark report\n";
    std::cout << "  platform=" << platform_name() << " config=" << build_configuration()
              << " warmup=" << options.warmup << " iterations=" << options.iterations << '\n';
    std::cout << "  jit.exec phase semantics:\n";
    std::cout << "    steady = one fresh Vm per tier, single vm.run() (does not use --iterations)\n";
    std::cout << "    cold / warm_steady = same Vm; cold=first run, warm_steady=median of --iterations "
                 "after --warmup (primary KPI for hot JIT)\n";
    std::cout << std::left << std::setw(42) << "name" << std::setw(12) << "tier" << std::setw(10) << "phase"
              << std::setw(14) << "median_ms" << std::setw(14) << "iqr_ms" << std::setw(14) << "min_ms"
              << std::setw(14) << "max_ms" << "quality/throughput\n";
    for (const BenchRecord& r : records) {
        if (!bench_record_matches_filter(r.name, options.filter_prefix)) {
            continue;
        }
        if (r.category == "representation") {
            std::cout << std::left << std::setw(42) << r.name << std::setw(12) << r.tier << std::setw(10) << r.phase
                      << "bytes=" << r.stats.median_ns << '\n';
            continue;
        }
        std::cout << std::left << std::setw(42) << r.name << std::setw(12) << r.tier << std::setw(10) << r.phase
                  << std::setw(14) << (static_cast<double>(r.stats.median_ns) / 1e6) << std::setw(14)
                  << (static_cast<double>(r.stats.iqr_ns) / 1e6) << std::setw(14)
                  << (static_cast<double>(r.stats.min_ns) / 1e6) << std::setw(14)
                  << (static_cast<double>(r.stats.max_ns) / 1e6);
        std::cout << measurement_quality_name(r.stats.quality);
        if (r.throughput_ops_per_sec) {
            std::cout << ' ' << std::fixed << std::setprecision(0) << *r.throughput_ops_per_sec << " ops/s";
        }
        if (r.compile_median_ns) {
            std::cout << " compile_ms=" << (static_cast<double>(*r.compile_median_ns) / 1e6);
        }
        if (r.memory_allocation_count) {
            std::cout << " alloc_count=" << *r.memory_allocation_count;
        }
        if (r.memory_allocation_bytes) {
            std::cout << " alloc_bytes=" << *r.memory_allocation_bytes;
        }
        std::cout << '\n';
    }
}

inline BenchOptions parse_args(int argc, char** argv) {
    BenchOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--json" && i + 1 < argc) {
            options.json_path = argv[++i];
        } else if (arg == "--warmup" && i + 1 < argc) {
            options.warmup = std::stoi(argv[++i]);
        } else if (arg == "--iterations" && i + 1 < argc) {
            options.iterations = std::stoi(argv[++i]);
        } else if (arg == "--filter" && i + 1 < argc) {
            options.filter_prefix = argv[++i];
        } else if (arg == "--jit-ic-dump") {
            options.jit_ic_dump = true;
        } else if (arg == "--jit-ic-profile") {
            options.jit_ic_profile = true;
        } else if (arg == "--continue-on-checksum-mismatch") {
            options.continue_on_checksum_mismatch = true;
        } else if (arg == "--help") {
            std::cout << "Usage: pscript_benchmark [--json path] [--warmup N] [--iterations N] [--filter prefix]"
                         " [--jit-ic-dump] [--jit-ic-profile] [--continue-on-checksum-mismatch]\n";
            std::exit(0);
        }
    }
    return options;
}

template <typename Fn>
SampleStats time_repeated(const BenchOptions& options, Fn fn) {
    for (int i = 0; i < options.warmup; ++i) {
        fn();
    }
    std::vector<std::int64_t> samples;
    samples.reserve(static_cast<std::size_t>(options.iterations));
    for (int i = 0; i < options.iterations; ++i) {
        const std::int64_t t0 = steady_now_ns();
        fn();
        samples.push_back(steady_now_ns() - t0);
    }
    return summarize_samples(std::move(samples));
}

}  // namespace pscript::bench
