#include "cli/cli.h"

#include "cli/dev_cli.h"
#include "cli/exit_codes.h"
#include "cli/pipeline.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <iostream>
#include <string>
#include <vector>

namespace pscript {
namespace {

void configure_utf8_stdio() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    auto enable_vt = [](DWORD std_handle) {
        const HANDLE handle = GetStdHandle(std_handle);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
            return;
        }
        DWORD mode = 0;
        if (!GetConsoleMode(handle, &mode)) {
            return;
        }
        mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        SetConsoleMode(handle, mode);
    };
    enable_vt(STD_OUTPUT_HANDLE);
    enable_vt(STD_ERROR_HANDLE);
#endif
}

}  // namespace

CliRunResult run_cli_with_args(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    return run_cli_with_args(args, out, err, std::cin);
}

CliRunResult run_cli_with_args(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                               std::istream& in) {
    return dispatch_dev_cli(args, out, err, in);
}

int run_cli(int argc, char** argv) {
    configure_utf8_stdio();

    if (argc < 2) {
        print_dev_usage(std::cerr);
        return kExitUsage;
    }

    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const CliRunResult result = run_cli_with_args(args, std::cout, std::cerr);
    return result.exit_code;
}

}  // namespace pscript
