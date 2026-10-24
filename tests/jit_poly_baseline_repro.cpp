#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_compiler.h"
#include "jit/jit_ir.h"
#include "jit/jit_opt.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include "benchmark/bench_workloads.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::Runtime;
using pscript::Vm;
using pscript::compile_with_builtins;

static std::uint32_t fn_index(const Bytecode& bc, const char* name) {
    for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
        if (bc.functions[i].name == name) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

int main(int argc, char** argv) {
    std::cerr << "repro_start\n" << std::flush;
    const int iterations = argc > 1 ? std::atoi(argv[1]) : 4000;
    const bool run_vm = argc <= 2 || std::atoi(argv[2]) != 0;
    std::string source = std::string(pscript::bench::kRuntimeCallValuePoly);
    if (iterations != 80000) {
        const std::string from = "hot(80000)";
        const std::string to = "hot(" + std::to_string(iterations) + ")";
        std::size_t pos = source.find(from);
        if (pos != std::string::npos) {
            source.replace(pos, from.size(), to);
        }
    }

    std::cerr << "compiling\n" << std::flush;
    Bytecode bc = compile_with_builtins(source);
    std::cerr << "compiled\n" << std::flush;
    const std::uint32_t hot = fn_index(bc, "hot");
    if (hot == 0xFFFFFFFFu) {
        std::cerr << "missing hot\n";
        return 2;
    }

    pscript::jit::JitFunctionIr ir;
    std::string ir_err;
    if (!pscript::jit::build_jit_ir_from_bytecode(bc, hot, ir, ir_err)) {
        std::cerr << "ir: " << ir_err << "\n";
        return 2;
    }
    pscript::jit::build_cfg_for_function(ir);
    std::size_t cv_in_loop = 0;
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        if (ir.instructions[i].opcode == pscript::jit::JitIrOpcode::CallValue &&
            pscript::jit::instruction_in_backedge_loop(ir, i)) {
            ++cv_in_loop;
        }
    }
    std::cout << "callvalue_in_loop=" << cv_in_loop << " ir_insns=" << ir.instructions.size() << "\n";

    std::string prep_err;
    pscript::jit::OptimizingPassStats stats;
    bool conservative_dispatch = false;
    const bool stable_direct = pscript::jit::ir_callvalue_sites_are_stable_direct(bc, ir);
    pscript::jit::JitFunctionIr prep_ir;
    if (!pscript::jit::build_jit_ir_from_bytecode(bc, hot, prep_ir, prep_err)) {
        std::cerr << prep_err << "\n";
        return 2;
    }
    if (!pscript::jit::prepare_function_ir_for_jit_compile(bc, prep_ir, nullptr, stable_direct, stats, prep_err,
                                                           conservative_dispatch)) {
        std::cerr << "prepare: " << prep_err << "\n";
        return 2;
    }
    std::cout << "stable_direct=" << stable_direct << " conservative_dispatch=" << conservative_dispatch
              << " ir.conservative_dispatch_emit=" << prep_ir.conservative_dispatch_emit
              << " prep_callvalue_sites=" << (pscript::jit::ir_has_call_value_sites(prep_ir) ? 1 : 0)
              << " prep_call_sites=" << (pscript::jit::ir_has_call_sites(prep_ir) ? 1 : 0)
              << " prep_insns=" << prep_ir.instructions.size() << "\n";

    const std::string fp = pscript::compute_program_fingerprint(bc);
    std::string compile_err;
    auto compiled = pscript::jit::compile_function_baseline(bc, hot, fp, nullptr, compile_err);
    if (!compiled) {
        std::cerr << "compile: " << compile_err << "\n";
        return 2;
    }
    std::cout << "dispatch_loop=" << compiled->uses_dispatch_loop << " code_bytes=" << compiled->code_size
              << " ir_insn=" << compiled->ir_instruction_count << "\n";

    std::ostringstream out;
    Runtime rt(out);
    Vm vm(std::move(bc), rt);
    vm.set_jit_enabled(true);
    pscript::jit::JitTieringConfig cfg;
    cfg.baseline_invocation_threshold = 1;
    cfg.optimizing_invocation_threshold = 1'000'000;
    cfg.require_profile_for_optimizing = true;
    vm.set_jit_config(cfg);
    vm.jit_engine().bind_bytecode(&vm.bytecode(), fp);

    if (!run_vm) {
        std::cout << "compile_only_ok\n";
        return 0;
    }
    std::cerr << "vm_run\n" << std::flush;
    const auto t0 = std::chrono::steady_clock::now();
    vm.run();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "elapsed_ms=" << ms << " output=" << out.str();
    return ms > 30'000 ? 1 : 0;
}
