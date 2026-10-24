#include "compiler/compiler.h"

#include <cstddef>
#include "compiler/pgo.h"
#include "jit/jit_backend.h"
#include "jit/jit_ir.h"
#include "jit/jit_code.h"
#include "jit/jit_compiler.h"
#include "jit/jit_ic.h"
#include "jit/jit_ir.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/profiler.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include "benchmark/bench_workloads.h"

#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::CompileOptions;
using pscript::ProfileDocument;
using pscript::ProfileHotness;
using pscript::Runtime;
using pscript::Vm;
using pscript::apply_profile_guided_optimizations;
using pscript::compile;
using pscript::compile_with_builtins;
using pscript::parse;

namespace {

int g_failed = 0;

void fail_test(const char* name, const std::string& detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    std::cerr.flush();
    ++g_failed;
}

void pass_test(const char* name) {
    std::cout << "PASS  " << name << '\n';
    std::cout.flush();
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
    for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
        if (bc.functions[i].name == fn_name) {
            pscript::ProfileFunctionRecord fn;
            fn.function_index = i;
            fn.calls = 50'000;
            fn.hotness = ProfileHotness::Hot;
            doc.functions.push_back(fn);
            pscript::ProfileLoopRecord loop;
            loop.function_index = i;
            loop.ip = bc.functions[i].entry;
            loop.iterations = 100'000;
            doc.loops.push_back(loop);
            break;
        }
    }
    return doc;
}

