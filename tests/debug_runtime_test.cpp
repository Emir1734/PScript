#include "compiler/compiler.h"
#include "debug/debug_eval.h"
#include "debug/debug_mapping.h"
#include "debug/debug_session.h"
#include "debug/debug_value.h"
#include "debug/debug_vm_inspector.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::CompileOptions;
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

Bytecode compile_source(const std::string& source, const std::string& file = "<dbg-test>") {
    CompileOptions options;
    options.source_file = file;
    return compile(parse(source), options);
}

}  // namespace

int main() {
    {
        const std::string source =
            "x = 1\n"
            "y = 2\n"
            "print(x + y)\n";
        const Bytecode bc = compile_source(source);
        if (!pscript::debug::line_is_executable(bc, bc.source_file, 1)) {
            fail_test("source mapping line executable", "line 1 not executable");
        } else {
            pass_test("source mapping line executable");
        }
        const auto ips = pscript::debug::map_line_to_ips(bc, bc.source_file, 3);
        if (ips.empty()) {
            fail_test("source mapping line to ip", "no ips for print line");
        } else {
            pass_test("source mapping line to ip");
        }
    }

    {
        const std::string source =
            "func add(a, b):\n"
            "    return a + b\n"
            "print(add(3, 4))\n";
        const Bytecode bc = compile_source(source);
        std::uint32_t add_idx = 0xFFFFFFFFu;
        for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
            if (bc.functions[i].name == "add") {
                add_idx = i;
                break;
            }
        }
        if (add_idx == 0xFFFFFFFFu || add_idx >= bc.function_debug.size() ||
            bc.function_debug[add_idx].locals.size() < 2) {
            fail_test("debug locals metadata", "missing function debug locals");
        } else {
            pass_test("debug locals metadata");
        }
    }

    {
        const std::string source =
            "func bump(n):\n"
            "    return n + 1\n"
            "x = 0\n"
            "while x < 3:\n"
            "    x = bump(x)\n"
            "print(x)\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        controller.add_line_breakpoint(vm.bytecode().source_file, 6);
        vm.run();
        if (controller.state() != pscript::debug::DebugState::Paused) {
            fail_test("breakpoint hit", "not paused");
        } else {
            pass_test("breakpoint hit");
        }
        pscript::debug::VmInspector inspector(vm);
        if (inspector.stack_frames().empty()) {
            fail_test("stack frames", "empty");
        } else {
            pass_test("stack frames");
        }
        controller.continue_execution();
        vm.debug_continue_execution();
        pass_test("breakpoint continue smoke");
        controller.detach(vm);
        std::ostringstream out2;
        Runtime runtime2(out2);
        Vm vm2(compile_source(source), runtime2);
        vm2.run();
        if (out2.str() != "3\n") {
            fail_test("program output without debugger", out2.str());
        } else {
            pass_test("program output without debugger");
        }
    }

    {
        const std::string source =
            "func inc(n):\n"
            "    return n + 1\n"
            "v = inc(1)\n"
            "print(v)\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        controller.add_function_breakpoint("inc");
        vm.run();
        if (controller.state() != pscript::debug::DebugState::Paused) {
            fail_test("function breakpoint", "not paused");
        } else {
            pass_test("function breakpoint");
        }
        controller.continue_execution();
        vm.debug_continue_execution();
        controller.detach(vm);
        pass_test("function breakpoint continue");
    }

    {
        const std::string source = "items = [1, 2, 3]\nprint(items[1])\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        controller.add_line_breakpoint(vm.bytecode().source_file, 2);
        vm.run();
        pscript::debug::VmInspector inspector(vm);
        pscript::debug::EvalError error;
        pscript::debug::ExpressionEvaluator eval(inspector);
        const auto value = eval.evaluate("items[1]", error);
        if (!value.has_value() || value->kind != pscript::ValueKind::Integer || value->integer != 2) {
            fail_test("expression index", error.message);
        } else {
            pass_test("expression index");
        }
        const auto bad = eval.evaluate("items = 1", error);
        if (bad.has_value()) {
            fail_test("expression assignment rejected", "unexpected success");
        } else {
            pass_test("expression assignment rejected");
        }
        controller.detach(vm);
        vm.debug_continue_execution();
    }

    {
        pscript::debug::ValueInspector inspector;
        const pscript::debug::DebugValue value = inspector.inspect(Runtime::make_integer(42));
        if (value.kind != pscript::debug::DebugValueKind::Integer) {
            fail_test("value inspect int", "wrong kind");
        } else {
            pass_test("value inspect int");
        }
    }

    {
        const std::string source = "print(1)\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        const auto t0 = std::chrono::steady_clock::now();
        vm.run();
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed > 5'000'000) {
            fail_test("debugger off overhead", "run too slow");
        } else {
            pass_test("debugger off overhead smoke");
        }
    }

    {
        const std::string source =
            "i = 0\n"
            "while i < 200:\n"
            "    i = i + 1\n"
            "print(i)\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        const std::uint32_t bp = controller.add_line_breakpoint(vm.bytecode().source_file, 3);
        controller.set_breakpoint_condition(bp, "i == 99");
        vm.run();
        if (controller.state() != pscript::debug::DebugState::Paused) {
            fail_test("conditional breakpoint", "not paused");
        } else {
            pscript::debug::EvalError error;
            const auto value = controller.evaluate_watch("i", error);
            if (!value.has_value() || value->integer != 99) {
                fail_test("conditional breakpoint value", error.message);
            } else {
                pass_test("conditional breakpoint");
            }
        }
        controller.detach(vm);
    }

    {
        const std::string source =
            "func add(a, b):\n"
            "    return a + b\n"
            "x = add(3, 4)\n"
            "print(x)\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        controller.add_line_breakpoint(vm.bytecode().source_file, 4);
        vm.run();
        pscript::debug::EvalError error;
        const auto value = controller.evaluate_watch("add(1, 2) + 10", error);
        if (!value.has_value() || value->integer != 13) {
            fail_test("expression call eval", error.message);
        } else {
            pass_test("expression call eval");
        }
        controller.detach(vm);
    }

    {
        const std::string source =
            "func demo():\n"
            "    x = 1\n"
            "    if true:\n"
            "        y = 2\n"
            "    print(x)\n"
            "demo()\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        controller.add_line_breakpoint(vm.bytecode().source_file, 5);
        vm.run();
        pscript::debug::VmInspector inspector(vm);
        bool has_y = false;
        for (const auto& local : inspector.stack_frames().back().locals) {
            if (local.name == "y") {
                has_y = true;
            }
        }
        if (has_y) {
            fail_test("scope hides block local", "y visible outside block");
        } else {
            pass_test("scope hides block local");
        }
        controller.detach(vm);
    }

    {
        const std::string source =
            "hits = 0\n"
            "while hits < 5:\n"
            "    hits = hits + 1\n"
            "print(hits)\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        const std::uint32_t id = controller.add_line_breakpoint(vm.bytecode().source_file, 3);
        controller.set_breakpoint_hit_count_target(id, 3);
        vm.run();
        if (controller.state() != pscript::debug::DebugState::Paused) {
            fail_test("hit count breakpoint", "not paused");
        } else {
            pass_test("hit count breakpoint");
        }
        controller.detach(vm);
    }

    {
        const std::string source = "print(1)\nprint(2)\n";
        const Bytecode bc = compile_source(source);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.attach(vm);
        controller.add_temporary_line_breakpoint(vm.bytecode().source_file, 1);
        vm.run();
        controller.continue_execution();
        vm.debug_continue_execution();
        if (controller.state() == pscript::debug::DebugState::Paused) {
            fail_test("one-shot breakpoint", "paused twice");
        } else {
            pass_test("one-shot breakpoint");
        }
        controller.detach(vm);
    }

    {
        const std::string source =
            "func boom():\n"
            "    throw RuntimeException(\"x\", null)\n"
            "try:\n"
            "    boom()\n"
            "catch RuntimeException e:\n"
            "    print(\"caught\")\n";
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(source), runtime);
        pscript::debug::ExecutionController controller(vm.bytecode());
        controller.set_exception_breakpoint(pscript::debug::ExceptionBreakpointMode::All);
        controller.attach(vm);
        vm.run();
        if (controller.state() != pscript::debug::DebugState::Paused) {
            fail_test("exception breakpoint all", "not paused");
        } else {
            pass_test("exception breakpoint all");
        }
        controller.detach(vm);
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All debug runtime tests passed\n";
    return 0;
}
