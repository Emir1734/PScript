#include "compiler/pgo.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>

namespace pscript {
namespace {

std::uint32_t function_end_ip(const Bytecode& bytecode, std::size_t function_index) {
    if (function_index + 1 < bytecode.functions.size()) {
        return bytecode.functions[function_index + 1].entry;
    }
    return static_cast<std::uint32_t>(bytecode.instructions.size());
}

void adjust_jump_targets(std::vector<Instruction>& instructions, std::uint32_t start, std::uint32_t end,
                         std::uint32_t removed_ip) {
    for (std::uint32_t ip = start; ip < end && ip < instructions.size(); ++ip) {
        Instruction& insn = instructions[ip];
        auto fix = [&](std::uint32_t& target) {
            if (target > removed_ip) {
                --target;
            }
        };
        switch (insn.opcode) {
            case Opcode::Jump:
                fix(insn.a);
                break;
            case Opcode::JumpIfFalse:
            case Opcode::JumpIfTrue:
                fix(insn.b);
                break;
            default:
                break;
        }
    }
}

}  // namespace

std::string compute_program_fingerprint(const Bytecode& bytecode) {
    std::ostringstream out;
    out << "pgo-v" << kPgoCompatVersion << '\n';
    out << bytecode.functions.size() << '\n';
    for (std::size_t i = 0; i < bytecode.functions.size(); ++i) {
        const FunctionInfo& fn = bytecode.functions[i];
        const std::uint32_t end = function_end_ip(bytecode, i);
        out << fn.name << ':' << fn.entry << ':' << (end - fn.entry) << ':' << fn.arity << '\n';
    }
    return out.str();
}

bool profile_matches_bytecode(const ProfileDocument& profile, const Bytecode& bytecode, std::string& error) {
    if (profile.version != ProfileDocument::kFormatVersion) {
        error = "unsupported profile version";
        return false;
    }
    if (profile.pgo_compat_version != 0 && profile.pgo_compat_version != kPgoCompatVersion) {
        error = "unsupported PGO compatibility version";
        return false;
    }
    if (profile.program_fingerprint.empty()) {
        error = "profile missing program fingerprint";
        return false;
    }
    const std::string fingerprint = compute_program_fingerprint(bytecode);
    if (profile.program_fingerprint != fingerprint) {
        error = "profile program fingerprint mismatch (stale profile)";
        return false;
    }
    for (const ProfileFunctionRecord& fn : profile.functions) {
        if (fn.function_index >= bytecode.functions.size()) {
            error = "profile references unknown function index";
            return false;
        }
        if (!fn.name.empty() && fn.name != bytecode.functions[fn.function_index].name) {
            error = "profile function name mismatch";
            return false;
        }
    }
    return true;
}

std::optional<ProfileDocument> merge_profile_documents(const std::vector<ProfileDocument>& profiles,
                                                       std::string& error) {
    if (profiles.empty()) {
        error = "merge requires at least one profile";
        return std::nullopt;
    }
    ProfileDocument merged = profiles.front();
    for (std::size_t i = 1; i < profiles.size(); ++i) {
        const ProfileDocument& next = profiles[i];
        if (next.version != merged.version) {
            error = "profile version mismatch during merge";
            return std::nullopt;
        }
        if (!next.program_fingerprint.empty() && !merged.program_fingerprint.empty() &&
            next.program_fingerprint != merged.program_fingerprint) {
            error = "program fingerprint mismatch during merge";
            return std::nullopt;
        }
        if (merged.program_fingerprint.empty()) {
            merged.program_fingerprint = next.program_fingerprint;
        }
        merged.instruction_executions += next.instruction_executions;
        merged.vm_wall_ns += next.vm_wall_ns;
        merged.memory.allocation_count += next.memory.allocation_count;
        merged.memory.allocation_bytes += next.memory.allocation_bytes;

        std::unordered_map<std::uint32_t, ProfileFunctionRecord> fn_map;
        for (ProfileFunctionRecord& fn : merged.functions) {
            fn_map[fn.function_index] = fn;
        }
        for (const ProfileFunctionRecord& fn : next.functions) {
            ProfileFunctionRecord& slot = fn_map[fn.function_index];
            if (slot.name.empty()) {
                slot.name = fn.name;
            }
            slot.function_index = fn.function_index;
            slot.calls += fn.calls;
            slot.inclusive_ns += fn.inclusive_ns;
            slot.exclusive_ns += fn.exclusive_ns;
        }
        merged.functions.clear();
        for (auto& entry : fn_map) {
            merged.functions.push_back(entry.second);
        }

        auto merge_counts = [](const std::vector<ProfileOpcodeRecord>& left,
                               const std::vector<ProfileOpcodeRecord>& right) {
            std::unordered_map<std::string, std::uint64_t> counts;
            for (const ProfileOpcodeRecord& op : left) {
                counts[op.name] += op.count;
            }
            for (const ProfileOpcodeRecord& op : right) {
                counts[op.name] += op.count;
            }
            std::vector<ProfileOpcodeRecord> out;
            for (const auto& entry : counts) {
                ProfileOpcodeRecord record;
                record.name = entry.first;
                record.count = entry.second;
                out.push_back(record);
            }
            return out;
        };
        merged.opcodes = merge_counts(merged.opcodes, next.opcodes);

        auto merge_loops = [](const std::vector<ProfileLoopRecord>& left, const std::vector<ProfileLoopRecord>& right) {
            std::unordered_map<std::uint64_t, std::uint64_t> counts;
            for (const ProfileLoopRecord& loop : left) {
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(loop.function_index) << 32) | static_cast<std::uint64_t>(loop.ip);
                counts[key] += loop.iterations;
            }
            for (const ProfileLoopRecord& loop : right) {
                const std::uint64_t key =
                    (static_cast<std::uint64_t>(loop.function_index) << 32) | static_cast<std::uint64_t>(loop.ip);
                counts[key] += loop.iterations;
            }
            std::vector<ProfileLoopRecord> out;
            for (const auto& entry : counts) {
                ProfileLoopRecord record;
                record.function_index = static_cast<std::uint32_t>(entry.first >> 32);
                record.ip = static_cast<std::uint32_t>(entry.first & 0xFFFFFFFFu);
                record.iterations = entry.second;
                out.push_back(record);
            }
            return out;
        };
        merged.loops = merge_loops(merged.loops, next.loops);

        auto merge_call_sites = [](const std::vector<ProfileCallSiteRecord>& left,
                                   const std::vector<ProfileCallSiteRecord>& right) {
            std::unordered_map<std::uint64_t, std::uint64_t> counts;
            for (const ProfileCallSiteRecord& site : left) {
                const std::uint64_t key = (static_cast<std::uint64_t>(site.caller_function_index) << 32) |
                                          static_cast<std::uint64_t>(site.ip);
                counts[key] += site.calls;
            }
            for (const ProfileCallSiteRecord& site : right) {
                const std::uint64_t key = (static_cast<std::uint64_t>(site.caller_function_index) << 32) |
                                          static_cast<std::uint64_t>(site.ip);
                counts[key] += site.calls;
            }
            std::vector<ProfileCallSiteRecord> out;
            for (const auto& entry : counts) {
                ProfileCallSiteRecord record;
                record.caller_function_index = static_cast<std::uint32_t>(entry.first >> 32);
                record.ip = static_cast<std::uint32_t>(entry.first & 0xFFFFFFFFu);
                record.calls = entry.second;
                out.push_back(record);
            }
            return out;
        };
        merged.call_sites = merge_call_sites(merged.call_sites, next.call_sites);
    }

