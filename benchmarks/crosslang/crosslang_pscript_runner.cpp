#include "compiler/compiler.h"
#include "jit/jit_config.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include "crosslang_bench.h"

#include <iostream>
#include <sstream>
#include <string>

namespace {

const char kDirectIncCallSource[] =
    "func inc(x):\n"
    "    return x + 1\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + inc(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

const char kNestedCallSource[] =
    "func leaf(x):\n"
    "    return x\n"
    "func mid(x):\n"
    "    return leaf(x) + leaf(x + 1)\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + mid(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

const char kClosureCallSource[] =
    "func make_adder(base):\n"
    "    func add(x):\n"
    "        return base + x\n"
    "    return add\n"
    "func bench(n):\n"
    "    callee = make_adder(1)\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + callee(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

const char kAddConstantSource[] =
    "func add7(x):\n"
    "    return x + 7\n"
    "func bench(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + add7(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

const char kIndirectCallSource[] =
    "func inc(x):\n"
    "    return x + 1\n"
    "func bench(n):\n"
    "    callee = inc\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + callee(i)\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

const char kIntLoopSource[] =
    "func int_loop(n):\n"
    "    sum = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        sum = sum + i\n"
    "        i = i + 1\n"
    "    return sum\n"
    "print(int_loop(ITERS))\n";

const char kStringEqSource[] =
    "func seq(n):\n"
    "    hits = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        if \"needle\" == \"needle\":\n"
    "            hits = hits + 1\n"
    "        if \"abc\" != \"xyz\":\n"
    "            hits = hits + 1\n"
    "        i = i + 1\n"
    "    return hits\n"
    "print(seq(ITERS))\n";

const char kStringConcatSource[] =
    "func swork(n):\n"
    "    s = \"\"\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        s = s + \"x\"\n"
    "        i = i + 1\n"
    "    return s.length()\n"
    "print(swork(ITERS))\n";

const char kListIndexSource[] =
    "func list_index(n):\n"
    "    items = []\n"
    "    i = 0\n"
    "    while i < 256:\n"
    "        items.append(i)\n"
    "        i = i + 1\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        j = 0\n"
    "        while j < 256:\n"
    "            total = total + items[j]\n"
    "            j = j + 1\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(list_index(ITERS))\n";

const char kArithmeticSource[] =
    "func acc(n):\n"
    "    total = 0\n"
    "    i = 0\n"
    "    while i < n:\n"
    "        total = total + i\n"
    "        i = i + 1\n"
    "    return total\n"
    "print(acc(ITERS))\n";

const char kDictLookupSource[] =
    "func dict_lookup(n):\n"
    "    m = {}\n"
    "    i = 0\n"
    "    while i < 512:\n"
    "        m[\"k\" + str(i)] = i\n"
    "        i = i + 1\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        j = 0\n"
    "        while j < 512:\n"
    "            total = total + m[\"k\" + str(j)]\n"
    "            j = j + 1\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(dict_lookup(ITERS))\n";

const char kJsonParseSource[] =
    "import json\n"
    "func bench(n):\n"
    "    payload = \"{\\\"items\\\":[\"\n"
    "    i = 0\n"
    "    while i < 500:\n"
    "        if i > 0:\n"
    "            payload = payload + \",\"\n"
    "        payload = payload + str(i)\n"
    "        i = i + 1\n"
    "    payload = payload + \"]}\"\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        doc = json.decode(payload)\n"
    "        total = total + doc[\"items\"].length()\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

const char kJsonStringifySource[] =
    "import json\n"
    "func bench(n):\n"
    "    items = []\n"
    "    i = 0\n"
    "    while i < 200:\n"
    "        items.append(i)\n"
    "        i = i + 1\n"
    "    doc = {\"items\": items}\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        encoded = json.encode(doc)\n"
    "        total = total + encoded.length()\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

const char kJsonPipelineSource[] =
    "import json\n"
    "func bench(n):\n"
    "    payload = \"{\\\"items\\\":[\"\n"
    "    i = 0\n"
    "    while i < 500:\n"
    "        if i > 0:\n"
    "            payload = payload + \",\"\n"
    "        payload = payload + str(i)\n"
    "        i = i + 1\n"
    "    payload = payload + \"]}\"\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        doc = json.decode(payload)\n"
    "        sum = 0\n"
    "        for v in doc[\"items\"]:\n"
    "            sum = sum + v\n"
    "        encoded = json.encode(doc)\n"
    "        total = total + sum + encoded.length()\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

const char kMixedRuntimeSource[] =
    "import json\n"
    "func bench(n):\n"
    "    payload = \"{\\\"items\\\":[\"\n"
    "    i = 0\n"
    "    while i < 500:\n"
    "        if i > 0:\n"
    "            payload = payload + \",\"\n"
    "        payload = payload + str(i)\n"
    "        i = i + 1\n"
    "    payload = payload + \"]}\"\n"
    "    total = 0\n"
    "    rep = 0\n"
    "    while rep < n:\n"
    "        doc = json.decode(payload)\n"
    "        sum = 0\n"
    "        for v in doc[\"items\"]:\n"
    "            sum = sum + v\n"
    "        encoded = json.encode(doc)\n"
    "        doc2 = json.decode(encoded)\n"
    "        total = total + sum + encoded.length() + doc2[\"items\"].length()\n"
    "        rep = rep + 1\n"
    "    return total\n"
    "print(bench(ITERS))\n";

std::string with_iters(std::uint64_t iters, const char* template_source) {
    std::string source = template_source;
    const std::string token = "ITERS";
    const std::size_t pos = source.find(token);
    if (pos != std::string::npos) {
        source.replace(pos, token.size(), std::to_string(iters));
    }
    return source;
}

std::int64_t parse_last_line_int(const std::string& output) {
    std::istringstream in(output);
    std::string line;
    std::string last;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            last = line;
        }
    }
    return std::stoll(last);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view workload = crosslang::workload_name(argc, argv);
    const char* source_template = nullptr;
    const std::uint64_t default_iters = crosslang::default_workload_iterations(workload);
    if (workload == "int_loop") {
        source_template = kIntLoopSource;
    } else if (workload == "list_index") {
        source_template = kListIndexSource;
    } else if (workload == "string_eq") {
        source_template = kStringEqSource;
    } else if (workload == "string_concat") {
        source_template = kStringConcatSource;
    } else if (workload == "direct_inc_call") {
        source_template = kDirectIncCallSource;
    } else if (workload == "nested_call") {
        source_template = kNestedCallSource;
    } else if (workload == "closure_call") {
        source_template = kClosureCallSource;
    } else if (workload == "add_constant") {
        source_template = kAddConstantSource;
    } else if (workload == "indirect_call") {
        source_template = kIndirectCallSource;
    } else if (workload == "arithmetic") {
        source_template = kArithmeticSource;
    } else if (workload == "dict_lookup") {
        source_template = kDictLookupSource;
    } else if (workload == "json_parse") {
        source_template = kJsonParseSource;
    } else if (workload == "json_stringify") {
        source_template = kJsonStringifySource;
    } else if (workload == "json_pipeline") {
        source_template = kJsonPipelineSource;
    } else if (workload == "mixed_runtime") {
        source_template = kMixedRuntimeSource;
    } else {
        std::cerr << "unsupported workload: " << workload << '\n';
        return 2;
    }
    const std::uint64_t iters = crosslang::bench_iterations(argc, argv, default_iters);

    std::string mode = "optimizing";
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--tier" && i + 1 < argc) {
            mode = argv[i + 1];
        }
    }

    pscript::CompileOptions options;
    options.source_file = "<crosslang>";
    const pscript::Bytecode bytecode_template =
        pscript::compile(pscript::parse(with_iters(iters, source_template)), options);

    std::int64_t checksum = 0;
    const double median = crosslang::median_ms(
        [&] {
            std::ostringstream out;
            pscript::Runtime runtime(out);
            pscript::Bytecode bytecode = bytecode_template;
            pscript::Vm vm(std::move(bytecode), runtime);
            vm.set_jit_enabled(mode != "vm");
            pscript::jit::JitTieringConfig config;
            config.baseline_invocation_threshold = 1;
            config.optimizing_invocation_threshold =
                mode == "baseline" ? 1'000'000 : 1;
            config.require_profile_for_optimizing = mode != "optimizing";
            vm.set_jit_config(config);
            vm.run();
            checksum = parse_last_line_int(out.str());
        },
        7);

    const char* label = mode == "vm" ? "pscript_vm" : (mode == "baseline" ? "pscript_baseline" : "pscript_opt");
    crosslang::print_timing(label, workload, median, checksum);
    return 0;
}