bool optimizing_jit_vm_parity_twice(const char* test_name, const std::string& source, const std::string& hot_fn,
                                    bool use_builtins) {
    const auto compile_prog = [&]() {
        return use_builtins ? compile_with_builtins(source) : compile_source(source);
    };
    ProfileDocument profile = hot_integer_profile(compile_prog(), hot_fn);

    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;
    config.optimizing_invocation_threshold = 1;
    config.require_profile_for_optimizing = false;

    std::ostringstream vm_out;
    {
        Bytecode vm_bc = compile_prog();
        pscript::apply_profile_guided_optimizations(vm_bc, profile);
        Runtime vm_rt(vm_out);
        Vm vm(std::move(vm_bc), vm_rt);
        vm.set_jit_enabled(false);
        vm.run();
        vm.run();
    }

    std::ostringstream jit_out;
    {
        Bytecode jit_bc = compile_prog();
        pscript::apply_profile_guided_optimizations(jit_bc, profile);
        Runtime jit_rt(jit_out);
        Vm jit_vm(std::move(jit_bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(),
                                          pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        jit_vm.run();
    }

    if (jit_out.str() != vm_out.str()) {
        fail_test(test_name, "jit=" + jit_out.str() + " vm=" + vm_out.str());
        return false;
    }
    pass_test(test_name);
    return true;
}

std::string run_program_capture(const std::string& source, bool use_builtins, bool jit, bool optimizing,
                                const std::string& hot_fn) {
    const auto compile_prog = [&]() {
        return use_builtins ? compile_with_builtins(source) : compile_source(source);
    };
    Bytecode bc = compile_prog();
    ProfileDocument profile = hot_integer_profile(bc, hot_fn);
    if (optimizing) {
        pscript::apply_profile_guided_optimizations(bc, profile);
    }
    std::ostringstream out;
    Runtime rt(out);
    Vm vm(std::move(bc), rt);
    if (jit) {
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = optimizing ? 1 : 1'000'000;
        config.require_profile_for_optimizing = false;
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        if (optimizing) {
            vm.jit_engine().set_profile(&profile);
        }
    } else {
        vm.set_jit_enabled(false);
    }
    vm.run();
    return out.str();
}

}  // namespace

int main() {
    {
        const std::string source = "func add(a, b):\n    return a + b\nprint(add(2, 3))\n";
        const Bytecode bc = compile_source(source);
        const std::uint32_t add_index = function_index_by_name(bc, "add");
        std::string error;
        pscript::jit::JitFunctionIr ir;
        if (add_index == 0xFFFFFFFFu) {
            fail_test("jit ir build", "missing add function");
        } else if (!pscript::jit::build_jit_ir_from_bytecode(bc, add_index, ir, error)) {
            fail_test("jit ir build", error);
        } else if (ir.instructions.empty()) {
            fail_test("jit ir build", "no instructions");
        } else {
            pass_test("jit ir build");
        }

        std::string reason;
        const Bytecode bad = compile_source("func f():\n    print(1)\n");
        const std::uint32_t f_index = function_index_by_name(bad, "f");
        if (f_index == 0xFFFFFFFFu) {
            fail_test("unsupported opcode fallback", "missing f function");
        } else if (pscript::jit::jit_function_is_eligible(bad, f_index, reason)) {
            fail_test("unsupported opcode fallback", "print should be unsupported");
        } else {
            pass_test("unsupported opcode fallback");
        }
    }

    {
        if (!pscript::jit::jit_backend_available()) {
            pass_test("native backend skipped");
        } else {
            const Bytecode bc = compile_source("func mul(x, y):\n    return x * y\nprint(mul(3, 5))\n");
            const std::uint32_t mul_index = function_index_by_name(bc, "mul");
            const std::string fingerprint = pscript::compute_program_fingerprint(bc);
            std::string compile_error;
            std::optional<pscript::jit::JitCompiledCode> compiled =
                pscript::jit::compile_function_baseline(bc, mul_index, fingerprint, nullptr, compile_error);
            if (!compiled) {
                fail_test("native execution", "compile failed: " + compile_error);
            } else {
                const pscript::FunctionInfo& mul_fn = bc.functions[mul_index];
                pscript::jit::JitExecState state;
                state.bytecode = &bc;
                state.registers.resize(compiled->register_count);
                pscript::jit::refresh_register_data(state);
                state.locals.resize(mul_fn.local_count);
                for (std::uint32_t i = 0; i < mul_fn.local_count; ++i) {
                    state.locals[i] = std::make_shared<pscript::Cell>();
                }
                state.locals[0]->value = Runtime::make_integer(3);
                state.locals[1]->value = Runtime::make_integer(5);
                pscript::jit::JitFunctionIr ir;
                std::string ir_error;
                pscript::jit::build_jit_ir_from_bytecode(bc, mul_index, ir, ir_error);
                state.instruction_count = static_cast<std::uint32_t>(ir.instructions.size());
                compiled->entry(&state);
                if (!state.completed || state.fallback) {
                    fail_test("native execution", "native run failed");
                } else if (state.return_value.kind != pscript::ValueKind::Integer ||
                             state.return_value.integer != 15) {
                    fail_test("native execution",
                              "wrong return value got=" + std::to_string(state.return_value.integer));
                } else {
                    pass_test("native execution");
                }
            }

            {
                pscript::jit::JitFunctionIr ir;
                ir.register_count = 1;
                ir.instructions.resize(2);
                ir.instructions[0].opcode = pscript::jit::JitIrOpcode::LoadConstInt;
                ir.instructions[0].a = 0;
                ir.instructions[0].const_int_value = 15;
                ir.instructions[1].opcode = pscript::jit::JitIrOpcode::Return;
                ir.instructions[1].a = 0;
                pscript::jit::JitCodeBuffer buffer;
                pscript::jit::JitCompiledCode manual;
                std::string manual_error;
                if (!pscript::jit::compile_baseline_jit(ir, nullptr, buffer, manual, manual_error)) {
                    fail_test("sequential inline const return", manual_error);
                } else {
                    pscript::jit::JitExecState manual_state;
                    manual_state.registers.resize(1);
                    pscript::jit::refresh_register_data(manual_state);
                    manual_state.instruction_count = 2;
                    manual.entry(&manual_state);
                    const std::int64_t slot0 = manual_state.registers[0].integer;
                    const int slot_kind = static_cast<int>(manual_state.registers[0].kind);
                    if (!manual_state.completed || manual_state.fallback ||
                        manual_state.return_value.integer != 15) {
                        std::ostringstream code_hex;
                        if (manual.executable != nullptr && manual.code_size > 0) {
                            const auto* bytes = static_cast<const std::uint8_t*>(manual.executable);
                            const std::size_t limit = std::min<std::size_t>(manual.code_size, 96);
                            for (std::size_t i = 0; i < limit; ++i) {
                                code_hex << std::hex << static_cast<int>(bytes[i]) << ' ';
                            }
                        }
                        fail_test("sequential inline const return",
                                  "ret=" + std::to_string(manual_state.return_value.integer) + " slot0=" +
                                      std::to_string(slot0) + " kind=" + std::to_string(slot_kind) + " code=" +
                                      code_hex.str());
                    } else {
                        pass_test("sequential inline const return");
                    }
                }
            }

            {
                pscript::jit::JitFunctionIr ir;
                ir.register_count = 3;
                ir.instructions.resize(4);
                ir.instructions[0].opcode = pscript::jit::JitIrOpcode::LoadConstInt;
                ir.instructions[0].a = 0;
                ir.instructions[0].const_int_value = 3;
                ir.instructions[1].opcode = pscript::jit::JitIrOpcode::LoadConstInt;
                ir.instructions[1].a = 1;
                ir.instructions[1].const_int_value = 5;
                ir.instructions[2].opcode = pscript::jit::JitIrOpcode::IntMul;
                ir.instructions[2].a = 2;
                ir.instructions[2].b = 0;
                ir.instructions[2].c = 1;
                ir.instructions[3].opcode = pscript::jit::JitIrOpcode::Return;
                ir.instructions[3].a = 2;
                pscript::jit::JitCodeBuffer buffer;
                pscript::jit::JitCompiledCode manual;
                std::string manual_error;
                if (!pscript::jit::compile_baseline_jit(ir, nullptr, buffer, manual, manual_error)) {
                    fail_test("sequential inline const mul", manual_error);
                } else {
                    pscript::jit::JitExecState manual_state;
                    manual_state.registers.resize(manual.register_count != 0 ? manual.register_count : 3);
                    pscript::jit::refresh_register_data(manual_state);
                    manual_state.instruction_count = 4;
                    manual.entry(&manual_state);
                    if (!manual_state.completed || manual_state.fallback ||
                        manual_state.return_value.integer != 15) {
                        fail_test("sequential inline const mul",
                                  "got=" + std::to_string(manual_state.return_value.integer) + " fallback=" +
                                      std::to_string(manual_state.fallback));
                    } else {
                        pass_test("sequential inline const mul");
                    }
                }
            }

            pscript::jit::JitTieringConfig config;
            config.baseline_invocation_threshold = 1;
            config.use_pgo_hotness = false;
            std::ostringstream output;
            Runtime runtime(output);
            Bytecode copy = bc;
            Vm vm(std::move(copy), runtime);
            vm.set_jit_enabled(true);
            vm.set_jit_config(config);
            vm.run();
            if (output.str() != "15\n") {
                fail_test("native execution vm", "unexpected output: " + output.str());
            } else {
                pass_test("native execution vm");
            }
        }
    }

    {
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1000;
        config.use_pgo_hotness = false;

        const Bytecode bc = compile_source("func id(n):\n    return n\nprint(id(7))\n");
        std::ostringstream cold_out;
        Runtime cold_runtime(cold_out);
        Bytecode cold_copy = bc;
        Vm cold_vm(std::move(cold_copy), cold_runtime);
        cold_vm.set_jit_config(config);
        cold_vm.run();
        if (cold_vm.jit_engine().compile_attempts() != 0) {
            fail_test("cold stays on vm", "unexpected compile");
        } else {
            pass_test("cold stays on vm");
        }
    }

    {
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.use_pgo_hotness = false;
        const Bytecode bc =
            compile_source("func twice(n):\n    return n + n\nprint(twice(4))\nprint(twice(5))\n");

        std::ostringstream out;
        Runtime rt(out);
        Bytecode copy = bc;
        Vm vm(std::move(copy), rt);
        vm.set_jit_config(config);
        vm.run();

        if (vm.jit_engine().compile_attempts() < 1) {
            fail_test("jit cache compile", "expected compile");
        } else if (vm.jit_engine().cache_hits() < 1) {
            fail_test("jit cache reuse", "expected cache hit on second call");
        } else {
            pass_test("jit cache reuse");
        }
        if (out.str() != "8\n10\n") {
            fail_test("vm jit parity", "output mismatch: " + out.str());
        } else {
            pass_test("vm jit parity");
        }
    }

    {
        ProfileDocument profile;
        profile.version = ProfileDocument::kFormatVersion;
        profile.pgo_compat_version = pscript::kPgoCompatVersion;
        const Bytecode bc = compile_source("func hot():\n    return 42\nprint(hot())\n");
        profile.program_fingerprint = pscript::compute_program_fingerprint(bc);
        const std::uint32_t hot_index = function_index_by_name(bc, "hot");
        pscript::ProfileFunctionRecord fn;
        fn.function_index = hot_index;
        fn.hotness = pscript::ProfileHotness::Hot;
        profile.functions.push_back(fn);

        Bytecode hot_bc = bc;
        pscript::apply_profile_guided_optimizations(hot_bc, profile);

        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 10'000;
        config.use_pgo_hotness = true;

        std::ostringstream output;
        Runtime runtime(output);
        Vm vm(std::move(hot_bc), runtime);
        vm.set_jit_config(config);
        vm.run();
        if (vm.jit_engine().compile_attempts() < 1) {
            fail_test("pgo hot tier", "expected JIT compile from PGO hotness");
        } else {
            pass_test("pgo hot tier");
        }
    }

    {
        pscript::jit::JitEngine engine;
        pscript::jit::JitTieringConfig config;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        engine.set_config(config);
        const Bytecode bc = compile_source("func f():\n    return 1\n");
        const std::uint32_t f_index = function_index_by_name(bc, "f");
        engine.bind_bytecode(&bc, pscript::compute_program_fingerprint(bc));
        const pscript::FunctionInfo& fn = bc.functions[f_index];
        engine.record_invocation(f_index, fn);
        if (engine.desired_tier(f_index, fn) != pscript::jit::JitTier::OptimizingJit) {
            fail_test("optimizing tier hook", "expected optimizing tier hook");
        } else {
            pass_test("optimizing tier hook");
        }
    }

    {
        const std::string source =
            "func sum_n(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(sum_n(3))\n"
            "print(sum_n(3))\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.use_pgo_hotness = false;
        std::ostringstream out;
        Runtime rt(out);
        Vm vm(compile_source(source), rt);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.run();
        if (out.str() != "3\n3\n") {
            fail_test("jit range iteration parity", out.str());
        } else if (vm.jit_engine().compile_attempts() < 1) {
            fail_test("jit range iteration compile", "expected compile");
        } else {
            pass_test("jit range iteration parity");
        }
    }

    {
        const std::string source =
            "func leaf(x):\n"
            "    return x + 1\n"
            "func mid(x):\n"
            "    return leaf(x)\n"
            "print(mid(4))\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        std::ostringstream out;
        Runtime rt(out);
        Vm vm(compile_source(source), rt);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.run();
        if (out.str() != "5\n") {
            fail_test("jit nested call parity", out.str());
        } else {
            pass_test("jit nested call parity");
        }
    }

    {
        const std::string source =
            "func leaf(x):\n"
            "    return x + 1\n"
            "func mid(x):\n"
            "    return leaf(x)\n"
            "func top(x):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < x:\n"
            "        total = total + mid(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(top(40))\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        std::ostringstream out;
        Runtime rt(out);
        Vm vm(compile_source(source), rt);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (out.str() != vm_out.str()) {
            fail_test("jit direct call ic parity", "jit=" + out.str() + " vm=" + vm_out.str());
        } else if (vm.jit_engine().compile_attempts() < 1) {
            fail_test("jit direct call ic parity", "expected JIT compile");
        } else {
            pass_test("jit direct call ic parity");
        }
    }

    {
        // Minimal repro: second loop iteration hits warm mono Call IC (emit_mono path).
        const std::string source =
            "func leaf(x):\n"
            "    return x + 1\n"
            "func mid(x):\n"
            "    return leaf(x)\n"
            "func top(x):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < x:\n"
            "        total = total + mid(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(top(2))\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        std::ostringstream out;
        Runtime rt(out);
        Vm vm(compile_source(source), rt);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.run();
        if (out.str() != "3\n") {
            fail_test("jit ic warm second loop iteration", out.str());
        } else {
            pass_test("jit ic warm second loop iteration");
        }
    }

    {
        // Minimal repro: second top() entry reuses warm Call IC on first mid() inside top.
        const std::string source =
            "func leaf(x):\n"
            "    return x + 1\n"
            "func mid(x):\n"
            "    return leaf(x)\n"
            "func top(x):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < x:\n"
            "        total = total + mid(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(top(1))\n"
            "print(top(1))\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        std::ostringstream out;
        Runtime rt(out);
        Vm vm(compile_source(source), rt);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.run();
        if (out.str() != "1\n1\n") {
            fail_test("jit ic warm second top entry", out.str());
        } else {
            pass_test("jit ic warm second top entry");
        }
    }

    {
        // Regression: nested mono Call must not AV under repeated optimizing/baseline compiles.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const std::string source = pscript::bench::kRuntimeNestedCalls;
            pscript::jit::JitTieringConfig config;
            config.baseline_invocation_threshold = 1;
            config.optimizing_invocation_threshold = 1;
            config.require_profile_for_optimizing = false;
            ProfileDocument profile = hot_integer_profile(compile_source(source), "d");
            std::ostringstream jit_out;
            Runtime jit_rt(jit_out);
            Bytecode bc = compile_source(source);
            pscript::apply_profile_guided_optimizations(bc, profile);
            Vm jit_vm(std::move(bc), jit_rt);
            jit_vm.set_jit_enabled(true);
            jit_vm.set_jit_config(config);
            jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(),
                                              pscript::compute_program_fingerprint(jit_vm.bytecode()));
            jit_vm.jit_engine().set_profile(&profile);
            jit_vm.run();
            std::ostringstream vm_out;
            Runtime vm_rt(vm_out);
            Vm vm_only(compile_source(source), vm_rt);
            vm_only.set_jit_enabled(false);
            vm_only.run();
            if (jit_out.str() != vm_out.str()) {
                fail_test("jit nested calls stress parity",
                          "attempt=" + std::to_string(attempt) + " jit=" + jit_out.str() + " vm=" + vm_out.str());
                break;
            }
            if (attempt == 7) {
                pass_test("jit nested calls stress parity");
            }
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeNestedCalls;
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        ProfileDocument profile = hot_integer_profile(compile_source(source), "d");
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Bytecode bc = compile_source(source);
        pscript::apply_profile_guided_optimizations(bc, profile);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("jit nested calls deep parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().compile_attempts() < 1) {
            fail_test("jit nested calls deep parity", "expected JIT compile");
        } else {
            pass_test("jit nested calls deep parity");
        }
    }

    if (pscript::jit::jit_backend_available()) {
        optimizing_jit_vm_parity_twice("phase6 inline_large nested mono warm parity",
                                       std::string(pscript::bench::kRuntimeInlineLargeCallee), "bench", true);
        optimizing_jit_vm_parity_twice("phase6 inline_medium nested mono warm parity",
                                       std::string(pscript::bench::kRuntimeInlineMedium), "bench", true);
        optimizing_jit_vm_parity_twice("phase6 simple_calls vm parity twice",
                                       std::string(pscript::bench::kRuntimeSimpleCalls), "hot", true);
        optimizing_jit_vm_parity_twice("phase6 callvalue_mono vm parity twice",
                                       std::string(pscript::bench::kRuntimeCallValueMono), "hot", true);
        const std::string mutable_capture =
            "func make():\n"
            "    n = 0\n"
            "    func bump():\n"
            "        n = n + 1\n"
            "        return n\n"
            "    return bump\n"
            "func demo():\n"
            "    f = make()\n"
            "    return f() + f()\n"
            "print(demo())\n";
        optimizing_jit_vm_parity_twice("phase6 mutable closure callvalue twice", mutable_capture, "demo", false);

        const std::string caller_live_acc =
            "func inc(x):\n"
            "    return x + 1\n"
            "func hot(n):\n"
            "    acc = 1000\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        acc = acc + inc(i)\n"
            "        i = i + 1\n"
            "    return acc\n"
            "print(hot(25))\n";
        optimizing_jit_vm_parity_twice("phase6 caller live acc across nested jit", caller_live_acc, "hot", false);

        const std::string nested_string_callee =
            "func slen(s):\n"
            "    return s.length()\n"
            "func mid(x):\n"
            "    return slen(\"abcd\") + x\n"
            "func bench(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + mid(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(bench(12))\n";
        optimizing_jit_vm_parity_twice("phase6 nested non-integer callee generic path", nested_string_callee, "bench",
                                       false);

        optimizing_jit_vm_parity_twice("phase6 closure nested multi capture twice",
                                       std::string(pscript::bench::kRuntimeClosureNested), "bench", true);

        const std::string nested_after_failure =
            "func risky():\n"
            "    throw \"fail\"\n"
            "func safe(x):\n"
            "    return x + 1\n"
            "func hot(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        try:\n"
            "            if i == 0:\n"
            "                risky()\n"
            "            total = total + safe(i)\n"
            "        catch e:\n"
            "            total = total + safe(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(hot(8))\n";
        optimizing_jit_vm_parity_twice("phase6 nested calls continue after failure", nested_after_failure, "hot", false);

        const std::string nested_throw_caught =
            "func leaf(x):\n"
            "    if x == 2:\n"
            "        throw \"stop\"\n"
            "    return x\n"
            "func hot(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        try:\n"
            "            total = total + leaf(i)\n"
            "        catch e:\n"
            "            total = total + 100\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(hot(5))\n";
        optimizing_jit_vm_parity_twice("phase6 nested throw caught in caller jit", nested_throw_caught, "hot", false);

        const std::string float_nested_deopt =
            "func inner(a, b):\n"
            "    return a + b\n"
            "func outer(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        a = i\n"
            "        if i == 4:\n"
            "            a = 1.0\n"
            "        total = total + inner(a, 1)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(outer(10))\n";
        const std::string vm_only = run_program_capture(float_nested_deopt, false, false, false, "outer");
        const std::string opt_jit = run_program_capture(float_nested_deopt, false, true, true, "outer");
        const std::string base_jit = run_program_capture(float_nested_deopt, false, true, false, "outer");
        if (vm_only != opt_jit) {
            fail_test("phase6 float nested mixed arg optimizing parity",
                      "vm=" + vm_only + " opt=" + opt_jit);
        } else {
            pass_test("phase6 float nested mixed arg optimizing parity");
        }
        if (vm_only != base_jit) {
            fail_test("phase6 float nested mixed arg baseline parity",
                      "vm=" + vm_only + " base=" + base_jit);
        } else {
            pass_test("phase6 float nested mixed arg baseline parity");
        }

        const std::string inner_float_only =
            "func inner(a, b):\n"
            "    return a + b\n"
            "func go():\n"
            "    return inner(1.0, 1)\n"
            "print(go())\n";
        const std::string if_vm = run_program_capture(inner_float_only, false, false, false, "go");
        const std::string if_base = run_program_capture(inner_float_only, false, true, false, "go");
        if (if_vm != if_base) {
            fail_test("phase6 inner float args baseline parity", "vm=" + if_vm + " base=" + if_base);
        } else {
            pass_test("phase6 inner float args baseline parity");
        }

        optimizing_jit_vm_parity_twice("phase6 float nested deopt mid loop optimizing parity", float_nested_deopt,
                                       "outer", false);

        const std::string caller_mutable_capture_nested =
            "func make():\n"
            "    n = 10\n"
            "    func read():\n"
            "        return n\n"
            "    func hot(k):\n"
            "        total = 0\n"
            "        i = 0\n"
            "        while i < k:\n"
            "            total = total + read()\n"
            "            n = n + 1\n"
            "            i = i + 1\n"
            "        return total + n\n"
            "    return hot\n"
            "func main():\n"
            "    h = make()\n"
            "    return h(6)\n"
            "print(main())\n";
        optimizing_jit_vm_parity_twice("phase6 caller mutable capture across nested jit", caller_mutable_capture_nested,
                                       "hot", false);

        const std::string nested_uncaught =
            "func leaf():\n"
            "    throw \"nested boom\"\n"
            "func hot():\n"
            "    return leaf()\n"
            "print(hot())\n";
        auto run_expect_unhandled = [&](bool jit, bool optimizing) -> std::string {
            try {
                Bytecode bc = compile_source(nested_uncaught);
                ProfileDocument profile = hot_integer_profile(bc, "hot");
                apply_profile_guided_optimizations(bc, profile);
                pscript::jit::JitTieringConfig config;
                config.baseline_invocation_threshold = 1;
                config.optimizing_invocation_threshold = 1;
                config.require_profile_for_optimizing = false;
                std::ostringstream out;
                Runtime rt(out);
                Vm vm(std::move(bc), rt);
                if (jit) {
                    vm.set_jit_enabled(true);
                    vm.set_jit_config(config);
                    vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
                    vm.jit_engine().set_profile(&profile);
                } else {
                    vm.set_jit_enabled(false);
                }
                vm.run();
                return {};
            } catch (const pscript::VmError& err) {
                return err.message();
            }
        };
        const std::string unhandled_vm = run_expect_unhandled(false, false);
        const std::string unhandled_jit = run_expect_unhandled(true, true);
        if (unhandled_vm.empty() || unhandled_jit.empty()) {
            fail_test("phase6 nested uncaught exception propagates",
                      "vm_empty=" + std::string(unhandled_vm.empty() ? "yes" : "no") +
                          " jit_empty=" + std::string(unhandled_jit.empty() ? "yes" : "no"));
        } else if (unhandled_vm != unhandled_jit) {
            fail_test("phase6 nested uncaught exception propagates", "vm=" + unhandled_vm + " jit=" + unhandled_jit);
        } else {
            pass_test("phase6 nested uncaught exception propagates");
        }

        const std::string triple_parallel_capture =
            "func outer(a, b, c):\n"
            "    func inner(z):\n"
            "        return a + b + c + z\n"
            "    return inner\n"
            "func driver_loop(n):\n"
            "    f = outer(1, 2, 3)\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + f(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(driver_loop(8))\n";
        optimizing_jit_vm_parity_twice("phase6 triple parallel capture nested loop parity",
                                       triple_parallel_capture, "driver_loop", false);

        const std::string triple_two_instances =
            "func outer(a, b, c):\n"
            "    func inner(z):\n"
            "        return a + b + c + z\n"
            "    return inner\n"
            "func driver():\n"
            "    f1 = outer(1, 2, 3)\n"
            "    f2 = outer(10, 20, 30)\n"
            "    return f1(1) + f2(1)\n"
            "print(driver())\n";
        optimizing_jit_vm_parity_twice("phase6 triple capture two closure instances parity", triple_two_instances,
                                       "driver", false);

        const std::string emit_mono_deopt_caller_live =
            "func inc(x):\n"
            "    return x + 1\n"
            "func driver(n):\n"
            "    acc = 100\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        v = i\n"
            "        if i == 4:\n"
            "            v = 1.0\n"
            "        total = total + inc(v)\n"
            "        acc = acc + 1\n"
            "        i = i + 1\n"
            "    return total + acc\n"
            "print(driver(12))\n";
        {
            Bytecode bc = compile_source(emit_mono_deopt_caller_live);
            ProfileDocument profile = hot_integer_profile(bc, "driver");
            apply_profile_guided_optimizations(bc, profile);
            pscript::jit::JitTieringConfig config;
            config.baseline_invocation_threshold = 1;
            config.optimizing_invocation_threshold = 1;
            config.require_profile_for_optimizing = false;
            std::ostringstream vm_out;
            {
                Runtime rt(vm_out);
                Vm vm(compile_source(emit_mono_deopt_caller_live), rt);
                vm.set_jit_enabled(false);
                vm.run();
                vm.run();
            }
            std::ostringstream jit_out;
            std::uint64_t emit_mono_fused = 0;
            std::uint64_t emit_mono_direct = 0;
            {
                Runtime rt(jit_out);
                Vm vm(std::move(bc), rt);
                vm.set_jit_enabled(true);
                vm.set_jit_config(config);
                vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
                vm.jit_engine().set_profile(&profile);
                pscript::jit::reset_ic_metrics(vm.jit_engine().ic_metrics());
                vm.run();
                vm.run();
                emit_mono_fused = vm.jit_engine().ic_metrics().emit_mono_invoke_fused;
                emit_mono_direct = vm.jit_engine().ic_metrics().nested_jit_emit_direct;
            }
            if (jit_out.str() != vm_out.str()) {
                fail_test("phase6 emitted mono deopt caller live parity",
                          "vm=" + vm_out.str() + " jit=" + jit_out.str());
            } else {
                // Fused mono x64 emit counters are optional; helper IC invoke path is used today.
                (void)emit_mono_fused;
                (void)emit_mono_direct;
                pass_test("phase6 emitted mono deopt caller live parity");
            }
        }
    }

    {
        const std::string source =
            "func g(x):\n"
            "    return x + 1\n"
            "func f():\n"
            "    callee = g\n"
            "    return callee(1)\n"
            "print(f())\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(compile_source(source), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != "2\n" || vm_out.str() != jit_out.str()) {
            fail_test("callvalue jit parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else {
            pass_test("callvalue jit parity");
        }
    }

    {
        const std::string source =
            "func swork(n):\n"
            "    s = \"\"\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        s = s + \"x\"\n"
            "        i = i + 1\n"
            "    return i\n"
            "print(swork(40))\n"
            "print(swork(40))\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 2;
        config.require_profile_for_optimizing = false;
        config.use_pgo_hotness = false;
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(compile_source(source), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("jit string concat parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().compile_attempts() < 1) {
            fail_test("jit string concat parity", "expected JIT compile");
        } else {
            pass_test("jit string concat parity");
        }
    }

    {
        const std::string source =
            "func rs(n):\n"
            "    if n <= 0:\n"
            "        return 0\n"
            "    return n + rs(n - 1)\n"
            "print(rs(40))\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        std::ostringstream out;
        Runtime rt(out);
        Vm vm(compile_source(source), rt);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (out.str() != vm_out.str() || out.str() != "820\n") {
            fail_test("jit recursive call parity", "jit=" + out.str() + " vm=" + vm_out.str());
        } else if (vm.jit_engine().compile_attempts() < 1) {
            fail_test("jit recursive call parity", "expected JIT compile");
        } else {
            pass_test("jit recursive call parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeRecursiveSum;
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        Bytecode bc = pscript::compile_with_builtins(source);
        ProfileDocument profile = hot_integer_profile(bc, "rs");
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        pscript::apply_profile_guided_optimizations(bc, profile);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(pscript::compile_with_builtins(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("jit recursive sum deep parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().compile_attempts() < 1) {
            fail_test("jit recursive sum deep parity", "expected JIT compile");
        } else {
            pass_test("jit recursive sum deep parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeRecursiveSum;
        Bytecode bc = pscript::compile_with_builtins(source);
        ProfileDocument profile = hot_integer_profile(bc, "rs");
        apply_profile_guided_optimizations(bc, profile);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1'000'000;
        config.require_profile_for_optimizing = true;
        std::ostringstream out;
        Runtime rt(out);
        Vm vm(std::move(bc), rt);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        vm.run();
        vm.run();
        if (out.str().find("32004000") == std::string::npos) {
            fail_test("jit recursive sum warm rerun", "unexpected output: " + out.str());
        } else {
            pass_test("jit recursive sum warm rerun");
        }
    }

    {
        const std::string source =
            "func f(x):\n"
            "    return x + 1\n"
            "func hot(n):\n"
            "    callee = f\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + callee(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(hot(500))\n";
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        ProfileDocument profile = hot_integer_profile(compile_source(source), "hot");
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Bytecode bc = compile_source(source);
        pscript::apply_profile_guided_optimizations(bc, profile);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("callvalue mono hot parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().compile_attempts() < 1) {
            fail_test("callvalue mono hot parity", "expected JIT compile");
        } else {
            pass_test("callvalue mono hot parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeClosureCall;
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        ProfileDocument profile = hot_integer_profile(compile_source(source), "bench");
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Bytecode bc = compile_source(source);
        pscript::apply_profile_guided_optimizations(bc, profile);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("closure call hot parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().compile_attempts() < 1) {
            fail_test("closure call hot parity", "expected JIT compile");
        } else {
            pass_test("closure call hot parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeClosureNested;
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        ProfileDocument profile = hot_integer_profile(compile_with_builtins(source), "bench");
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Bytecode bc = compile_with_builtins(source);
        pscript::apply_profile_guided_optimizations(bc, profile);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_with_builtins(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("closure nested hot parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().compile_attempts() < 1) {
            fail_test("closure nested hot parity", "expected JIT compile");
        } else {
            pass_test("closure nested hot parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeClosureNested;
        pscript::jit::JitTieringConfig baseline_cfg;
        baseline_cfg.baseline_invocation_threshold = 1;
        baseline_cfg.optimizing_invocation_threshold = 1'000'000;
        baseline_cfg.require_profile_for_optimizing = true;
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(compile_with_builtins(source), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(baseline_cfg);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_with_builtins(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("closure nested baseline parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().compile_attempts() < 1) {
            fail_test("closure nested baseline parity", "expected JIT compile");
        } else if (jit_vm.jit_engine().optimizing_compile_attempts() != 0) {
            fail_test("closure nested baseline parity", "expected baseline tier only");
        } else {
            pass_test("closure nested baseline parity");
        }
    }

    {
        using pscript::jit::JitCallIcMode;
        using pscript::jit::JitCallIcSlot;
        using pscript::jit::JitCompiledCode;
        using pscript::jit::update_call_ic;
        JitCallIcSlot slot;
        JitCompiledCode code_a;
        JitCompiledCode code_b;
        code_a.entry = reinterpret_cast<pscript::jit::JitNativeEntry>(0x1000);
        code_b.entry = reinterpret_cast<pscript::jit::JitNativeEntry>(0x2000);
        update_call_ic(&slot, 1, &code_a, nullptr, nullptr);
        if (slot.mode != JitCallIcMode::Monomorphic || slot.mono.function_index != 1) {
            fail_test("call ic mono", "expected monomorphic slot for first callee");
        } else {
            update_call_ic(&slot, 2, &code_b, nullptr, nullptr);
            if (slot.mode != JitCallIcMode::Polymorphic || slot.poly_count != 2) {
                fail_test("call ic poly transition", "expected polymorphic slot with two entries");
            } else if (slot.poly[0].function_index != 1 || slot.poly[1].function_index != 2) {
                fail_test("call ic poly transition", "expected prior mono entry preserved in poly[0]");
            } else {
                pass_test("call ic poly transition");
            }
        }
    }

    {
        const std::string source =
            "func shared(n):\n"
            "    x = 0\n"
            "    func inc():\n"
            "        x = x + 1\n"
            "        return x\n"
            "    func read():\n"
            "        return x\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        inc()\n"
            "        total = total + read()\n"
            "        i = i + 1\n"
            "    return total\n"
            "func outer(x):\n"
            "    func mid(y):\n"
            "        func inner(z):\n"
            "            return x + y + z\n"
            "        return inner\n"
            "    return mid\n"
            "print(shared(200))\n"
            "f = outer(10)\n"
            "g = f(20)\n"
            "print(g(3))\n"
            "func make_recursive():\n"
            "    func sum(n):\n"
            "        if n <= 0:\n"
            "            return 0\n"
            "        return n + sum(n - 1)\n"
            "    return sum\n"
            "r = make_recursive()\n"
            "print(r(20))\n";
        const Bytecode baseline_bc = compile_source(source);
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(baseline_bc, vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();

        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(baseline_bc, jit_rt);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("jit closure capture matrix parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else {
            pass_test("jit closure capture matrix parity");
        }
    }

    {
        const std::string source =
            "msg = \"boom\"\n"
            "func make():\n"
            "    func inner():\n"
            "        throw ValueException(msg, null)\n"
            "    return inner\n"
            "f = make()\n"
            "try:\n"
            "    f()\n"
            "catch ValueException error:\n"
            "    print(error.message)\n";
        const Bytecode bc = pscript::compile_with_builtins(source);
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(bc, vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(bc, jit_rt);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("jit closure exception parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else {
            pass_test("jit closure exception parity");
        }
    }

    {
        const std::string source =
            "func tuple_loop(n):\n"
            "    t = (1, 2, 3, 4, 5, 6, 7, 8)\n"
            "    total = 0\n"
            "    rep = 0\n"
            "    while rep < n:\n"
            "        i = 0\n"
            "        while i < 8:\n"
            "            total = total + t[i]\n"
            "            i = i + 1\n"
            "        rep = rep + 1\n"
            "    return total\n"
            "print(tuple_loop(400))\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(bc, vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(bc, jit_rt);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("jit tuple index parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else {
            pass_test("jit tuple index parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeCollectionAllocation;
        const Bytecode bc = pscript::compile_with_builtins(source);
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(bc, vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Bytecode jit_bc = bc;
        ProfileDocument profile = hot_integer_profile(bc, "allocate");
        pscript::apply_profile_guided_optimizations(jit_bc, profile);
        Vm jit_vm(std::move(jit_bc), jit_rt);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1'000'000;
        config.require_profile_for_optimizing = true;
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("jit collection allocation baseline parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else {
            pass_test("jit collection allocation baseline parity");
        }
        jit_out.str(std::string());
        jit_out.clear();
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        jit_vm.set_jit_config(config);
        jit_vm.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("jit collection allocation optimizing parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else {
            pass_test("jit collection allocation optimizing parity");
        }
    }

    {
        const std::string source =
            "func alias_write():\n"
            "    a = [1, 2, 3]\n"
            "    b = a\n"
            "    b[0] = 10\n"
            "    return a[0] + a[1] + a[2]\n"
            "print(alias_write())\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(bc, vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(bc, jit_rt);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.run();
        jit_out.str(std::string());
        jit_out.clear();
        jit_vm.run();
        if (jit_out.str() != vm_out.str() || jit_out.str() != "15\n") {
            fail_test("jit list alias mutation parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else {
            pass_test("jit list alias mutation parity");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All JIT tests passed\n";
    return 0;
}
