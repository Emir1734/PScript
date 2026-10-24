#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "parser/parser.h"
#include "runtime/profiler.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::CompileOptions;
using pscript::ProfileDocument;
using pscript::Runtime;
using pscript::Vm;
using pscript::compile;
using pscript::parse;

namespace {

int g_failed = 0;

void fail_test(const char* name, const std::string& detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(const char* name) {
    std::cout << "PASS  " << name << '\n';
}

Bytecode compile_source(const std::string& source) {
    CompileOptions options;
    options.source_file = "<test>";
    return compile(parse(source), options);
}

ProfileDocument profile_for_bytecode(const Bytecode& bytecode, std::uint64_t insn_count) {
    ProfileDocument document;
    document.version = ProfileDocument::kFormatVersion;
    document.pgo_compat_version = pscript::kPgoCompatVersion;
    document.program_fingerprint = pscript::compute_program_fingerprint(bytecode);
    document.instruction_executions = insn_count;
    if (!bytecode.functions.empty()) {
        pscript::ProfileFunctionRecord fn;
        fn.function_index = 0;
        fn.name = bytecode.functions[0].name;
        fn.calls = 1;
        fn.inclusive_ns = 1'000'000;
        fn.exclusive_ns = 900'000;
        fn.hotness = pscript::ProfileHotness::Hot;
        document.functions.push_back(fn);
    }
    return document;
}

std::string run_program(const Bytecode& bytecode) {
    std::ostringstream output;
    Runtime runtime(output);
    Bytecode copy = bytecode;
    Vm vm(std::move(copy), runtime);
    vm.run();
    return output.str();
}

}  // namespace

int main() {
    const std::string source = "func add(a, b):\n    return a + b\nprint(add(2, 3))\n";
    const Bytecode baseline_bc = compile_source(source);
    const std::string baseline_out = run_program(baseline_bc);

    {
        ProfileDocument profile = profile_for_bytecode(baseline_bc, 100);
        std::string error;
        if (!pscript::profile_matches_bytecode(profile, baseline_bc, error)) {
            fail_test("valid profile", error);
        } else {
            pass_test("valid profile");
        }
    }

    {
        ProfileDocument profile = profile_for_bytecode(baseline_bc, 100);
        profile.program_fingerprint = "stale";
        std::string error;
        if (pscript::profile_matches_bytecode(profile, baseline_bc, error)) {
            fail_test("stale profile", "expected mismatch");
        } else {
            pass_test("stale profile rejected");
        }
    }

    {
        std::string error;
        const std::optional<ProfileDocument> loaded = pscript::load_profile_document("<missing>", error);
        if (loaded.has_value()) {
            fail_test("invalid profile load", "missing file should fail");
        } else {
            pass_test("invalid profile load");
        }
    }

    {
        ProfileDocument a = profile_for_bytecode(baseline_bc, 10);
        ProfileDocument b = profile_for_bytecode(baseline_bc, 20);
        std::string error;
        const std::optional<ProfileDocument> merged = pscript::merge_profile_documents({a, b}, error);
        if (!merged.has_value()) {
            fail_test("profile merge", error);
        } else if (merged->instruction_executions != 30) {
            fail_test("profile merge", "instruction totals mismatch");
        } else {
            pass_test("profile merge");
        }
    }

    {
        ProfileDocument profile = profile_for_bytecode(baseline_bc, 500);
        Bytecode optimized = baseline_bc;
        const pscript::PgoOptimizationResult result = pscript::apply_profile_guided_optimizations(optimized, profile);
        if (!result.applied || result.hot_functions == 0) {
            fail_test("pgo apply", "expected hot function metadata");
        } else {
            pass_test("pgo apply");
        }
        const std::string pgo_out = run_program(optimized);
        if (pgo_out != baseline_out) {
            fail_test("pgo semantics", "output changed after PGO");
        } else {
            pass_test("pgo semantics preserved");
        }
    }

    {
        ProfileDocument bad = profile_for_bytecode(baseline_bc, 1);
        bad.version = 999;
        Bytecode bc = baseline_bc;
        const pscript::PgoOptimizationResult result = pscript::apply_profile_guided_optimizations(bc, bad);
        if (result.applied) {
            fail_test("pgo fallback", "invalid version should not apply");
        } else {
            pass_test("pgo fallback on bad profile");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " PGO test(s) failed\n";
        return 1;
    }
    std::cout << "All PGO tests passed\n";
    return 0;
}
