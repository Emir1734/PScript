#include "benchmark/bench_workloads.h"
#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_runtime.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    using pscript::Bytecode;
    using pscript::ProfileDocument;
    using pscript::ProfileHotness;
    using pscript::Runtime;
    using pscript::Vm;
    using pscript::compile_with_builtins;

    std::string source(pscript::bench::kRuntimeClosureNested);
    if (argc > 1) {
        const std::string from = "bench(25000)";
        const std::string to = std::string("bench(") + argv[1] + ")";
        std::size_t pos = 0;
        while ((pos = source.find(from, pos)) != std::string::npos) {
            source.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    std::ostringstream vm_out;
    Runtime vm_rt(vm_out);
    Vm vm_only(compile_with_builtins(source), vm_rt);
    vm_only.set_jit_enabled(false);
    vm_only.run();

    ProfileDocument profile;
    profile.version = ProfileDocument::kFormatVersion;
    profile.pgo_compat_version = pscript::kPgoCompatVersion;

    Bytecode jit_bc = compile_with_builtins(source);
    profile.program_fingerprint = pscript::compute_program_fingerprint(jit_bc);
    for (std::uint32_t i = 0; i < jit_bc.functions.size(); ++i) {
        if (jit_bc.functions[i].name == "bench") {
            pscript::ProfileFunctionRecord fn;
            fn.function_index = i;
            fn.calls = 50'000;
            fn.hotness = ProfileHotness::Hot;
            profile.functions.push_back(fn);
            pscript::ProfileLoopRecord loop;
            loop.function_index = i;
            loop.ip = jit_bc.functions[i].entry;
            loop.iterations = 100'000;
            profile.loops.push_back(loop);
            break;
        }
    }
    pscript::apply_profile_guided_optimizations(jit_bc, profile);

    std::ostringstream jit_out;
    Runtime jit_rt(jit_out);
    Vm jit_vm(std::move(jit_bc), jit_rt);
    pscript::jit::JitTieringConfig cfg;
    cfg.baseline_invocation_threshold = 1;
    cfg.optimizing_invocation_threshold = 1;
    cfg.require_profile_for_optimizing = false;
    jit_vm.set_jit_enabled(true);
    jit_vm.set_jit_config(cfg);
    jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
    jit_vm.jit_engine().set_profile(&profile);
    jit_vm.run();

    auto norm = [](std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        return s;
    };
    if (norm(jit_out.str()) != norm(vm_out.str())) {
        std::cerr << "MISMATCH jit=" << jit_out.str() << " vm=" << vm_out.str();
        return 1;
    }
    std::cout << "OK " << jit_out.str();
    return 0;
}
