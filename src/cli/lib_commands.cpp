#include "cli/lib_commands.h"

#include "cli/exit_codes.h"
#include "cli/pipeline.h"
#include "package/package_identity.h"
#include "package/project.h"

#include <filesystem>

namespace pscript {
namespace {

int cmd_lib_new(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.size() < 2) {
        print_cli_error(err, "lib new requires a package name");
        out << "Usage: pscript lib new <name> [directory]\n";
        return kExitUsage;
    }
    const std::string& name = args[1];
    std::filesystem::path directory = std::filesystem::current_path();
    if (args.size() > 2) {
        directory = std::filesystem::path(args[2]);
        if (directory.is_relative()) {
            directory = std::filesystem::current_path() / directory;
        }
    } else {
        directory /= package::install_dir_basename(name);
    }
    std::error_code ec;
    if (std::filesystem::exists(directory, ec) && !std::filesystem::is_empty(directory, ec)) {
        if (std::filesystem::exists(directory / "pscript.toml", ec)) {
            print_cli_error(err, "refusing to overwrite existing project at " + directory.lexically_normal().string());
            return kExitUsage;
        }
    }
    std::string error;
    if (!package::scaffold_library_project(directory, name, &error)) {
        print_cli_error(err, error.empty() ? "lib new failed" : error);
        return kExitCompile;
    }
    const std::string module_file = "src/" + package::install_dir_basename(name) + ".phs";
    out << "Created library project at " << directory.lexically_normal().string() << '\n';
    out << "  pscript.toml\n";
    out << "  " << module_file << '\n';
    out << "  tests/\n";
    out << '\n';
    out << "Next steps:\n";
    out << "  cd " << directory.filename().string() << '\n';
    out << "  pscript check " << module_file << '\n';
    out << "  pscript pack\n";
    return kExitSuccess;
}

}  // namespace

int run_lib_command(const std::vector<std::string>& positionals, std::ostream& out, std::ostream& err) {
    if (positionals.empty()) {
        print_cli_error(err, "lib requires a subcommand (try: lib new)");
        out << "Usage: pscript lib new <name> [directory]\n";
        return kExitUsage;
    }
    if (positionals[0] == "new") {
        return cmd_lib_new(positionals, out, err);
    }
    print_cli_error(err, "unknown lib subcommand '" + positionals[0] + "'");
    out << "Usage: pscript lib new <name> [directory]\n";
    return kExitUsage;
}

}  // namespace pscript
