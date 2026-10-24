#include "cli/profile_commands.h"

#include "cli/exit_codes.h"
#include "cli/pipeline.h"
#include "compiler/pgo.h"
#include "runtime/profiler.h"

#include <filesystem>
#include <fstream>
#include <string_view>

namespace pscript {
namespace {

int cmd_profile_run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        print_cli_error(err, "profile run requires a .phs file path");
        return kExitUsage;
    }

    std::string path;
    std::string output_path;
    bool print_report = false;
    std::vector<std::string> program_args;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--report") {
            print_report = true;
            continue;
        }
        if ((args[i] == "-o" || args[i] == "--output") && i + 1 < args.size()) {
            output_path = args[++i];
            continue;
        }
        if (path.empty()) {
            path = args[i];
            continue;
        }
        program_args.push_back(args[i]);
    }
    if (path.empty()) {
        print_cli_error(err, "profile run requires a .phs file path");
        return kExitUsage;
    }
    if (output_path.empty()) {
        const std::filesystem::path source(path);
        output_path = (source.parent_path() / (source.stem().string() + ".psprof.json")).string();
    }

    ExecuteOptions options;
    options.enable_profiling = true;
    options.profile_output_path = output_path;
    options.print_profile_report = print_report;

    std::string source;
    if (!read_source_file(path, source, err)) {
        return kExitCompile;
    }
    return execute_source(std::move(source), path, program_args, out, err, options);
}

int cmd_profile_merge(const std::vector<std::string>& args, std::ostream& err) {
    if (args.size() < 2) {
        print_cli_error(err, "profile merge requires -o <out> and at least one input profile");
        return kExitUsage;
    }
    std::string output_path;
    std::vector<std::string> inputs;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "-o" || args[i] == "--output") && i + 1 < args.size()) {
            output_path = args[++i];
            continue;
        }
        inputs.push_back(args[i]);
    }
    if (output_path.empty() || inputs.empty()) {
        print_cli_error(err, "profile merge requires -o <out> and input profile paths");
        return kExitUsage;
    }
    std::vector<ProfileDocument> profiles;
    for (const std::string& path : inputs) {
        std::string error;
        const std::optional<ProfileDocument> document = load_profile_document(path, error);
        if (!document.has_value()) {
            print_cli_error(err, error.empty() ? "failed to load profile: " + path : error);
            return kExitUsage;
        }
        profiles.push_back(*document);
    }
    std::string merge_error;
    const std::optional<ProfileDocument> merged = merge_profile_documents(profiles, merge_error);
    if (!merged.has_value()) {
        print_cli_error(err, merge_error.empty() ? "profile merge failed" : merge_error);
        return kExitUsage;
    }
    std::string save_error;
    if (!save_profile_document(*merged, output_path, save_error)) {
        print_cli_error(err, save_error);
        return kExitRuntime;
    }
    return kExitSuccess;
}

int cmd_profile_report(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        print_cli_error(err, "profile report requires a profile file path");
        return kExitUsage;
    }
    std::string error;
    const std::optional<ProfileDocument> document = load_profile_document(args[0], error);
    if (!document.has_value()) {
        print_cli_error(err, error.empty() ? "failed to load profile" : error);
        return kExitUsage;
    }
    write_profile_report(*document, out);
    return kExitSuccess;
}

}  // namespace

int run_profile_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err) {
    if (positionals.empty()) {
        print_cli_error(err, "profile requires a subcommand: run | merge | report");
        return kExitUsage;
    }
    const std::string& sub = positionals[0];
    std::vector<std::string> rest;
    if (positionals.size() > 1) {
        rest.assign(positionals.begin() + 1, positionals.end());
    }
    if (sub == "run") {
        return cmd_profile_run(rest, out, err);
    }
    if (sub == "report") {
        return cmd_profile_report(rest, out, err);
    }
    if (sub == "merge") {
        return cmd_profile_merge(rest, err);
    }
    print_cli_error(err, "unknown profile subcommand '" + sub + "'");
    return kExitUsage;
}

}  // namespace pscript
