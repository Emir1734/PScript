#include "benchmark/bench_workloads.h"
#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "debug/debug_session.h"
#include "jit/jit_ir.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::CompileOptions;
using pscript::ProfileDocument;
using pscript::Runtime;
using pscript::Vm;
using pscript::compile;
using pscript::compile_with_builtins;
using pscript::parse;

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

std::string run_vm(const std::string& source) {
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(compile_with_builtins(source), runtime);
    vm.run();
    return out.str();
}

std::string run_debug_attached(const std::string& source) {
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(compile_with_builtins(source), runtime);
    pscript::debug::ExecutionController controller(vm.bytecode());
    controller.attach(vm);
    vm.run();
    if (controller.state() == pscript::debug::DebugState::Paused) {
        controller.detach(vm);
        return {};
    }
    controller.detach(vm);
    return out.str();
}

ProfileDocument hot_profile(const Bytecode& bc, const std::string& fn_name) {
    ProfileDocument doc;
    doc.version = ProfileDocument::kFormatVersion;
    doc.pgo_compat_version = pscript::kPgoCompatVersion;
    doc.program_fingerprint = pscript::compute_program_fingerprint(bc);
    for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
        if (bc.functions[i].name == fn_name) {
            pscript::ProfileFunctionRecord fn;
            fn.function_index = i;
            fn.calls = 50'000;
            fn.hotness = pscript::ProfileHotness::Hot;
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

std::string run_optimizing_jit(const std::string& source, const std::string& hot_fn) {
    Bytecode bc = compile_with_builtins(source);
    ProfileDocument profile = hot_profile(bc, hot_fn);
    pscript::apply_profile_guided_optimizations(bc, profile);
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(std::move(bc), runtime);
    vm.set_jit_enabled(true);
    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;
    config.optimizing_invocation_threshold = 1;
    config.require_profile_for_optimizing = false;
    config.osr_loop_trip_threshold = 8;
    vm.set_jit_config(config);
    vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
    vm.jit_engine().set_profile(&profile);
    vm.run();
    return out.str();
}

constexpr const char kComplexMatrixSource[] =
    "func inner(n, acc):\n"
    "    if n <= 0:\n"
    "        return acc\n"
    "    defer acc = acc + 1\n"
    "    try:\n"
    "        if n % 2 == 0:\n"
    "            acc = acc + inner(n - 1, acc)\n"
    "        else:\n"
    "            acc = acc + n\n"
    "    catch Exception e:\n"
    "        acc = acc\n"
    "    finally:\n"
    "        acc = acc + 0\n"
    "    return acc\n"
    "func driver(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + inner(4, 0)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(driver(6))\n";

}  // namespace

int main() {
    {
        const std::string vm_out = run_vm(kComplexMatrixSource);
        const std::string dbg_out = run_debug_attached(kComplexMatrixSource);
        if (dbg_out.empty() || vm_out != dbg_out) {
            fail_test("matrix debug vm parity", "output mismatch");
        } else {
            pass_test("matrix debug vm parity");
        }
    }

    {
        const std::string vm_out = run_vm(kComplexMatrixSource);
        const std::string jit_out = run_optimizing_jit(kComplexMatrixSource, "driver");
        if (vm_out != jit_out) {
            fail_test("matrix optimizing jit parity", "vm vs optimizing mismatch");
        } else {
            pass_test("matrix optimizing jit parity");
        }
    }

    {
        const std::string source =
            "func fact(n):\n"
            "    if n <= 1:\n"
            "        return 1\n"
            "    return n * fact(n - 1)\n"
            "print(fact(8))\n";
        if (run_vm(source) != run_debug_attached(source)) {
            fail_test("matrix recursion debug parity", "mismatch");
        } else {
            pass_test("matrix recursion debug parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeAsync;
        const std::string vm_out = run_vm(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(source), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        vm.run();
        controller.detach(vm);
        if (vm_out != out.str()) {
            fail_test("async debug attached parity", "mismatch");
        } else {
            pass_test("async debug attached parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeAsync;
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(source), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        controller.add_function_breakpoint("worker");
        vm.run();
        const bool paused = controller.state() == pscript::debug::DebugState::Paused;
        controller.detach(vm);
        if (!paused) {
            fail_test("async breakpoint pause", "not paused");
        } else {
            pass_test("async breakpoint pause");
        }
    }

    {
        static constexpr char kCallsTiming[] =
            "func leaf(x):\n"
            "    return x + 1\n"
            "func mid(x):\n"
            "    return leaf(x) + leaf(x + 1)\n"
            "func top(x):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < x:\n"
            "        total = total + mid(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "top(2500)\n";
        {
            const Bytecode bc = compile_with_builtins(kCallsTiming);
            std::uint32_t top_idx = 0xFFFFFFFFu;
            for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
                if (bc.functions[i].name == "top") {
                    top_idx = i;
                    break;
                }
            }
            std::string reason;
            if (top_idx == 0xFFFFFFFFu ||
                !pscript::jit::jit_function_is_eligible(bc, top_idx, reason)) {
                fail_test("calls workload jit eligibility", reason.empty() ? "top missing" : reason);
            } else {
                pass_test("calls workload jit eligibility");
            }
        }
        auto run_calls = [](bool jit) {
            Bytecode bc = compile_with_builtins(kCallsTiming);
            std::ostringstream out;
            Runtime runtime(out);
            Vm vm(std::move(bc), runtime);
            pscript::jit::JitTieringConfig config;
            config.baseline_invocation_threshold = 1;
            config.optimizing_invocation_threshold = 2;
            vm.set_jit_config(config);
            vm.set_jit_enabled(jit);
            const auto t0 = std::chrono::steady_clock::now();
            vm.run();
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                .count();
        };
        (void)run_calls(false);
        (void)run_calls(true);
        const long long vm_ms = run_calls(false);
        const long long jit_ms = run_calls(true);
        const bool ratio_fail = vm_ms >= 10 && jit_ms > vm_ms * 40 + 80;
        const bool absolute_fail = vm_ms < 10 && jit_ms > 400;
        if (ratio_fail || absolute_fail) {
            fail_test("jit calls loop overhead", "jit=" + std::to_string(jit_ms) + " vm=" + std::to_string(vm_ms));
        } else {
            pass_test("jit calls loop overhead");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins("print(1)\n"), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        if (vm.jit_enabled()) {
            fail_test("debugger disables jit", "jit still enabled");
        } else {
            pass_test("debugger disables jit");
        }
        controller.detach(vm);
        if (!vm.jit_enabled()) {
            fail_test("debugger restores jit", "jit not restored");
        } else {
            pass_test("debugger restores jit");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All debug matrix tests passed\n";
    return 0;
}
