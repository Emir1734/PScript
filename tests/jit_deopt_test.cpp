#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_backend.h"
#include "jit/jit_compiler.h"
#include "jit/jit_deopt.h"
#include "jit/jit_ir.h"
#include "jit/jit_opt_profile.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

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

std::uint32_t function_index_by_name(const Bytecode& bytecode, const std::string& name) {
    for (std::uint32_t i = 0; i < bytecode.functions.size(); ++i) {
        if (bytecode.functions[i].name == name) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

ProfileDocument hot_integer_profile(const Bytecode& bc, const std::string& fn_name) {
    ProfileDocument doc;
    doc.version = ProfileDocument::kFormatVersion;
    doc.pgo_compat_version = pscript::kPgoCompatVersion;
    doc.program_fingerprint = pscript::compute_program_fingerprint(bc);
    const std::uint32_t idx = function_index_by_name(bc, fn_name);
    pscript::ProfileFunctionRecord fn;
    fn.function_index = idx;
    fn.calls = 20'000;
    fn.hotness = pscript::ProfileHotness::Hot;
    doc.functions.push_back(fn);
    pscript::ProfileLoopRecord loop;
    loop.function_index = idx;
    loop.ip = bc.functions[idx].entry;
    loop.iterations = 50'000;
    doc.loops.push_back(loop);
    return doc;
}

std::string run_program(const std::string& source, bool jit, const ProfileDocument* profile = nullptr) {
    Bytecode bc = compile_source(source);
    if (profile) {
        pscript::apply_profile_guided_optimizations(bc, *profile);
    }
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(std::move(bc), runtime);
    vm.set_jit_enabled(jit);
    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;
    config.optimizing_invocation_threshold = 2;
    config.require_profile_for_optimizing = true;
    config.deopt_recompile_invocation_cooldown = 4;
    vm.set_jit_config(config);
    if (profile) {
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(profile);
    }
    vm.run();
    return out.str();
}

}  // namespace

int main() {
    if (pscript::jit::jit_backend_available()) {
        const Bytecode bc = compile_source("func add_i(a, b):\n    return a + b\n");
        const std::uint32_t idx = function_index_by_name(bc, "add_i");
        ProfileDocument profile = hot_integer_profile(bc, "add_i");
        pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
        std::string error;
        auto compiled = pscript::jit::compile_function_optimizing(bc, idx, pscript::compute_program_fingerprint(bc),
                                                                  &opt, error);
        if (!compiled || !compiled->metadata || compiled->metadata->deopt.sites.empty()) {
            fail_test("deopt metadata", error.empty() ? std::string("missing deopt sites") : error);
        } else {
            pass_test("deopt metadata");
        }
        if (compiled) {
        pscript::jit::JitExecState state;
        state.bytecode = &bc;
        state.deopt_context = compiled->metadata ? &compiled->metadata->deopt : nullptr;
        const auto& fn = bc.functions[idx];
        state.registers.resize(fn.register_count);
        pscript::jit::refresh_register_data(state);
        state.locals.resize(fn.local_count);
        for (std::uint32_t i = 0; i < fn.local_count; ++i) {
            state.locals[i] = std::make_shared<pscript::Cell>();
        }
        state.locals[0]->value = Runtime::make_integer(3);
        state.locals[1]->value = Runtime::make_integer(4);
        pscript::jit::JitFunctionIr ir;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        state.instruction_count = static_cast<std::uint32_t>(ir.instructions.size());
        compiled->entry(&state);
        if (!state.completed || state.deopt_requested || state.return_value.integer != 7) {
            fail_test("integer guard success", "bad result");
        } else {
            pass_test("integer guard success");
        }

        state.completed = false;
        state.deopt_requested = false;
        state.current_insn = 0;
        state.locals[1]->value = Runtime::make_float(1.5);
        compiled->entry(&state);
        if (!state.deopt_requested || state.deopt_resume_bytecode_ip == 0) {
            fail_test("integer guard failure", "expected deopt with resume ip");
        } else {
            pass_test("integer guard failure");
        }
        }
    }

    {
        const std::string source = "func f(x):\n    return x + 1\nprint(f(5))\n";
        ProfileDocument profile = hot_integer_profile(compile_source(source), "f");
        const std::string vm_out = run_program(source, false);
        const std::string jit_out = run_program(source, true, &profile);
        if (vm_out != jit_out) {
            fail_test("semantic parity deopt", "vm=" + vm_out + " jit=" + jit_out);
        } else {
            pass_test("semantic parity deopt");
        }
    }

    {
        pscript::jit::JitEngine engine;
        pscript::jit::JitTieringConfig config;
        config.optimizing_invocation_threshold = 2;
        config.require_profile_for_optimizing = true;
        engine.set_config(config);
        const Bytecode bc = compile_source("func g():\n    return 1\n");
        const std::uint32_t idx = function_index_by_name(bc, "g");
        ProfileDocument profile = hot_integer_profile(bc, "g");
        Bytecode hot_bc = bc;
        pscript::apply_profile_guided_optimizations(hot_bc, profile);
        engine.bind_bytecode(&hot_bc, pscript::compute_program_fingerprint(hot_bc));
        engine.set_profile(&profile);
        const auto& fn = hot_bc.functions[idx];
        for (int i = 0; i < 3; ++i) {
            engine.record_invocation(idx, fn);
        }
        engine.compile_if_needed(idx, fn);
        const std::uint32_t gen1 = engine.optimization_generation(idx);
        engine.invalidate_optimizing_code(idx, pscript::jit::JitGuardKind::IntegerType);
        if (engine.lookup(idx) != nullptr) {
            fail_test("stale cache rejection", "invalid code still cached");
        } else {
            pass_test("stale cache rejection");
        }
        for (int i = 0; i < 80; ++i) {
            engine.record_invocation(idx, fn);
        }
        engine.compile_if_needed(idx, fn);
        if (engine.optimization_generation(idx) <= gen1) {
            fail_test("recompilation generation", "expected bumped generation");
        } else {
            pass_test("recompilation generation");
        }
    }

    {
        const std::string source =
            "func acc(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(acc(40))\n";
        ProfileDocument profile = hot_integer_profile(compile_source(source), "acc");
        const std::string vm_out = run_program(source, false, &profile);
        const std::string jit_out = run_program(source, true, &profile);
        if (vm_out != jit_out) {
            fail_test("loop acc vm jit parity", "mismatch");
        } else {
            pass_test("loop acc vm jit parity");
        }
    }

    {
        const std::string source =
            "func osr_sum(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(osr_sum(500))\n";
        ProfileDocument profile = hot_integer_profile(compile_source(source), "osr_sum");
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 2;
        config.require_profile_for_optimizing = true;
        config.osr_loop_trip_threshold = 8;
        const std::string vm_out = run_program(source, false, &profile);
        Bytecode bc = compile_source(source);
        pscript::apply_profile_guided_optimizations(bc, profile);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        vm.run();
        if (vm_out != out.str()) {
            fail_test("osr vm jit parity", "mismatch");
        } else {
            pass_test("osr vm jit parity");
        }
    }

    {
        pscript::jit::JitEngine engine;
        pscript::jit::JitTieringConfig config;
        config.deopt_recompile_invocation_cooldown = 100;
        config.optimizing_invocation_threshold = 2;
        engine.set_config(config);
        const Bytecode bc = compile_source("func h():\n    return 2\n");
        const std::uint32_t idx = function_index_by_name(bc, "h");
        ProfileDocument profile = hot_integer_profile(bc, "h");
        engine.bind_bytecode(&bc, pscript::compute_program_fingerprint(bc));
        engine.set_profile(&profile);
        const auto& fn = bc.functions[idx];
        for (int i = 0; i < 5; ++i) {
            engine.record_invocation(idx, fn);
        }
        engine.invalidate_optimizing_code(idx, pscript::jit::JitGuardKind::IntegerType);
        if (engine.qualifies_for_optimizing_tier(idx, fn)) {
            fail_test("deopt cooldown", "should block immediate re-opt");
        } else {
            pass_test("deopt cooldown");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All jit deopt tests passed\n";
    return 0;
}
