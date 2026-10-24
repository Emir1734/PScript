#include "compiler/compiler.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "stdlib/stdlib.h"
#include "version.h"
#include "vm/vm.h"

#include <cctype>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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

bool looks_like_semver(std::string_view version) {
    std::string_view core = version;
    const std::size_t dash = core.find('-');
    if (dash != std::string_view::npos) {
        core = core.substr(0, dash);
        if (dash + 1 >= version.size()) {
            return false;
        }
    }
    int parts = 0;
    int digits = 0;
    for (char c : core) {
        if (c == '.') {
            if (digits == 0) {
                return false;
            }
            ++parts;
            digits = 0;
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            ++digits;
        } else {
            return false;
        }
    }
    return digits > 0 && parts == 2;
}

std::string run_program(const std::string& source) {
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(compile_with_builtins(source), runtime);
    vm.run();
    return out.str();
}

void expect_output(std::string_view name, const std::string& source, std::string_view expected) {
    try {
        const std::string actual = run_program(source);
        if (actual != expected) {
            fail_test(name, "output mismatch");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_compiles(std::string_view name, const std::string& source) {
    try {
        CompileOptions options;
        options.source_file = "<compat>";
        (void)compile(parse(source), options);
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

}  // namespace

int main() {
    if (!looks_like_semver(pscript::kPscriptVersion)) {
        fail_test("version semver", pscript::kPscriptVersion);
    } else {
        pass_test("version semver");
    }

    expect_output("syntax colon block", "func f():\n    return 1\nprint(f())\n", "1\n");
    expect_output("syntax brace block", "func g() {\n    return 2\n}\nprint(g())\n", "2\n");
    expect_output("defer statement", "func w():\n    total = 0\n    defer total = total + 1\n    total = 5\n    return total\nprint(w())\n",
                  "5\n");
    expect_output("match statement", "match 2:\n    1:\n        print(10)\n    2:\n        print(20)\n    _:\n        print(0)\n",
                  "20\n");
    expect_output("stdlib math import", "import math\nprint(math.sqrt(9))\n", "3\n");

    {
        const auto modules = pscript::stdlib_module_specifiers();
        if (modules.empty()) {
            fail_test("stdlib module registry", "empty");
        } else {
            pass_test("stdlib module registry");
        }
    }

    expect_compiles("from import syntax", "from math import sqrt\n");

    if (g_failed != 0) {
        std::cerr << g_failed << " compatibility test(s) failed\n";
        return 1;
    }
    std::cout << "All compatibility tests passed\n";
    return 0;
}
