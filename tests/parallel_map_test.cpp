#include "compiler/compiler.h"
#include "runtime/collection_ops.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::Runtime;
using pscript::Vm;
using pscript::compile_with_builtins;

static void fail(const char* message) {
    std::cerr << "FAIL " << message << "\n";
    std::exit(1);
}

static void pass(const char* message) {
    std::cout << "PASS " << message << "\n";
}

static void run_source(const char* source) {
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(compile_with_builtins(source), runtime);
    vm.run();
}

static void expect_output(const char* name, const char* source, const std::string& expected) {
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(compile_with_builtins(source), runtime);
    vm.run();
    if (out.str() != expected) {
        std::cerr << "FAIL " << name << " got: " << out.str();
        fail(name);
    }
    pass(name);
}

static void expect_throws(const char* name, const char* source) {
    std::ostringstream out;
    Runtime runtime(out);
    try {
        Vm vm(compile_with_builtins(source), runtime);
        vm.run();
        fail(name);
    } catch (...) {
        pass(name);
    }
}

int main() {
    expect_output("parallel_map ordered results",
                  "func double(x):\n    return x * 2\n"
                  "func hot():\n    return parallel_map([1, 2, 3, 4, 5, 6, 7, 8], double)\n"
                  "print(hot())\n",
                  "[2, 4, 6, 8, 10, 12, 14, 16]\n");

    if (pscript::parallel_map_last_peak_concurrency() < 2) {
        fail("parallel_map concurrency peak");
    }
    pass("parallel_map concurrency peak");

    expect_output("parallel_map empty",
                  "func id(x):\n    return x\nprint(parallel_map([], id))\n",
                  "[]\n");

    expect_throws("parallel_map rejects closure",
                  "func make():\n    c = 1\n    func add(x):\n        return x + c\n    return add\n"
                  "print(parallel_map([1], make()))\n");

    expect_throws("parallel_map worker error",
                  "func boom(x):\n    if x == 3:\n        throw \"bad\"\n    return x\n"
                  "func hot():\n    return parallel_map([1, 2, 3, 4], boom)\n"
                  "print(hot())\n");

    run_source("func id(x):\n    return x\nprint(parallel_map([1, 2], id))\n");
    run_source("func id(x):\n    return x\nprint(parallel_map([3, 4], id))\n");
    pass("parallel_map repeated calls");

    if (!pscript::parallel_callable_is_worker_safe(
            Runtime::make_null())) {  // null is not a safe worker callable
        pass("parallel_callable_is_worker_safe rejects non-function");
    } else {
        fail("parallel_callable_is_worker_safe rejects non-function");
    }

    std::cout << "All parallel_map tests passed\n";
    return 0;
}
