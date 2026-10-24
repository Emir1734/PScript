#include "cli/dev_cli.h"

#include "analysis/diagnostics.h"
#include "analysis/query_api.h"
#include "cli/exit_codes.h"
#include "cli/lib_commands.h"
#include "cli/package_commands.h"
#include "cli/registry_auth_commands.h"
#include "package/project.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "cli/pipeline.h"
#include "cli/profile_commands.h"
#include "cli/workspace_commands.h"
#include "runtime/cli_state.h"
#include "runtime/runtime.h"
#include "version.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace pscript {
namespace {

constexpr std::string_view kSubRun = "run";
constexpr std::string_view kSubCheck = "check";
constexpr std::string_view kSubInit = "init";
constexpr std::string_view kSubFmt = "fmt";
constexpr std::string_view kSubLint = "lint";
constexpr std::string_view kSubRepl = "repl";
constexpr std::string_view kSubProfile = "profile";

bool is_known_subcommand(std::string_view name) {
    return name == kSubRun || name == kSubCheck || name == kSubInit || name == kSubFmt || name == kSubLint ||
           name == kSubRepl || name == kSubProfile || name == "lib" || is_package_command(name) ||
           is_workspace_command(name) || is_registry_auth_command(name);
}

void configure_dev_cli(CliState& cli) {
    cli.set_help_text("PScript developer tools — run, check, init, fmt, lint, repl, package manager");
    cli.register_subcommand(std::string(kSubRun));
    cli.register_subcommand(std::string(kSubCheck));
    cli.register_subcommand(std::string(kSubInit));
    cli.register_subcommand(std::string(kSubFmt));
    cli.register_subcommand(std::string(kSubLint));
    cli.register_subcommand(std::string(kSubRepl));
    cli.register_subcommand(std::string(kSubProfile));
    cli.register_subcommand("add");
    cli.register_subcommand("remove");
    cli.register_subcommand("install");
    cli.register_subcommand("update");
    cli.register_subcommand("sync");
    cli.register_subcommand("package");
    cli.register_subcommand("pack");
    cli.register_subcommand("publish");
    cli.register_subcommand("list");
    cli.register_subcommand("tree");
    cli.register_subcommand("metadata");
    cli.register_subcommand("audit");
    cli.register_subcommand("lib");
    cli.register_subcommand("test");
    cli.register_subcommand("doctor");
    cli.register_subcommand("explain");
    cli.register_subcommand("register");
    cli.register_subcommand("login");
    cli.register_subcommand("logout");
    cli.register_subcommand("whoami");
    cli.register_subcommand("search");
    cli.register_option("dry-run", true, false, std::nullopt);
    cli.register_option("force", true, false, std::nullopt);
    cli.register_option("dev", true, false, std::nullopt);
}

int cmd_run(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err) {
    if (positionals.empty()) {
        print_cli_error(err, "run requires a .phs file path");
        return kExitUsage;
    }
    std::string path;
    std::string pgo_profile_path;
    std::vector<std::string> program_args;
    for (std::size_t i = 0; i < positionals.size(); ++i) {
        if (positionals[i] == "--pgo-profile" && i + 1 < positionals.size()) {
            pgo_profile_path = positionals[++i];
            continue;
        }
        if (path.empty()) {
            path = positionals[i];
            continue;
        }
        program_args.push_back(positionals[i]);
    }
    if (path.empty()) {
        print_cli_error(err, "run requires a .phs file path");
        return kExitUsage;
    }
    std::string source;
    if (!read_source_file(path, source, err)) {
        return kExitCompile;
    }
    ExecuteOptions options;
    options.pgo_profile_path = std::move(pgo_profile_path);
    return execute_source(std::move(source), path, program_args, out, err, options);
}

int print_analysis_diagnostics(const AnalysisResult& result, std::ostream& err) {
    for (const Diagnostic& diagnostic : result.diagnostics) {
        format_diagnostic(diagnostic, err);
    }
    return analysis_has_errors(result) ? kExitCompile : kExitSuccess;
}

int cmd_check(const std::vector<std::string>& positionals, std::ostream& err) {
    if (positionals.empty()) {
        print_cli_error(err, "check requires a .phs file path");
        return kExitUsage;
    }
    const std::string& path = positionals[0];
    std::string source;
    if (!read_source_file(path, source, err)) {
        return kExitCompile;
    }
    AnalysisOptions options;
    options.file = path;
    const AnalysisResult result = analyze_source(std::move(source), options);
    if (!result.parse_ok || !result.compile_ok) {
        return print_analysis_diagnostics(result, err);
    }
    return print_analysis_diagnostics(result, err);
}

int cmd_init(const std::vector<std::string>& positionals, std::ostream& err) {
    std::filesystem::path project_dir = std::filesystem::current_path();
    std::string project_name = "my_project";
    if (!positionals.empty()) {
        project_dir = std::filesystem::path(positionals[0]);
        project_name = project_dir.filename().string();
        if (project_dir.is_relative()) {
            project_dir = std::filesystem::current_path() / project_dir;
        }
    }
    std::error_code ec;
    if (!std::filesystem::exists(project_dir, ec)) {
        std::filesystem::create_directories(project_dir, ec);
        if (ec) {
            print_cli_error(err, "cannot create directory '" + project_dir.string() + "': " + ec.message());
            return kExitUsage;
        }
    } else if (!std::filesystem::is_directory(project_dir, ec)) {
        print_cli_error(err, "init target is not a directory: " + project_dir.string());
        return kExitUsage;
    }
    if (project_name.empty()) {
        project_name = "my_project";
    }
    std::string error;
    if (!package::scaffold_project(project_dir, project_name, &error)) {
        print_cli_error(err, error.empty() ? "init failed" : error);
        return kExitUsage;
    }
    return kExitSuccess;
}

int cmd_fmt(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err) {
    bool write_back = false;
    std::string path;
    for (std::size_t i = 0; i < positionals.size(); ++i) {
        if (positionals[i] == "--write" || positionals[i] == "-w") {
            write_back = true;
            continue;
        }
        if (path.empty()) {
            path = positionals[i];
        }
    }
    if (path.empty()) {
        print_cli_error(err, "fmt requires a .phs file path");
        return kExitUsage;
    }
    std::string source;
    if (!read_source_file(path, source, err)) {
        return kExitCompile;
    }
    AnalysisOptions options;
    options.file = path;
    options.run_linter = false;
    try {
        const std::string formatted = format_source(std::move(source), options);
        if (write_back) {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                print_cli_error(err, "cannot write " + path);
                return kExitCompile;
            }
            file << formatted;
        } else {
            out << formatted;
        }
        return kExitSuccess;
    } catch (const LexerError& ex) {
        print_compile_error(err, ex.message(), ex.line(), ex.column());
        return kExitCompile;
    } catch (const ParserError& ex) {
        print_compile_error(err, ex.message(), ex.line(), ex.column());
        return kExitCompile;
    }
}

