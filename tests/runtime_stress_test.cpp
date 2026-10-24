#include "compiler/compiler.h"
#include "parser/parser.h"
#include "runtime/runtime_limits.h"
#include "stdlib/native_boundary.h"
#include "vm/vm.h"

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using pscript::Bytecode;
using pscript::CompileOptions;
using pscript::CompilerError;
using pscript::Constant;
using pscript::ConstantKind;
using pscript::Instruction;
using pscript::Opcode;
using pscript::Runtime;
using pscript::Vm;
using pscript::VmError;
using pscript::compile;
using pscript::compile_with_builtins;
using pscript::limits::kMaxBytecodeInstructions;

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

Bytecode compile_ok(const std::string& source) {
    return compile_with_builtins(source);
}

void expect_no_crash_compile(std::string_view name, const std::string& source) {
    try {
        (void)compile_ok(source);
        pass_test(name);
    } catch (const std::exception&) {
        pass_test(name);
    }
}

void expect_vm_error(std::string_view name, Bytecode bytecode, std::string_view fragment) {
    try {
        std::ostringstream out;
        Vm vm(std::move(bytecode), out);
        vm.run();
        fail_test(name, "expected VM error");
    } catch (const VmError& err) {
        if (std::string_view(err.message()).find(fragment) == std::string_view::npos) {
            fail_test(name, std::string("unexpected message: ") + err.message());
        } else {
            pass_test(name);
        }
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_runtime_message(std::string_view name, const std::string& source, std::string_view fragment) {
    try {
        std::ostringstream out;
        Vm vm(compile_ok(source), out);
        vm.run();
        fail_test(name, "expected runtime error");
    } catch (const VmError& err) {
        if (std::string_view(err.message()).find(fragment) == std::string_view::npos) {
            fail_test(name, std::string("unexpected: ") + err.message());
        } else {
            pass_test(name);
        }
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

}  // namespace

int main() {
    expect_no_crash_compile("stress empty source", "");
    expect_no_crash_compile("stress incomplete string", "x = \"hello\n");
    expect_no_crash_compile("stress incomplete expression", "1 +\n");
    expect_no_crash_compile("stress broken block", "if true:\n");
    expect_no_crash_compile("stress long identifier", std::string("a_") + std::string(4000, 'b') + " = 1\n");
    expect_no_crash_compile("stress deep parens", std::string(200, '(') + "1" + std::string(200, ')') + "\n");

    {
        std::string many_funcs;
        for (int i = 0; i < 80; ++i) {
            many_funcs += "func f" + std::to_string(i) + "():\n    return " + std::to_string(i) + "\n";
        }
        expect_no_crash_compile("stress many functions", many_funcs);
    }

    {
        Bytecode bc;
        bc.register_count = 4;
        bc.constants.push_back(Constant{ConstantKind::String, "x"});
        bc.instructions.push_back(Instruction{Opcode::LoadConst, 0, 0, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("stress invalid opcode", std::move(bc), "invalid bytecode");
    }

    {
        Bytecode bc;
        bc.register_count = 2;
        bc.instructions.push_back(Instruction{Opcode::LoadConst, 9, 0, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("stress invalid register", std::move(bc), "invalid register");
    }

    {
        Bytecode bc;
        bc.register_count = 2;
        bc.constants.push_back(Constant{ConstantKind::Integer, "1"});
        bc.instructions.push_back(Instruction{Opcode::LoadConst, 0, 9, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("stress invalid constant index", std::move(bc), "invalid constant");
    }

    {
        Bytecode bc;
        bc.register_count = 2;
        bc.instructions.push_back(Instruction{Opcode::Jump, 5, 0, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("stress invalid jump target", std::move(bc), "invalid jump");
    }

    static_assert(pscript::limits::kMaxInstructionExecutions > 1'000'000,
                  "instruction budget should allow normal programs");
    pass_test("stress instruction limit configured");

    expect_runtime_message("stress deep recursion",
                           "func boom(n):\n    return boom(n + 1)\nprint(boom(0))\n",
                           "maximum call stack depth exceeded");

    {
        try {
            std::ostringstream out;
            Runtime runtime(out);
            (void)runtime.call("min", {Runtime::make_string("x")});
            fail_test("stress invalid native arguments", "expected error");
        } catch (const pscript::RuntimeError&) {
            pass_test("stress invalid native arguments");
        } catch (const std::exception& ex) {
            fail_test("stress invalid native arguments", ex.what());
        }
    }

    {
        try {
            std::ostringstream out;
            Vm vm(compile_ok("throw RuntimeException(\"boom\", null)\n"), out);
            vm.run();
            fail_test("stress user throw", "expected failure");
        } catch (const VmError&) {
            pass_test("stress user throw");
        } catch (const std::exception& ex) {
            fail_test("stress user throw", ex.what());
        }
    }

    {
        std::string src = "items = []\nfor i in range(200):\n    items.append(i)\nprint(items)\n";
        try {
            std::ostringstream out;
            Vm vm(compile_ok(src), out);
            vm.run();
            if (out.str().empty()) {
                fail_test("stress list append bounded", "empty output");
            } else {
                pass_test("stress list append bounded");
            }
        } catch (const std::exception& ex) {
            fail_test("stress list append bounded", ex.what());
        }
    }

    {
        const std::string src =
            "async func main():\n    t = async_input()\n    await delay(0ms)\n    t.cancel()\n    try:\n"
            "        await t\n    catch CancellationException:\n        print(\"cancelled\")\n";
        try {
            std::istringstream in("never\n");
            std::ostringstream out;
            Runtime runtime(out, in);
            Vm vm(compile_with_builtins(src), runtime);
            vm.run();
            if (out.str() != "cancelled\n") {
                fail_test("stress async input cancellation", "got \"" + out.str() + "\"");
            } else {
                pass_test("stress async input cancellation");
            }
        } catch (const std::exception& ex) {
            fail_test("stress async input cancellation", ex.what());
        }
    }

    {
        const std::string src =
            "func bump(n, acc):\n"
            "    if n == 0:\n"
            "        return acc\n"
            "    return bump(n - 1, acc + 1)\n"
            "print(bump(4000, 0))\n";
        try {
            std::ostringstream out;
            Vm vm(compile_ok(src), out);
            vm.run();
            if (out.str() != "4000\n") {
                fail_test("stress deep calls cell reuse", "got \"" + out.str() + "\"");
            } else {
                pass_test("stress deep calls cell reuse");
            }
        } catch (const std::exception& ex) {
            fail_test("stress deep calls cell reuse", ex.what());
        }
    }

    {
        Bytecode bc;
        bc.register_count = 1;
        bc.instructions = {Instruction{Opcode::Halt, 0, 0, 0}};
        bc.instructions.resize(static_cast<std::size_t>(kMaxBytecodeInstructions) + 1,
                               Instruction{Opcode::Halt, 0, 0, 0});
        if (const auto err = bc.validate()) {
            if (std::string_view(*err).find("instruction count") == std::string_view::npos) {
                fail_test("stress bytecode size limit", *err);
            } else {
                pass_test("stress bytecode size limit");
            }
        } else {
            fail_test("stress bytecode size limit", "expected validation failure");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All runtime stress tests passed\n";
    return 0;
}
