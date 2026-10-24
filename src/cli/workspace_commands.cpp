#include "cli/workspace_commands.h"

#include "cli/exit_codes.h"
#include "cli/pipeline.h"
#include "package/checksum.h"
#include "package/lockfile.h"
#include "package/manifest.h"
#include "package/project.h"
#include "version.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string_view>

namespace pscript {
namespace {

constexpr std::string_view kCmdTest = "test";
constexpr std::string_view kCmdDoctor = "doctor";
constexpr std::string_view kCmdExplain = "explain";

void print_check(std::ostream& out, std::string_view status, std::string_view label, std::string_view detail) {
    out << status << "  " << label;
    if (!detail.empty()) {
        out << " — " << detail;
    }
    out << '\n';
}

std::vector<std::filesystem::path> collect_test_files(const std::filesystem::path& tests_dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::exists(tests_dir, ec) || !std::filesystem::is_directory(tests_dir, ec)) {
        return files;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(tests_dir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() == ".phs") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string relative_test_path(const std::filesystem::path& root, const std::filesystem::path& file) {
    std::error_code ec;
    const auto rel = std::filesystem::relative(file, root, ec);
    if (!ec) {
        return rel.lexically_normal().string();
    }
    return file.lexically_normal().string();
}

struct ExplainEntry {
    std::string_view code;
    std::string_view title;
    std::string_view body;
    std::string_view next_step;
};

const ExplainEntry* find_explain_entry(std::string_view code) {
    static constexpr ExplainEntry kEntries[] = {
        {"lexer",
         "Lexer error",
         "PScript could not turn the source file into tokens. This usually means invalid characters, an unclosed "
         "string, or another lexical mistake.",
         "Check the reported line and column. Run `pscript check <file>` for the same diagnostic."},
        {"parser",
         "Parser error",
         "The file tokens did not form valid PScript syntax. Common causes are missing colons after headers, bad "
         "indentation, or incomplete statements.",
         "Fix the syntax near the reported location, then run `pscript check <file>`."},
        {"compile",
         "Compiler error",
         "The program parsed but could not be compiled to bytecode. This often involves unsupported constructs or "
         "invalid use of language features.",
         "Read the compiler message and run `pscript check <file>`."},
        {"type",
         "Type error",
         "A value was used in a way that conflicts with PScript's type rules.",
         "Adjust the expression or add an explicit conversion where the language allows it."},
        {"name",
         "Name error",
         "A variable, function, or module name could not be resolved.",
         "Check spelling, imports, and that the symbol is defined before use."},
        {"unused",
         "Unused symbol",
         "A definition is never used. This is a lint-style warning from analysis.",
         "Remove the unused symbol or use it intentionally."},
        {"unreachable",
         "Unreachable code",
         "A statement cannot run because control flow never reaches it.",
         "Remove dead code or fix branching logic."},
        {"empty-expr",
         "Empty expression",
         "An expression is required here but the source is empty or incomplete.",
         "Fill in the missing expression."},
        {"lint",
         "Lint finding",
         "The linter reported a style or correctness note that is not a hard compile error.",
         "Review the message from `pscript lint <file>`."},
    };
    for (const ExplainEntry& entry : kEntries) {
        if (entry.code == code) {
            return &entry;
        }
    }
    return nullptr;
}

bool verify_locked_packages(const package::ProjectPaths& paths, const package::Lockfile& lockfile, std::string* detail) {
    for (const package::LockedPackage& entry : lockfile.packages) {
        std::error_code ec;
        if (!std::filesystem::exists(entry.install_dir, ec)) {
            if (detail) {
                *detail = "missing installed package: " + entry.name + " — run `pscript install`";
            }
            return false;
        }
        if (!entry.checksum.empty()) {
            std::string checksum_error;
            if (!package::verify_directory_checksum(entry.install_dir, entry.checksum, &checksum_error)) {
                if (detail) {
                    *detail = entry.name + ": " + (checksum_error.empty() ? "checksum mismatch" : checksum_error);
                }
                return false;
            }
        }
    }
    return true;
}

}  // namespace

bool is_workspace_command(std::string_view name) {
    return name == kCmdTest || name == kCmdDoctor || name == kCmdExplain;
}

int run_test_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err) {
    if (!positionals.empty()) {
        print_cli_error(err, "test does not accept arguments (run from the project root)");
        return kExitUsage;
    }
    const auto paths = package::find_project_root(std::filesystem::current_path());
    if (!paths) {
        print_cli_error(err, "no pscript.toml found in this directory or its parents — run `pscript init` to create a project");
        return kExitUsage;
    }

    const std::vector<std::filesystem::path> files = collect_test_files(paths->tests_dir);
    if (files.empty()) {
        out << "No test files found in tests/\n";
        out << "Add .phs files under tests/ with functions named test_* and use the testing module.\n";
        return kExitUsage;
    }

    std::size_t total_tests = 0;
    std::size_t files_with_tests = 0;
    bool had_failure = false;
    bool had_compile_error = false;

    for (const std::filesystem::path& file : files) {
        const std::string label = relative_test_path(paths->root, file);
        out << "==> " << label << '\n';
        std::ostringstream file_out;
        std::ostringstream file_err;
        const TestFileRunResult run = run_test_source_file(file.lexically_normal().string(), file_out, file_err);
        out << file_out.str();
        if (!file_err.str().empty()) {
            err << file_err.str();
        }
        if (run.exit_code == kExitCompile) {
            had_compile_error = true;
            had_failure = true;
            continue;
        }
        if (run.tests_discovered == 0) {
            out << "  (no test_* functions in this file)\n";
            continue;
        }
        files_with_tests += 1;
        total_tests += run.tests_discovered;
        if (run.exit_code != kExitSuccess) {
            had_failure = true;
        }
    }

    if (files_with_tests == 0) {
        out << "No test functions (test_*) found under tests/\n";
        out << "Define functions such as `func test_example():` and call assertions from the testing module.\n";
        return kExitUsage;
    }

    out << '\n' << total_tests << " test(s) in " << files_with_tests << " file(s)";
    if (had_failure) {
        out << " — failures detected\n";
        return had_compile_error ? kExitCompile : kExitRuntime;
    }
    out << " — all passed\n";
    return kExitSuccess;
}

int run_doctor_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err) {
    if (!positionals.empty()) {
        print_cli_error(err, "doctor does not accept arguments");
        return kExitUsage;
    }

    bool has_error = false;
    bool has_warn = false;

    print_check(out, "OK", "PScript runtime", std::string(kPscriptVersion));

    const auto paths = package::find_project_root(std::filesystem::current_path());
    if (!paths) {
        print_check(out, "WARN", "Project", "no pscript.toml in this directory or parents");
        has_warn = true;
        out << '\n';
        out << "Doctor finished with warnings.\n";
        return kExitSuccess;
    }

    print_check(out, "OK", "Project", paths->root.lexically_normal().string());

    std::string manifest_error;
    const auto manifest = package::load_manifest_file(paths->manifest.string(), &manifest_error);
    if (!manifest) {
        print_check(out, "ERROR", "Manifest", manifest_error.empty() ? "invalid pscript.toml" : manifest_error);
        has_error = true;
    } else {
        print_check(out, "OK", "Manifest", manifest->name + " " + manifest->version);
    }

    const std::filesystem::path main_file = paths->src_dir / "main.phs";
    std::error_code ec;
    if (std::filesystem::exists(main_file, ec)) {
        print_check(out, "OK", "Entry", "src/main.phs");
    } else {
        print_check(out, "WARN", "Entry", "src/main.phs is missing");
        has_warn = true;
    }

    if (!std::filesystem::exists(paths->lockfile, ec)) {
        if (manifest && !manifest->dependencies.empty()) {
            print_check(out, "WARN", "Lockfile", "pscript.lock missing — run `pscript install`");
            has_warn = true;
        } else {
            print_check(out, "OK", "Lockfile", "not required yet");
        }
    } else {
        std::string lock_error;
        const auto lock = package::load_lockfile(paths->lockfile.string(), &lock_error);
        if (!lock) {
            print_check(out, "ERROR", "Lockfile", lock_error.empty() ? "could not read pscript.lock" : lock_error);
            has_error = true;
        } else {
            print_check(out, "OK", "Lockfile", std::to_string(lock->packages.size()) + " locked package(s)");
            std::string deps_detail;
            if (!verify_locked_packages(*paths, *lock, &deps_detail)) {
                print_check(out, "ERROR", "Dependencies", deps_detail);
                has_error = true;
            } else if (lock->packages.empty() && manifest && !manifest->dependencies.empty()) {
                print_check(out, "WARN", "Dependencies", "lockfile has no packages — run `pscript install`");
                has_warn = true;
            } else {
                print_check(out, "OK", "Dependencies", "installed packages match lockfile");
            }
        }
    }

    out << '\n';
    if (has_error) {
        out << "Doctor found blocking problems.\n";
        return kExitRuntime;
    }
    if (has_warn) {
        out << "Doctor finished with warnings.\n";
        return kExitSuccess;
    }
    out << "Doctor reports no issues.\n";
    return kExitSuccess;
}

int run_explain_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err) {
    if (positionals.empty()) {
        print_cli_error(err, "explain requires a diagnostic code (for example: parser)");
        out << "Usage: pscript explain <code>\n"
               "Codes: lexer, parser, compile, type, name, unused, unreachable, empty-expr, lint\n";
        return kExitUsage;
    }
    if (positionals.size() > 1) {
        print_cli_error(err, "explain accepts a single diagnostic code");
        return kExitUsage;
    }
    const std::string& code = positionals[0];
    const ExplainEntry* entry = find_explain_entry(code);
    if (entry == nullptr) {
        print_cli_error(err, "unknown diagnostic code '" + code + "'");
        out << "Usage: pscript explain <code>\n"
               "Codes: lexer, parser, compile, type, name, unused, unreachable, empty-expr, lint\n";
        return kExitUsage;
    }
    out << entry->title << " [" << entry->code << "]\n\n";
    out << entry->body << "\n\n";
    out << "Next step: " << entry->next_step << '\n';
    return kExitSuccess;
}

}  // namespace pscript