int cmd_lint(const std::vector<std::string>& positionals, std::ostream& err) {
    if (positionals.empty()) {
        print_cli_error(err, "lint requires a .phs file path");
        return kExitUsage;
    }
    const std::string& path = positionals[0];
    std::string source;
    if (!read_source_file(path, source, err)) {
        return kExitCompile;
    }
    AnalysisOptions options;
    options.file = path;
    const AnalysisResult result = analyze_source(std::move(source), options);
    print_analysis_diagnostics(result, err);
    for (const Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            return kExitCompile;
        }
    }
    if (!result.parse_ok || !result.compile_ok) {
        return kExitCompile;
    }
    return kExitSuccess;
}

bool repl_looks_incomplete(const AnalysisResult& result) {
    for (const Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.code != DiagnosticCode::ParserError) {
            continue;
        }
        if (diagnostic.message.find("missing indent") != std::string::npos ||
            diagnostic.message.find("missing block") != std::string::npos) {
            return true;
        }
    }
    return false;
}

int cmd_repl(std::ostream& out, std::istream& in, std::ostream& err) {
    std::string accumulated;
    bool pending_block = false;
    while (true) {
        out << (pending_block ? "... " : ">>> ");
        out.flush();
        std::string line;
        if (!std::getline(in, line)) {
            break;
        }
        if (line == ".exit" || line == "exit" || line == "quit") {
            break;
        }
        if (line.empty() && !pending_block) {
            continue;
        }
        accumulated += line;
        accumulated += '\n';
        if (!line.empty() && line.back() == ':') {
            pending_block = true;
        }

        AnalysisOptions options;
        options.file = "<repl>";
        AnalysisResult analysis = analyze_source(accumulated, options);
        for (const Diagnostic& diagnostic : analysis.diagnostics) {
            if (diagnostic.severity != DiagnosticSeverity::Info) {
                format_diagnostic(diagnostic, err);
            }
        }
        if (!analysis.parse_ok) {
            if (repl_looks_incomplete(analysis) || pending_block) {
                continue;
            }
            accumulated.clear();
            pending_block = false;
            continue;
        }
        if (!analysis.compile_ok) {
            accumulated.resize(accumulated.size() - line.size() - 1);
            pending_block = false;
            continue;
        }
        pending_block = false;
        const int code = execute_source(accumulated, "<repl>", {}, out, err);
        if (code != kExitSuccess && code != kExitRuntime) {
            accumulated.resize(accumulated.size() - line.size() - 1);
        }
    }
    return kExitSuccess;
}

int legacy_run(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        print_dev_usage(err);
        return kExitUsage;
    }
    if (is_known_subcommand(args[0])) {
        print_cli_error(err, "missing command arguments for '" + args[0] + "'");
        return kExitUsage;
    }
    return cmd_run(args, out, err);
}

}  // namespace

