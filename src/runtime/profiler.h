#pragma once

#include "compiler/bytecode.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pscript {

constexpr std::size_t kProfileOpcodeSlotCount = static_cast<std::size_t>(Opcode::Halt) + 1;

enum class ProfileHotness {
    Cold,
    Warm,
    Hot,
};

struct ProfileFunctionRecord {
    std::uint32_t function_index = 0;
    std::string name;
    std::uint64_t calls = 0;
    std::uint64_t inclusive_ns = 0;
    std::uint64_t exclusive_ns = 0;
    ProfileHotness hotness = ProfileHotness::Cold;
};

struct ProfileOpcodeRecord {
    std::string name;
    std::uint64_t count = 0;
};

struct ProfileLoopRecord {
    std::uint32_t function_index = 0;
    std::uint32_t ip = 0;
    std::uint64_t iterations = 0;
};

struct ProfileCallSiteRecord {
    std::uint32_t caller_function_index = 0;
    std::uint32_t ip = 0;
    std::uint64_t calls = 0;
};

struct ProfileMemoryRecord {
    std::uint64_t allocation_count = 0;
    std::uint64_t allocation_bytes = 0;
};

struct ProfileDocument {
    static constexpr int kFormatVersion = 1;

    int version = kFormatVersion;
    int pgo_compat_version = 0;
    std::string program_fingerprint;
    std::uint64_t instruction_executions = 0;
    std::uint64_t vm_wall_ns = 0;
    ProfileMemoryRecord memory;
    std::vector<ProfileFunctionRecord> functions;
    std::vector<ProfileOpcodeRecord> opcodes;
    std::vector<ProfileLoopRecord> loops;
    std::vector<ProfileCallSiteRecord> call_sites;
};

const char* profile_hotness_name(ProfileHotness hotness);
ProfileHotness classify_hotness(double time_share);

class RuntimeProfiler {
public:
    void set_enabled(bool enabled);
    bool enabled() const { return enabled_; }

    void begin_session();
    void end_session();

    void on_instruction(Opcode opcode, std::uint32_t ip, std::uint32_t function_index);
    void on_function_enter(std::uint32_t function_index);
    void on_function_leave(std::uint32_t function_index);
    void on_call_site(std::uint32_t caller_function_index, std::uint32_t ip);
    void on_loop_back(std::uint32_t function_index, std::uint32_t target_ip);
    void on_allocation(std::size_t bytes);

    ProfileDocument finalize(const Bytecode& bytecode) const;

private:
    struct FunctionStats {
        std::uint64_t calls = 0;
        std::uint64_t inclusive_ns = 0;
        std::uint64_t exclusive_ns = 0;
    };

    struct ActiveFrame {
        std::uint32_t function_index = 0;
        std::chrono::steady_clock::time_point start{};
        std::uint64_t child_inclusive_ns = 0;
    };

    static std::uint64_t loop_key(std::uint32_t function_index, std::uint32_t ip);
    static std::uint64_t call_site_key(std::uint32_t function_index, std::uint32_t ip);

    void ensure_function_slot(std::uint32_t function_index);

    bool enabled_ = false;
    std::vector<FunctionStats> function_stats_;
    std::array<std::uint64_t, kProfileOpcodeSlotCount> opcode_counts_{};
    std::uint64_t instruction_executions_ = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> loop_iterations_;
    std::unordered_map<std::uint64_t, std::uint64_t> call_site_counts_;
    ProfileMemoryRecord memory_;
    std::vector<ActiveFrame> active_frames_;
    std::chrono::steady_clock::time_point session_start_{};
    std::uint64_t vm_wall_ns_ = 0;
};

class ProfilerThreadScope {
public:
    explicit ProfilerThreadScope(RuntimeProfiler* profiler);
    ~ProfilerThreadScope();

private:
    RuntimeProfiler* previous_ = nullptr;
};

RuntimeProfiler* thread_local_profiler();
void set_thread_local_profiler(RuntimeProfiler* profiler);

bool save_profile_document(const ProfileDocument& document, const std::string& path, std::string& error);
std::optional<ProfileDocument> load_profile_document(const std::string& path, std::string& error);
void write_profile_report(const ProfileDocument& document, std::ostream& out);

}  // namespace pscript