    std::uint64_t total_inclusive = 0;
    for (const ProfileFunctionRecord& fn : merged.functions) {
        total_inclusive += fn.inclusive_ns;
    }
    if (total_inclusive == 0) {
        total_inclusive = merged.vm_wall_ns;
    }
    for (ProfileFunctionRecord& fn : merged.functions) {
        const double share =
            total_inclusive > 0 ? static_cast<double>(fn.inclusive_ns) / static_cast<double>(total_inclusive) : 0.0;
        fn.hotness = classify_hotness(share);
    }
    merged.pgo_compat_version = kPgoCompatVersion;
    return merged;
}

PgoAnalysis analyze_profile_for_pgo(const ProfileDocument& profile, const Bytecode& bytecode) {
    PgoAnalysis analysis;
    std::string error;
    if (!profile_matches_bytecode(profile, bytecode, error)) {
        return analysis;
    }
    analysis.profile_usable = true;
    analysis.function_hotness.assign(bytecode.functions.size(), ProfileHotness::Cold);
    for (const ProfileFunctionRecord& fn : profile.functions) {
        if (fn.function_index < analysis.function_hotness.size()) {
            analysis.function_hotness[fn.function_index] = fn.hotness;
        }
    }
    std::uint64_t max_loop = 0;
    for (const ProfileLoopRecord& loop : profile.loops) {
        max_loop = std::max(max_loop, loop.iterations);
    }
    const std::uint64_t hot_loop_threshold = max_loop > 0 ? max_loop / 10 : 0;
    for (const ProfileLoopRecord& loop : profile.loops) {
        if (loop.iterations >= hot_loop_threshold && loop.iterations > 0) {
            analysis.hot_loops.push_back(loop);
        }
    }
    std::uint64_t max_calls = 0;
    for (const ProfileCallSiteRecord& site : profile.call_sites) {
        max_calls = std::max(max_calls, site.calls);
    }
    const std::uint64_t hot_call_threshold = max_calls > 0 ? max_calls / 5 : 0;
    for (const ProfileCallSiteRecord& site : profile.call_sites) {
        if (site.calls >= hot_call_threshold && site.calls > 0) {
            analysis.hot_call_sites.push_back(site);
        }
    }
    return analysis;
}

