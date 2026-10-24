#include "compiler/compiler.h"
#include "parser/parser.h"
#include "jit/jit_config.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <cstdio>
#include <sstream>
#include <string>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

static int run_top_loop(int n) {
    const std::string source =
        "func leaf(x):\n"
        "    return x + 1\n"
        "func mid(x):\n"
        "    return leaf(x)\n"
        "func top(x):\n"
        "    total = 0\n"
        "    i = 0\n"
        "    while i < x:\n"
        "        total = total + mid(i)\n"
        "        i = i + 1\n"
        "    return total\n"
        "print(top(" +
        std::to_string(n) + "))\n";

    pscript::CompileOptions options;
    options.source_file = "<repro>";
    pscript::Bytecode bc = pscript::compile(pscript::parse(source), options);

    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;

    std::ostringstream out;
    pscript::Runtime rt(out);
    pscript::Vm vm(std::move(bc), rt);
    vm.set_jit_enabled(true);
    vm.set_jit_config(config);
    vm.run();
    std::fprintf(stderr, "output=%s", out.str().c_str());
    return 0;
}

#if defined(_WIN32)
static LONG WINAPI av_veh(PEXCEPTION_POINTERS info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    std::fprintf(stderr, "EXCEPTION_ACCESS_VIOLATION\n");
    std::fprintf(stderr, "fault_ip=%p\n", info->ExceptionRecord->ExceptionAddress);
    if (info->ExceptionRecord->NumberParameters >= 2) {
        std::fprintf(stderr, "access_type=%llu\n",
                     static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[0]));
        std::fprintf(stderr, "access_addr=%p\n",
                     reinterpret_cast<void*>(info->ExceptionRecord->ExceptionInformation[1]));
    }
    std::fprintf(stderr, "rsp=%p rcx=%p rdx=%p r8=%p r9=%p r10=%p r15=%p\n",
                 reinterpret_cast<void*>(info->ContextRecord->Rsp),
                 reinterpret_cast<void*>(info->ContextRecord->Rcx),
                 reinterpret_cast<void*>(info->ContextRecord->Rdx),
                 reinterpret_cast<void*>(info->ContextRecord->R8),
                 reinterpret_cast<void*>(info->ContextRecord->R9),
                 reinterpret_cast<void*>(info->ContextRecord->R10),
                 reinterpret_cast<void*>(info->ContextRecord->R15));
    const auto* ip = static_cast<const std::uint8_t*>(info->ExceptionRecord->ExceptionAddress);
    std::fprintf(stderr, "insn_bytes=");
    for (int i = -8; i < 16; ++i) {
        std::fprintf(stderr, "%02x ", ip[i]);
    }
    std::fprintf(stderr, "\n");
    if (info->ContextRecord->R15 != 0) {
        const auto* state = reinterpret_cast<const std::uint8_t*>(info->ContextRecord->R15);
        std::fprintf(stderr, "state_fn_index=%u current_insn=%u\n",
                     *reinterpret_cast<const std::uint32_t*>(state + 12),
                     *reinterpret_cast<const std::uint32_t*>(state + 4));
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv) {
    int n = 2;
    if (argc >= 2) {
        n = std::atoi(argv[1]);
    }

#if defined(_WIN32)
    PVOID veh = AddVectoredExceptionHandler(1, av_veh);
#endif

    const int rc = run_top_loop(n);

#if defined(_WIN32)
    RemoveVectoredExceptionHandler(veh);
#endif
    return rc;
}
