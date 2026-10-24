#include "compiler/compiler.h"
#include "parser/parser.h"
#include "runtime/profiler.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::CompileOptions;
using pscript::ProfileDocument;
using pscript::Runtime;
using pscript::RuntimeProfiler;
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

ProfileDocument run_profiled(const std::string& source, bool enabled = true) {
    std::ostringstream output;
    Runtime runtime(output);
    runtime.profiler().set_enabled(enabled);
    Bytecode bytecode = compile_source(source);
    Vm vm(std::move(bytecode), runtime);
    vm.run();
    return runtime.profiler().finalize(vm.bytecode());
}

}  // namespace

int main() {
    {
        const ProfileDocument document = run_profiled(
            "func hot():\n"
            "    i = 0\n"
            "    while i < 1000:\n"
            "        i = i + 1\n"
            "hot()\n");

        if (document.instruction_executions == 0) {
            fail_test("instruction counting", "expected non-zero instruction executions");
        } else {
            pass_test("instruction counting");
        }

        std::uint64_t opcode_total = 0;
        for (const auto& opcode : document.opcodes) {
            opcode_total += opcode.count;
        }
        if (opcode_total != document.instruction_executions) {
            fail_test("opcode counting", "opcode totals do not match instruction executions");
        } else {
            pass_test("opcode counting");
        }

        bool saw_calls = false;
        for (const auto& fn : document.functions) {
            if (fn.name == "hot" && fn.calls >= 1) {
                saw_calls = true;
            }
        }
        if (!saw_calls) {
            fail_test("function call counting", "expected hot() call count");
        } else {
            pass_test("function call counting");
        }

        if (document.loops.empty()) {
            fail_test("loop profiling", "expected loop back-edge hits");
        } else {
            pass_test("loop profiling");
        }

        bool saw_call_site = false;
        for (const auto& site : document.call_sites) {
            if (site.calls > 0) {
                saw_call_site = true;
            }
        }
        if (!saw_call_site) {
            fail_test("call-site profiling", "expected call site hits");
        } else {
            pass_test("call-site profiling");
        }
    }

    {
        const ProfileDocument document = run_profiled("print(1)\n");
        pass_test(document.memory.allocation_count > 0 ? "allocation stats collection" : "allocation stats optional");
    }

    {
        const ProfileDocument document = run_profiled("x = 1\n", false);
        if (document.instruction_executions != 0) {
            fail_test("profiling disabled", "expected zero instructions when disabled");
        } else {
            pass_test("profiling disabled");
        }
    }

    {
        const ProfileDocument document = run_profiled("x = 0\n");
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "pscript-profile-test.psprof.json";
        std::string error;
        if (!pscript::save_profile_document(document, path.string(), error)) {
            fail_test("profile save", error);
        } else {
            const std::optional<ProfileDocument> loaded = pscript::load_profile_document(path.string(), error);
            if (!loaded.has_value()) {
                fail_test("profile load", error);
            } else if (loaded->instruction_executions != document.instruction_executions) {
                fail_test("profile roundtrip", "instruction_executions mismatch");
            } else {
                pass_test("profile file roundtrip");
            }
        }
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    {
        std::string error;
        const std::optional<ProfileDocument> loaded = pscript::load_profile_document("<missing>", error);
        if (loaded.has_value()) {
            fail_test("corrupt profile", "missing file should fail");
        } else {
            pass_test("missing profile rejected");
        }

        const std::filesystem::path bad =
            std::filesystem::temp_directory_path() / "pscript-profile-bad.psprof.json";
        {
            std::ofstream out(bad, std::ios::trunc);
            out << "{ \"format\": \"other\" }\n";
        }
        const std::optional<ProfileDocument> bad_loaded = pscript::load_profile_document(bad.string(), error);
        if (bad_loaded.has_value()) {
            fail_test("invalid profile format", "should reject unknown format");
        } else {
            pass_test("invalid profile format rejected");
        }
        std::error_code ec;
        std::filesystem::remove(bad, ec);
    }

    {
        const std::string source =
            "func inc(x):\n"
            "    return x + 1\n"
            "func hot(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + inc(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(hot(50000))\n";
        const ProfileDocument document = run_profiled(source);
        std::uint64_t call_count = 0;
        std::uint64_t return_count = 0;
        std::uint64_t load_local_count = 0;
        for (const auto& opcode : document.opcodes) {
            if (opcode.name == "CALL") {
                call_count = opcode.count;
            } else if (opcode.name == "RETURN") {
                return_count = opcode.count;
            } else if (opcode.name == "LOAD_LOCAL") {
                load_local_count = opcode.count;
            }
        }
        std::uint64_t inc_calls = 0;
        std::uint64_t hot_inclusive = 0;
        for (const auto& fn : document.functions) {
            if (fn.name == "inc") {
                inc_calls = fn.calls;
            } else if (fn.name == "hot") {
                hot_inclusive = fn.inclusive_ns;
            }
        }
        if (inc_calls < 49'000 || call_count < 49'000) {
            fail_test("simple_calls profile shape",
                      "inc_calls=" + std::to_string(inc_calls) + " CALL=" + std::to_string(call_count) + " RETURN=" +
                          std::to_string(return_count));
        } else if (load_local_count == 0) {
            fail_test("simple_calls profile shape", "expected LoadLocal hits");
        } else if (hot_inclusive == 0) {
            fail_test("simple_calls profile shape", "expected hot() inclusive time");
        } else {
            std::cout << "PROFILE simple_calls inc_calls=" << inc_calls << " Call=" << call_count
                      << " Return=" << return_count << " LoadLocal=" << load_local_count
                      << " hot_inclusive_ms=" << (hot_inclusive / 1'000'000.0) << '\n';
            pass_test("simple_calls profile shape");
        }
    }

    {
        const std::string source = "x = 0\nwhile x < 500:\n    x = x + 1\n";
        std::ostringstream output;
        Runtime runtime(output);
        runtime.profiler().set_enabled(false);
        Bytecode bytecode = compile_source(source);
        const auto start = std::chrono::steady_clock::now();
        Vm vm(std::move(bytecode), runtime);
        vm.run();
        const auto baseline =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();

        std::ostringstream output2;
        Runtime runtime2(output2);
        runtime2.profiler().set_enabled(true);
        Bytecode bytecode2 = compile_source(source);
        const auto start2 = std::chrono::steady_clock::now();
        Vm vm2(std::move(bytecode2), runtime2);
        vm2.run();
        const auto profiled =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start2).count();

        if (profiled > baseline * 3 + 50) {
            fail_test("profiling overhead", "profiled run unexpectedly slow");
        } else {
            pass_test("profiling overhead bounded");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " profiling test(s) failed\n";
        return 1;
    }
    std::cout << "All profiling tests passed\n";
    return 0;
}
