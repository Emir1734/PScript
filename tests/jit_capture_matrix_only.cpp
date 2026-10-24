#include "compiler/compiler.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <iostream>
#include <sstream>

int main() {
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
        "print(shared(2))\n";
    pscript::CompileOptions options;
    options.source_file = "<test>";
    const pscript::Bytecode bc = pscript::compile(pscript::parse(source), options);
    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;
    config.optimizing_invocation_threshold = 1;
    config.require_profile_for_optimizing = false;
    std::ostringstream out;
    pscript::Runtime rt(out);
    pscript::Vm vm(bc, rt);
    vm.set_jit_enabled(true);
    vm.set_jit_config(config);
    vm.run();
    return 0;
}
