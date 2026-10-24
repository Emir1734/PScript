#include "benchmark/bench_workloads.h"
#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_backend.h"
#include "jit/jit_compiler.h"
#include "jit/jit_opt_profile.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::ProfileDocument;
using pscript::ProfileHotness;
using pscript::Runtime;
using pscript::Vm;
using pscript::apply_profile_guided_optimizations;
using pscript::compile;
using pscript::parse;

namespace {

int g_failed = 0;

void fail_line(const char* name, const std::string& detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_line(const char* name) {
    std::cout << "PASS  " << name << '\n';
}

ProfileDocument bench_hot_profile(const Bytecode& bc) {
    ProfileDocument doc;
    doc.version = ProfileDocument::kFormatVersion;
    doc.pgo_compat_version = pscript::kPgoCompatVersion;
    doc.program_fingerprint = pscript::compute_program_fingerprint(bc);
    for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
        if (bc.functions[i].name == "bench") {
            pscript::ProfileFunctionRecord fn;
            fn.function_index = i;
            fn.calls = 50'000;
            fn.hotness = ProfileHotness::Hot;
            doc.functions.push_back(fn);
            break;
        }
    }
    return doc;
}

std::uint32_t fn_index(const Bytecode& bc, const char* name) {
    for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
        if (bc.functions[i].name == name) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

std::string run_inline_bench(bool jit, std::uint32_t bench_iters) {
    std::string source = std::string(pscript::bench::kRuntimeInlineLargeCallee);
    const std::string from = "print(bench(80000))";
    const std::string to = "print(bench(" + std::to_string(bench_iters) + "))";
    const std::size_t pos = source.find(from);
    if (pos != std::string::npos) {
        source.replace(pos, from.size(), to);
    }
    Bytecode prog = compile(parse(source));
    const ProfileDocument profile = bench_hot_profile(prog);
    apply_profile_guided_optimizations(prog, profile);
    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;
    config.optimizing_invocation_threshold = 1;
    config.require_profile_for_optimizing = false;
    std::ostringstream out;
    Runtime rt(out);
    Vm vm(std::move(prog), rt);
    if (jit) {
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
    } else {
        vm.set_jit_enabled(false);
    }
    vm.run();
    return out.str();
}

}  // namespace

int main() {
    Bytecode bc = compile(parse(std::string(pscript::bench::kRuntimeInlineLargeCallee)));
    const ProfileDocument profile = bench_hot_profile(bc);
    apply_profile_guided_optimizations(bc, profile);
    const pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
    const std::uint32_t heavy_idx = fn_index(bc, "heavy");
    if (heavy_idx == 0xFFFFFFFFu) {
        std::cerr << "heavy function missing\n";
        return 1;
    }

    std::string err;
    const auto heavy_compiled =
        pscript::jit::compile_function_optimizing(bc, heavy_idx, profile.program_fingerprint, &opt, err);
    if (!heavy_compiled) {
        std::cerr << "compile heavy failed: " << err << '\n';
        return 1;
    }
    if (!heavy_compiled->uses_dispatch_loop) {
        fail_line("large heavy uses dispatch emit",
                  "ir_insn=" + std::to_string(heavy_compiled->ir_instruction_count));
    } else {
        pass_line("large heavy uses dispatch emit");
    }

    const std::string vm_out = run_inline_bench(false, 12);
    const std::string jit_out = run_inline_bench(true, 12);
    if (vm_out != jit_out) {
        fail_line("large heavy nested bench(12) parity", "vm=" + vm_out + " jit=" + jit_out);
    } else {
        pass_line("large heavy nested bench(12) parity");
    }

    const std::string vm_osr = run_inline_bench(false, 1000);
    const std::string jit_osr = run_inline_bench(true, 1000);
    if (vm_osr != jit_osr) {
        fail_line("large heavy nested bench(1000) osr parity", "vm=" + vm_osr + " jit=" + jit_osr);
    } else {
        pass_line("large heavy nested bench(1000) osr parity");
    }

    const std::string vm_big = run_inline_bench(false, 80000);
    const std::string jit_big = run_inline_bench(true, 80000);
    if (vm_big != jit_big) {
        fail_line("large heavy nested bench(80000) parity", "jit=" + jit_big + " vm=" + vm_big);
    } else {
        pass_line("large heavy nested bench(80000) parity");
    }

    return g_failed == 0 ? 0 : 1;
}