PgoOptimizationResult apply_profile_guided_optimizations(Bytecode& bytecode, const ProfileDocument& profile) {
    PgoOptimizationResult result;
    std::string error;
    const PgoAnalysis analysis = analyze_profile_for_pgo(profile, bytecode);
    if (!analysis.profile_usable) {
        return result;
    }
    result.applied = true;

    for (std::size_t fn_index = 0; fn_index < bytecode.functions.size(); ++fn_index) {
        FunctionInfo& fn = bytecode.functions[fn_index];
        const ProfileHotness hotness =
            fn_index < analysis.function_hotness.size() ? analysis.function_hotness[fn_index] : ProfileHotness::Cold;
        fn.pgo_hotness = static_cast<std::uint8_t>(hotness);
        switch (hotness) {
            case ProfileHotness::Hot:
                ++result.hot_functions;
                break;
            case ProfileHotness::Warm:
                ++result.warm_functions;
                break;
            case ProfileHotness::Cold:
                ++result.cold_functions;
                break;
        }
        if (hotness == ProfileHotness::Cold) {
            continue;
        }

        const std::uint32_t start = fn.entry;
        const std::uint32_t end = function_end_ip(bytecode, fn_index);
        for (std::uint32_t ip = start; ip < end && ip < bytecode.instructions.size();) {
            if (bytecode.instructions[ip].opcode != Opcode::Jump) {
                ++ip;
                continue;
            }
            if (bytecode.instructions[ip].a != ip + 1) {
                ++ip;
                continue;
            }
            adjust_jump_targets(bytecode.instructions, start, end, ip);
            bytecode.instructions.erase(bytecode.instructions.begin() + static_cast<std::ptrdiff_t>(ip));
            if (ip < bytecode.instruction_locations.size()) {
                bytecode.instruction_locations.erase(
                    bytecode.instruction_locations.begin() + static_cast<std::ptrdiff_t>(ip));
            }
            for (std::size_t later = fn_index + 1; later < bytecode.functions.size(); ++later) {
                if (bytecode.functions[later].entry > ip) {
                    --bytecode.functions[later].entry;
                }
            }
            ++result.removed_redundant_jumps;
        }
    }
    return result;
}

}  // namespace pscript
