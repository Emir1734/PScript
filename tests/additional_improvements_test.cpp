#include "compiler/compiler.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::CompilerError;
using pscript::CompileOptions;
using pscript::Runtime;
using pscript::Vm;
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

std::string run_source(std::string_view source) {
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(compile_with_builtins(std::string(source)), runtime);
    vm.run();
    return out.str();
}

void expect_output(std::string_view name, std::string_view source, std::string_view expected) {
    try {
        const std::string actual = run_source(source);
        if (actual != expected) {
            fail_test(name, "expected output '" + std::string(expected) + "', got '" + actual + "'");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_compile_error(std::string_view name, std::string_view source, std::string_view substring) {
    try {
        (void)compile_with_builtins(std::string(source));
        fail_test(name, "expected compile failure");
    } catch (const CompilerError& err) {
        const std::string message = err.what();
        if (message.find(substring) == std::string::npos) {
            fail_test(name, "unexpected compile error: " + message);
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

}  // namespace

int main() {
    expect_output("url encode", "import url\nprint(url.encode(\"a b\"))\n", "a%20b\n");

    expect_output("uuid parse",
                  "import uuid\nu = uuid.parse(\"550e8400-e29b-41d4-a716-446655440000\")\nprint(u[\"string\"])\n",
                  "550e8400-e29b-41d4-a716-446655440000\n");

    expect_output("csv parse", "import csv\nrows = csv.parse(\"a,b\\n1,2\")\nprint(rows[0][0])\nprint(rows[1][1])\n",
                  "a\n2\n");

    expect_output("xml roundtrip",
                  "import xml\nn = xml.parse(\"<root><item>x</item></root>\")\nprint(n[\"tag\"])\nprint(n[\"children\"][0][\"text\"])\n",
                  "root\nx\n");

    expect_output("defer lifo",
                  "func demo():\n    defer print(\"second\")\n    defer print(\"first\")\n    print(\"body\")\ndemo()\n",
                  "body\nfirst\nsecond\n");

    expect_output("enum payload match",
                  "v = Result.Ok(42)\nmatch v:\n    Result.Ok(x):\n        print(x)\n    Result.Err(msg):\n        print(msg)\n",
                  "42\n");

    expect_compile_error("enum exhaustiveness", "enum Color:\n    Red\n    Blue\nc = Color.Red\nmatch c:\n    Color.Red:\n        print(\"r\")\n",
                         "non-exhaustive match on enum");
    expect_output("match guard", "x = 10\nmatch x:\n    n if n > 5:\n        print(n)\n    _:\n        print(0)\n",
                  "10\n");
    expect_output("match or literal", "match 2:\n    1 | 2:\n        print(\"hit\")\n    _:\n        print(\"miss\")\n",
                  "hit\n");

    expect_output("match alternation",
                  "n = 2\nmatch n:\n    1 | 2:\n        print(\"hit\")\n    _:\n        print(\"miss\")\n",
                  "hit\n");

    expect_output("json valid",
                  "import json\nprint(json.valid(\"{}\"))\nprint(json.valid(\"{\"))\n",
                  "true\nfalse\n");

    expect_output("result ok unwrap",
                  "func step():\n    return Result.Ok(7)\nfunc run():\n    x = step()?\n    print(x)\nrun()\n",
                  "7\n");

    expect_output("result err propagates",
                  "func fail():\n    return Result.Err(\"nope\")\nfunc run():\n    fail()?\n    print(\"ok\")\nfunc main():\n    r = run()\n    match r:\n        Result.Err(e):\n            print(e)\n        _:\n            print(\"unexpected\")\nmain()\n",
                  "nope\n");

    expect_output("result nested propagate",
                  "func a():\n    return Result.Ok(1)\nfunc b():\n    x = a()?\n    return Result.Ok(x + 1)\nfunc c():\n    y = b()?\n    print(y)\nc()\n",
                  "2\n");

    expect_output("ternary question still works",
                  "print(true ? 1 : 2)\nprint(false ? 1 : 2)\n",
                  "1\n2\n");

    expect_compile_error("propagate outside function", "x = Result.Ok(1)?\n", "outside function");

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All additional improvement tests passed\n";
    return 0;
}