void print_dev_usage(std::ostream& out) {
    out << "Usage: pscript <command> [options] [arguments...]\n"
           "       pscript <file.phs> [arguments...]   (legacy run)\n"
           "\n"
           "Commands:\n"
           "  run <file.phs> [args...]   Run a script\n"
           "  check <file.phs>           Syntax and compile-time validation (no execution)\n"
           "  init [name]                Create a project (pscript.toml, src/main.phs)\n"
           "  add <pkg> [constraint]     Add a dependency\n"
           "  remove <pkg>               Remove a dependency (manifest, lockfile, install)\n"
           "  install [--dev]            Resolve and install dependencies\n"
           "  update [pkg]               Re-resolve dependencies (one or all)\n"
           "  sync                       Verify installed packages match lockfile\n"
           "  audit                      Local dependency integrity audit (read-only)\n"
           "  package                    Build a source package under dist/\n"
           "  pack [--force]             Build package.tar.gz under dist/\n"
           "  publish [--dry-run] [token] Publish to registry (or validate only)\n"
           "  list                       List installed dependencies (from lockfile)\n"
           "  tree                       Show dependency tree (from lockfile)\n"
           "  metadata                   Show project and lockfile metadata\n"
           "  lib new <name> [dir]       Scaffold a library project\n"
           "  fmt [--write|-w] <file>    Format source (layout-preserving)\n"
           "  lint <file>                Lint and analyze source\n"
           "  repl                       Interactive read-eval-print loop\n"
           "  profile run <file> [-o out] [--report]   Run with runtime profiling\n"
           "  profile merge -o out a.psprof.json ...   Merge training profiles\n"
           "  profile report <file>      Print a human-readable profile report\n"
           "  run <file> [--pgo-profile path]            Run with profile-guided bytecode opts\n"
           "  test                       Run project tests under tests/\n"
           "  doctor                     Check runtime and project health (read-only)\n"
           "  explain <code>             Plain-language help for diagnostic codes\n"
           "\n"
           "Options:\n"
           "  --help, -h                 Show help\n"
           "  --version, -V              Show version\n";
}

CliRunResult dispatch_dev_cli(const std::vector<std::string>& args, std::ostream& out, std::ostream& err,
                              std::istream& in) {
    CliRunResult result;
    result.exit_code = kExitUsage;

    if (args.empty()) {
        print_dev_usage(err);
        return result;
    }

    if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) {
        print_dev_usage(out);
        result.exit_code = kExitSuccess;
        return result;
    }
    if (args.size() == 1 && (args[0] == "--version" || args[0] == "-V")) {
        out << kPscriptVersion << '\n';
        result.exit_code = kExitSuccess;
        return result;
    }

    Runtime parse_runtime(out, in, err);
    parse_runtime.set_program_args(args);
    CliState& cli = parse_runtime.cli_state();
    configure_dev_cli(cli);

    try {
        cli.ensure_parsed(parse_runtime);
    } catch (const RuntimeError& ex) {
        print_cli_error(err, ex.message());
        result.exit_code = kExitUsage;
        return result;
    }

    if (parse_runtime.process_exit_requested()) {
        result.exit_code = parse_runtime.process_exit_code();
        return result;
    }

    const std::optional<std::string>& command = cli.active_command();
    if (!command.has_value()) {
        result.exit_code = legacy_run(cli.positionals(), out, err);
        return result;
    }

    const std::vector<std::string>& positionals = cli.positionals();
    if (*command == kSubRun) {
        result.exit_code = cmd_run(positionals, out, err);
    } else if (*command == kSubCheck) {
        result.exit_code = cmd_check(positionals, err);
    } else if (*command == kSubInit) {
        result.exit_code = cmd_init(positionals, err);
    } else if (*command == kSubFmt) {
        result.exit_code = cmd_fmt(positionals, out, err);
    } else if (*command == kSubLint) {
        result.exit_code = cmd_lint(positionals, err);
    } else if (*command == kSubRepl) {
        result.exit_code = cmd_repl(out, in, err);
    } else if (*command == kSubProfile) {
        result.exit_code = run_profile_command(positionals, out, err);
    } else if (is_registry_auth_command(*command)) {
        result.exit_code = run_registry_auth_command(*command, positionals, out, err);
    } else if (is_package_command(*command)) {
        result.exit_code = run_package_command(*command, positionals, cli, out, err);
    } else if (*command == "lib") {
        result.exit_code = run_lib_command(positionals, out, err);
    } else if (*command == "test") {
        result.exit_code = run_test_command(positionals, out, err);
    } else if (*command == "doctor") {
        result.exit_code = run_doctor_command(positionals, out, err);
    } else if (*command == "explain") {
        result.exit_code = run_explain_command(positionals, out, err);
    } else {
        print_cli_error(err, "unknown command '" + *command + "'");
        result.exit_code = kExitUsage;
    }
    return result;
}

}  // namespace pscript
