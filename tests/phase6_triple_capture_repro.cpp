#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <cstdlib>
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
using pscript::parse;

namespace {

Bytecode compile_source(const std::string& source) {
    CompileOptions options;
    options.source_file = "<test>";
    return compile(parse(source), options);
}

ProfileDocument hot_profile(const Bytecode& bc, const std::string& fn) {
    ProfileDocument doc;
    doc.version = ProfileDocument::kFormatVersion;
    doc.pgo_compat_version = pscript::kPgoCompatVersion;
    doc.program_fingerprint = pscript::compute_program_fingerprint(bc);
    for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
        if (bc.functions[i].name == fn) {
            pscript::ProfileFunctionRecord rec;
            rec.function_index = i;
            rec.calls = 50'000;
            rec.hotness = ProfileHotness::Hot;
            doc.functions.push_back(rec);
            break;
        }
    }
    return doc;
}

}  // namespace

static int run_once(bool jit) {
    const std::string source =
        "func outer(a, b, c):\n"
        "    func inner(z):\n"
        "        return a + b + c + z\n"
        "    return inner\n"
        "func main(n):\n"
        "    f = outer(1, 2, 3)\n"
        "    total = 0\n"
        "    i = 0\n"
        "    while i < n:\n"
        "        total = total + f(i)\n"
        "        i = i + 1\n"
        "    return total\n"
        "print(main(8))\n";

    try {
        Bytecode bc = compile_source(source);
        ProfileDocument profile = hot_profile(bc, "main");
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
        const std::string expected = "76\n";
        if (out.str() != expected) {
            std::cerr << (jit ? "jit " : "vm ") << "unexpected output got=" << out.str() << " expected=" << expected
                      << '\n';
            return 2;
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << (jit ? "jit " : "vm ") << "exception: " << ex.what() << '\n';
        return 3;
    }
}

int main() {
    if (run_once(false) != 0) {
        return 1;
    }
    if (run_once(true) != 0) {
        return 1;
    }
    std::cout << "OK triple capture vm and optimizing jit\n";
    return 0;
}
