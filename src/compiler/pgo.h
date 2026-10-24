#pragma once

#include "compiler/bytecode.h"
#include "runtime/profiler.h"

#include <optional>
#include <string>
#include <vector>

namespace pscript {

constexpr int kPgoCompatVersion = 1;

struct PgoAnalysis {
    bool profile_usable = false;
    std::vector<ProfileHotness> function_hotness;
    std::vector<ProfileLoopRecord> hot_loops;
    std::vector<ProfileCallSiteRecord> hot_call_sites;
};

std::string compute_program_fingerprint(const Bytecode& bytecode);

bool profile_matches_bytecode(const ProfileDocument& profile, const Bytecode& bytecode, std::string& error);

std::optional<ProfileDocument> merge_profile_documents(const std::vector<ProfileDocument>& profiles,
                                                       std::string& error);

PgoAnalysis analyze_profile_for_pgo(const ProfileDocument& profile, const Bytecode& bytecode);

struct PgoOptimizationResult {
    bool applied = false;
    std::size_t removed_redundant_jumps = 0;
    std::size_t hot_functions = 0;
    std::size_t warm_functions = 0;
    std::size_t cold_functions = 0;
};

PgoOptimizationResult apply_profile_guided_optimizations(Bytecode& bytecode, const ProfileDocument& profile);

}  // namespace pscript
