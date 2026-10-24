#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_backend.h"
#include "jit/jit_runtime.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

using pscript::Bytecode;
using pscript::CompileOptions;
using pscript::Lexer;
using pscript::LexerError;
using pscript::ParserError;
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

Bytecode compile_source(const std::string& source) {
    CompileOptions options;
    options.source_file = "<stabilization>";
    return compile(parse(source), options);
}

std::string run_with_jit_mode(const std::string& source, bool jit, const ProfileDocument* profile = nullptr,
                              pscript::jit::JitTieringConfig* config_override = nullptr) {
    Bytecode bc = compile_with_builtins(source);
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
    config.osr_loop_trip_threshold = 100'000;
    config.deopt_recompile_invocation_cooldown = 8;
    if (config_override) {
        config = *config_override;
    }
    vm.set_jit_config(config);
    if (jit && profile) {
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(profile);
    }
    vm.run();
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

void expect_parity(std::string_view name, const std::string& source, const ProfileDocument* profile = nullptr) {
    try {
        const std::string vm_out = run_with_jit_mode(source, false);
        const std::string jit_out = run_with_jit_mode(source, true, profile);
        if (vm_out != jit_out) {
            fail_test(name, "vm vs jit mismatch");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_optimizing_parity(std::string_view name, const std::string& source, const std::string& hot_fn) {
    try {
        Bytecode bc = compile_with_builtins(source);
        ProfileDocument profile = hot_profile(bc, hot_fn);
        pscript::apply_profile_guided_optimizations(bc, profile);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        const std::string vm_out = run_with_jit_mode(source, false);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        vm.run();
        if (vm_out != out.str()) {
            fail_test(name, "vm vs optimizing mismatch");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void fuzz_lexer_no_crash(std::string_view name, const std::string& source) {
    try {
        (void)Lexer(source).tokenize();
        pass_test(name);
    } catch (const LexerError&) {
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void fuzz_parse_no_crash(std::string_view name, const std::string& source) {
    try {
        (void)parse(source);
        pass_test(name);
    } catch (const ParserError&) {
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

}  // namespace

int main() {
    {
        const std::string source =
            "func acc(n):\n    total = 0\n    i = 0\n    while i < n:\n        total = total + i\n        i = i + 1\n"
            "    return total\nprint(acc(15))\n";
        const Bytecode bc = compile_with_builtins(source);
        const ProfileDocument profile = hot_profile(bc, "acc");
        expect_parity("vm jit parity loop arithmetic", source, &profile);
    }

    {
        const std::string source =
            "func mix(x):\n    return x + 1\nprint(mix(3))\nprint(mix(4))\n";
        const Bytecode bc = compile_with_builtins(source);
        const ProfileDocument profile = hot_profile(bc, "mix");
        expect_parity("vm jit parity after deopt path", source, &profile);
    }

    {
        std::ostringstream err_capture;
        std::ostringstream out;
        Runtime runtime(out, std::cin, err_capture);
        const std::string source = "func id(n):\n    return n\nprint(id(9))\n";
        Vm vm(compile_with_builtins(source), runtime);
        vm.run();
        if (!err_capture.str().empty()) {
            fail_test("jit silent stderr on success", err_capture.str());
        } else if (out.str() != "9\n") {
            fail_test("jit silent stderr on success", "bad output " + out.str());
        } else {
            pass_test("jit silent stderr on success");
        }
    }

    fuzz_lexer_no_crash("fuzz lexer unterminated string", "x = \"abc\n");
    fuzz_lexer_no_crash("fuzz lexer invalid escape", "x = \"\\q\"\n");
    fuzz_parse_no_crash("fuzz parser incomplete block", "if true:\n");
    fuzz_parse_no_crash("fuzz parser bad import", "import \n");

    {
        static const char kMalformed[] = "\xFF\xFE\x00func x():\n    return 1\n";
        fuzz_lexer_no_crash("fuzz lexer non-utf8 bytes", std::string(kMalformed, sizeof(kMalformed) - 1));
    }

    {
        const std::string source =
            "try:\n    print(1)\nfinally:\n    print(2)\n";
        expect_parity("vm jit parity try finally", source);
    }

    {
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        const std::string source =
            "func kernel(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i * 2\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(kernel(80))\n";
        Bytecode bc = compile_with_builtins(source);
        ProfileDocument profile = hot_profile(bc, "kernel");
        pscript::apply_profile_guided_optimizations(bc, profile);
        try {
            const std::string vm_out = run_with_jit_mode(source, false);
            std::ostringstream out;
            Runtime runtime(out);
            Vm vm(std::move(bc), runtime);
            vm.set_jit_enabled(true);
            vm.set_jit_config(config);
            vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
            vm.jit_engine().set_profile(&profile);
            vm.run();
            if (vm_out != out.str()) {
                fail_test("vm jit parity optimizing loop kernel", "output mismatch");
            } else {
                pass_test("vm jit parity optimizing loop kernel");
            }
        } catch (const std::exception& ex) {
            fail_test("vm jit parity optimizing loop kernel", ex.what());
        }
    }

    {
        const std::string source =
            "func nested(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        j = 0\n"
            "        while j < n:\n"
            "            total = total + i + j\n"
            "            j = j + 1\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(nested(25))\n";
        expect_optimizing_parity("optimizing nested loop parity", source, "nested");
    }

    {
        const std::string source =
            "func call_in_loop(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + bump(i)\n"
            "        i = i + 1\n"
            "    return total\n"
            "func bump(x):\n"
            "    return x + 1\n"
            "print(call_in_loop(120))\n";
        expect_optimizing_parity("optimizing loop with call parity", source, "call_in_loop");
    }

    {
        const std::string source =
            "func cond(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        if i < n / 2:\n"
            "            total = total + i\n"
            "        else:\n"
            "            total = total + 1\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(cond(200))\n";
        expect_optimizing_parity("optimizing loop conditional parity", source, "cond");
    }

    {
        const std::string source =
            "func kernel(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i * 2\n"
            "        i = i + 1\n"
            "    return total\n"
            "acc = 0\n"
            "rep = 0\n"
            "while rep < 5:\n"
            "    acc = acc + kernel(4000)\n"
            "    rep = rep + 1\n"
            "print(acc)\n";
        expect_optimizing_parity("optimizing amortized kernel parity", source, "kernel");
    }

    {
        const std::string source =
            "func mix(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        if i == 5:\n"
            "            total = total + 1.5\n"
            "        total = total + i\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(mix(20))\n";
        expect_optimizing_parity("optimizing loop float deopt parity", source, "mix");
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All stabilization tests passed\n";
    return 0;
}
