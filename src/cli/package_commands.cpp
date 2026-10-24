#include "cli/package_commands.h"

#include "cli/exit_codes.h"
#include "cli/pipeline.h"
#include "runtime/cli_state.h"
#include "package/install.h"
#include "package/lockfile.h"
#include "package/manifest.h"
#include "package/package_identity.h"
#include "package/packaging.h"
#include "package/project.h"
#include "package/registry.h"
#include "package/registry_credentials.h"
#include "package/semver.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>

namespace pscript {
namespace {

constexpr std::string_view kCmdAdd = "add";
constexpr std::string_view kCmdRemove = "remove";
constexpr std::string_view kCmdInstall = "install";
constexpr std::string_view kCmdUpdate = "update";
constexpr std::string_view kCmdSync = "sync";
constexpr std::string_view kCmdPackage = "package";
constexpr std::string_view kCmdPack = "pack";
constexpr std::string_view kCmdPublish = "publish";
constexpr std::string_view kCmdList = "list";
constexpr std::string_view kCmdTree = "tree";
constexpr std::string_view kCmdMetadata = "metadata";
constexpr std::string_view kCmdAudit = "audit";

std::optional<package::ProjectPaths> project_in_cwd(std::ostream& err) {
    const auto paths = package::find_project_root(std::filesystem::current_path());
    if (!paths) {
        print_cli_error(err, "no pscript.toml found in current directory or parents");
        return std::nullopt;
    }
    return paths;
}

int cmd_add(const std::vector<std::string>& args, std::ostream& err) {
    if (args.empty()) {
        print_cli_error(err, "add requires a package name and optional version constraint");
        return kExitUsage;
    }
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    std::string error;
    auto manifest = package::load_manifest_file(paths->manifest.string(), &error);
    if (!manifest) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    package::DependencySpec spec;
    std::string constraint_text = args.size() > 1 ? args[1] : "^0.0.0";
    if (const std::optional<package::RegistryDependencyRef> scoped_ref =
            package::parse_registry_dependency_arg(args[0], &error)) {
        spec.name = scoped_ref->canonical_name;
        constraint_text = scoped_ref->version_constraint;
    } else {
        spec.name = args[0];
    }
    const auto constraint = package::VersionConstraint::parse(constraint_text);
    if (!constraint) {
        print_cli_error(err, "invalid version constraint: " + constraint_text);
        return kExitUsage;
    }
    spec.constraint = *constraint;
    spec.source = package::DependencySourceKind::Registry;
    manifest->dependencies[spec.name] = spec;
    if (!package::write_manifest_file(paths->manifest.string(), *manifest, &error)) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    return kExitSuccess;
}

int cmd_remove(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.empty()) {
        print_cli_error(err, "remove requires a package name");
        return kExitUsage;
    }
    if (args.size() > 1) {
        print_cli_error(err, "remove accepts a single package name");
        return kExitUsage;
    }
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    std::string error;
    if (!package::remove_project_dependency(*paths, args[0], &error)) {
        if (error.find("not found") != std::string::npos) {
            print_cli_error(err, error);
            return kExitUsage;
        }
        print_cli_error(err, error.empty() ? "remove failed" : error);
        return kExitCompile;
    }
    out << "Removed dependency " << args[0] << '\n';
    return kExitSuccess;
}

int cmd_install(const std::vector<std::string>& args, CliState& cli, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    package::InstallOptions options;
    options.include_dev_dependencies = cli.flag("dev");
    for (const std::string& arg : args) {
        if (arg == "--dev") {
            options.include_dev_dependencies = true;
        } else if (!arg.empty() && arg[0] == '-') {
            print_cli_error(err, "install does not accept option '" + arg + "'");
            return kExitUsage;
        }
    }
    std::string error;
    if (!package::install_project(*paths, options, &error)) {
        print_cli_error(err, error.empty() ? "install failed" : error);
        return kExitCompile;
    }
    return kExitSuccess;
}

int cmd_sync(std::ostream& out, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    std::string error;
    const auto manifest = package::load_manifest_file(paths->manifest.string(), &error);
    if (!manifest) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    std::error_code ec;
    if (!std::filesystem::exists(paths->lockfile, ec)) {
        print_cli_error(err, "pscript.lock is missing — run `pscript install` first");
        return kExitCompile;
    }
    const auto lock_before = package::load_lockfile(paths->lockfile.string(), &error);
    if (!lock_before) {
        print_cli_error(err, error.empty() ? "cannot read pscript.lock" : error);
        return kExitCompile;
    }
    if (!package::sync_project(*paths, &error)) {
        print_cli_error(err, error.empty() ? "sync failed" : error);
        return kExitCompile;
    }
    out << "Installed packages match pscript.lock\n";
    for (const auto& [name, spec] : manifest->dependencies) {
        if (package::find_locked_package(*lock_before, name) == nullptr) {
            out << "WARN  manifest dependency not in lockfile: " << name << '\n';
        }
    }
    return kExitSuccess;
}

int cmd_update(const std::vector<std::string>& args, std::ostream& out, std::ostream& err) {
    if (args.size() > 1) {
        print_cli_error(err, "update accepts at most one package name");
        return kExitUsage;
    }
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    std::string error;
    const auto manifest = package::load_manifest_file(paths->manifest.string(), &error);
    if (!manifest) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    std::optional<package::Lockfile> before;
    std::error_code ec;
    if (std::filesystem::exists(paths->lockfile, ec)) {
        before = package::load_lockfile(paths->lockfile.string(), &error);
        if (!before) {
            print_cli_error(err, error.empty() ? "cannot read pscript.lock" : error);
            return kExitCompile;
        }
    }
    if (!args.empty()) {
        const std::string& name = args[0];
        if (manifest->dependencies.count(name) == 0 && manifest->dev_dependencies.count(name) == 0) {
            print_cli_error(err, "dependency not found in pscript.toml: " + name);
            return kExitUsage;
        }
    }
    package::InstallOptions options;
    options.include_dev_dependencies = true;
    if (!package::install_project(*paths, options, &error)) {
        print_cli_error(err, error.empty() ? "update failed" : error);
        return kExitCompile;
    }
    const auto after = package::load_lockfile(paths->lockfile.string(), &error);
    if (!after) {
        print_cli_error(err, error.empty() ? "update succeeded but lockfile is unreadable" : error);
        return kExitCompile;
    }
    if (!args.empty()) {
        const std::string& name = args[0];
        const package::LockedPackage* old_entry =
            before.has_value() ? package::find_locked_package(*before, name) : nullptr;
        const package::LockedPackage* new_entry = package::find_locked_package(*after, name);
        if (new_entry == nullptr) {
            print_cli_error(err, "dependency was not resolved after update: " + name);
            return kExitCompile;
        }
        const std::string old_version = old_entry != nullptr ? old_entry->version : "(none)";
        out << name << ": " << old_version << " -> " << new_entry->version << '\n';
    } else {
        out << "Updated " << after->packages.size() << " locked package(s)\n";
    }
    return kExitSuccess;
}

void print_audit_line(std::ostream& out, const package::DependencyAuditFinding& finding) {
    switch (finding.level) {
        case package::DependencyAuditLevel::Error:
            out << "ERROR ";
            break;
        case package::DependencyAuditLevel::Warn:
            out << "WARN  ";
            break;
        case package::DependencyAuditLevel::Info:
        default:
            out << "INFO  ";
            break;
    }
    out << finding.message << '\n';
}

int cmd_audit(std::ostream& out, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    const package::DependencyAuditReport report = package::audit_project_dependencies(*paths);
    for (const package::DependencyAuditFinding& finding : report.findings) {
        print_audit_line(out, finding);
    }
    if (report.has_errors()) {
        out << '\n';
        out << "Audit found blocking problems.\n";
        return kExitRuntime;
    }
    if (report.has_warnings()) {
        out << '\n';
        out << "Audit finished with warnings.\n";
        return kExitSuccess;
    }
    out << '\n';
    out << "Audit reports no issues.\n";
    return kExitSuccess;
}

int cmd_package(std::ostream& out, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    std::string error;
    const auto manifest = package::load_manifest_file(paths->manifest.string(), &error);
    if (!manifest) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    const std::filesystem::path dist = paths->root / "dist" / (manifest->name + "-" + manifest->version);
    std::error_code ec;
    std::filesystem::remove_all(dist, ec);
    std::filesystem::create_directories(dist, ec);
    std::filesystem::copy(paths->manifest, dist / "pscript.toml", std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (std::filesystem::exists(paths->root / "README.md")) {
        std::filesystem::copy(paths->root / "README.md", dist / "README.md",
                              std::filesystem::copy_options::overwrite_existing, ec);
    }
    if (std::filesystem::exists(paths->src_dir)) {
        std::filesystem::copy(paths->src_dir, dist / "src",
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              ec);
    }
    if (ec) {
        print_cli_error(err, ec.message());
        return kExitCompile;
    }
    out << dist.string() << '\n';
    return kExitSuccess;
}

std::string dependency_source_label(package::DependencySourceKind kind) {
    switch (kind) {
        case package::DependencySourceKind::Path:
            return "path";
        case package::DependencySourceKind::Git:
            return "git";
        case package::DependencySourceKind::Registry:
        default:
            return "registry";
    }
}

bool field_looks_sensitive(std::string_view key) {
    std::string lower(key);
    for (char& ch : lower) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return lower.find("token") != std::string::npos || lower.find("password") != std::string::npos ||
           lower.find("secret") != std::string::npos;
}

void print_locked_row(std::ostream& out, const package::LockedPackage& entry) {
    std::error_code ec;
    const bool installed = std::filesystem::exists(entry.install_dir, ec);
    out << "  " << entry.name << "  " << entry.version << "  " << dependency_source_label(entry.source) << "  ";
    if (!installed) {
        out << "missing";
    } else {
        out << "ok";
    }
    if (!entry.install_dir.empty()) {
        out << "  (" << entry.install_dir << ')';
    }
    out << '\n';
}

int cmd_pack(const std::vector<std::string>& args, CliState& cli, std::ostream& out, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    if (!args.empty()) {
        print_cli_error(err, "pack does not accept positional arguments");
        return kExitUsage;
    }
    package::PackOptions options;
    options.force_overwrite = cli.flag("force");
    package::PackResult result;
    std::string error;
    if (!package::pack_project(*paths, options, result, &error)) {
        print_cli_error(err, error.empty() ? "pack failed" : error);
        return kExitCompile;
    }
    out << result.archive_path.lexically_normal().string() << '\n';
    out << result.name << '@' << result.version << "  " << result.file_count << " file(s)  sha256:" << result.checksum
        << '\n';
    return kExitSuccess;
}

int cmd_list(std::ostream& out, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    std::string error;
    const auto manifest = package::load_manifest_file(paths->manifest.string(), &error);
    if (!manifest) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    std::error_code ec;
    if (!std::filesystem::exists(paths->lockfile, ec)) {
        out << "No pscript.lock — run `pscript install` to resolve dependencies.\n\n";
        out << "Declared in pscript.toml:\n";
        if (manifest->dependencies.empty() && manifest->dev_dependencies.empty()) {
            out << "  (none)\n";
        } else {
            for (const auto& [name, spec] : manifest->dependencies) {
                out << "  " << name << "  (not installed)\n";
            }
            for (const auto& [name, spec] : manifest->dev_dependencies) {
                out << "  " << name << "  dev  (not installed)\n";
            }
        }
        return kExitSuccess;
    }
    const auto lock = package::load_lockfile(paths->lockfile.string(), &error);
    if (!lock) {
        print_cli_error(err, error.empty() ? "cannot read pscript.lock" : error);
        return kExitCompile;
    }
    out << "Installed dependencies:\n";
    if (lock->packages.empty()) {
        out << "  (none)\n";
    } else {
        for (const package::LockedPackage& entry : lock->packages) {
            print_locked_row(out, entry);
        }
    }
    for (const auto& [name, spec] : manifest->dependencies) {
        if (package::find_locked_package(*lock, name) == nullptr) {
            out << "  " << name << "  (declared in manifest, not in lockfile)\n";
        }
    }
    return kExitSuccess;
}

void print_tree_node(std::ostream& out, const package::Lockfile& lock, const std::string& dep_name,
                     const std::string& prefix, bool is_last, std::set<std::string>& stack) {
    const std::string branch = prefix + (is_last ? "`-- " : "|-- ");
    if (stack.count(dep_name) != 0) {
        out << branch << dep_name << " (cycle)\n";
        return;
    }
    const package::LockedPackage* entry = package::find_locked_package(lock, dep_name);
    if (entry == nullptr) {
        out << branch << dep_name << " (unresolved)\n";
        return;
    }
    out << branch << entry->name << '@' << entry->version << '\n';
    stack.insert(dep_name);
    std::string manifest_error;
    const std::filesystem::path dep_manifest = std::filesystem::path(entry->install_dir) / "pscript.toml";
    const auto dep_meta = package::load_manifest_file(dep_manifest.string(), &manifest_error);
    std::vector<std::string> children;
    if (dep_meta) {
        for (const auto& [child_name, spec] : dep_meta->dependencies) {
            children.push_back(child_name);
        }
        std::sort(children.begin(), children.end());
    }
    const std::string child_prefix = prefix + (is_last ? "    " : "|   ");
    for (std::size_t i = 0; i < children.size(); ++i) {
        print_tree_node(out, lock, children[i], child_prefix, i + 1 == children.size(), stack);
    }
    stack.erase(dep_name);
}

int cmd_tree(std::ostream& out, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    std::string error;
    const auto manifest = package::load_manifest_file(paths->manifest.string(), &error);
    if (!manifest) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    out << manifest->name << '@' << manifest->version << '\n';
    std::vector<std::string> roots;
    for (const auto& [name, spec] : manifest->dependencies) {
        roots.push_back(name);
    }
    std::sort(roots.begin(), roots.end());
    if (roots.empty()) {
        out << "  (no dependencies)\n";
        return kExitSuccess;
    }
    std::error_code ec;
    if (!std::filesystem::exists(paths->lockfile, ec)) {
        for (std::size_t i = 0; i < roots.size(); ++i) {
            out << (i + 1 == roots.size() ? "`-- " : "|-- ") << roots[i] << " (lockfile missing)\n";
        }
        return kExitSuccess;
    }
    const auto lock = package::load_lockfile(paths->lockfile.string(), &error);
    if (!lock) {
        print_cli_error(err, error.empty() ? "cannot read pscript.lock" : error);
        return kExitCompile;
    }
    std::set<std::string> stack;
    for (std::size_t i = 0; i < roots.size(); ++i) {
        print_tree_node(out, *lock, roots[i], "", i + 1 == roots.size(), stack);
    }
    return kExitSuccess;
}

void print_metadata_field(std::ostream& out, std::string_view key, const std::string& value) {
    if (field_looks_sensitive(key) || field_looks_sensitive(value)) {
        return;
    }
    if (value.empty()) {
        return;
    }
    out << key << ": " << value << '\n';
}

int cmd_metadata(std::ostream& out, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    std::string error;
    const auto manifest = package::load_manifest_file(paths->manifest.string(), &error);
    if (!manifest) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    out << "Project metadata\n";
    print_metadata_field(out, "name", manifest->name);
    print_metadata_field(out, "version", manifest->version);
    print_metadata_field(out, "description", manifest->description);
    print_metadata_field(out, "license", manifest->license);
    print_metadata_field(out, "authors", manifest->authors);
    out << "root: " << paths->root.lexically_normal().string() << '\n';
    out << "dependencies: " << manifest->dependencies.size() << '\n';
    out << "dev_dependencies: " << manifest->dev_dependencies.size() << '\n';
    std::error_code ec;
    if (!std::filesystem::exists(paths->lockfile, ec)) {
        out << "lockfile: (missing)\n";
        return kExitSuccess;
    }
    const auto lock = package::load_lockfile(paths->lockfile.string(), &error);
    if (!lock) {
        print_cli_error(err, error.empty() ? "cannot read pscript.lock" : error);
        return kExitCompile;
    }
    out << "lockfile: " << paths->lockfile.lexically_normal().string() << '\n';
    out << "locked_packages: " << lock->packages.size() << '\n';
    for (const package::LockedPackage& entry : lock->packages) {
        out << "  - " << entry.name << '@' << entry.version;
        if (!entry.checksum.empty()) {
            out << "  sha256:" << entry.checksum.substr(0, std::min<std::size_t>(12, entry.checksum.size())) << "...";
        }
        out << '\n';
    }
    return kExitSuccess;
}

int cmd_publish(const std::vector<std::string>& args, CliState& cli, std::ostream& out, std::ostream& err) {
    const auto paths = project_in_cwd(err);
    if (!paths) {
        return kExitUsage;
    }
    const bool dry_run = cli.flag("dry-run");
    if (cli.flag("force")) {
        print_cli_error(err, "publish does not accept --force");
        return kExitUsage;
    }
    std::vector<std::string> token_args;
    for (const std::string& arg : args) {
        if (arg == "--dry-run" || arg == "--force") {
            continue;
        }
        token_args.push_back(arg);
    }
    std::string error;
    if (dry_run) {
        package::PackageDryRunSummary summary;
        if (!package::dry_run_package(*paths, summary, &error)) {
            print_cli_error(err, error.empty() ? "publish validation failed" : error);
            return kExitCompile;
        }
        out << "Dry run only — no registry upload was performed.\n";
        out << "Remote availability was not checked.\n\n";
        out << "Package: " << summary.name << '@' << summary.version << '\n';
        out << "Archive: " << summary.archive_path.lexically_normal().string() << '\n';
        out << "Files:   " << summary.file_count << '\n';
        out << "SHA-256: " << summary.checksum << '\n';
        return kExitSuccess;
    }
    const auto manifest = package::load_manifest_file(paths->manifest.string(), &error);
    if (!manifest) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    const auto version = package::SemVer::parse(manifest->version);
    if (!version) {
        print_cli_error(err, "invalid project version");
        return kExitCompile;
    }
    std::string token;
    if (!token_args.empty()) {
        token = token_args[0];
    } else if (const std::optional<package::RegistryCredentials> credentials =
                   package::load_registry_credentials(&error)) {
        if (!package::registry_url_matches_configured(credentials->registry_url, &error)) {
            print_cli_error(err, error);
            return kExitCompile;
        }
        token = credentials->token;
    } else if (const char* env = std::getenv("PSCRIPT_PUBLISH_TOKEN")) {
        token = env;
    }
    const std::filesystem::path source = paths->root;
    if (!package::registry_publish_package(package::default_registry_url(), manifest->name, *version,
                                           source.string(), token, &error)) {
        print_cli_error(err, error);
        return kExitCompile;
    }
    return kExitSuccess;
}

}  // namespace

bool is_package_command(std::string_view name) {
    return name == kCmdAdd || name == kCmdRemove || name == kCmdInstall || name == kCmdUpdate || name == kCmdSync ||
           name == kCmdPackage || name == kCmdPack || name == kCmdPublish || name == kCmdList || name == kCmdTree ||
           name == kCmdMetadata || name == kCmdAudit;
}

int run_package_command(const std::string& command, const std::vector<std::string>& args, CliState& cli,
                        std::ostream& out, std::ostream& err) {
    if (command == kCmdAdd) {
        return cmd_add(args, err);
    }
    if (command == kCmdRemove) {
        return cmd_remove(args, out, err);
    }
    if (command == kCmdInstall) {
        return cmd_install(args, cli, err);
    }
    if (command == kCmdUpdate) {
        return cmd_update(args, out, err);
    }
    if (command == kCmdSync) {
        return cmd_sync(out, err);
    }
    if (command == kCmdPackage) {
        return cmd_package(out, err);
    }
    if (command == kCmdPack) {
        return cmd_pack(args, cli, out, err);
    }
    if (command == kCmdPublish) {
        return cmd_publish(args, cli, out, err);
    }
    if (command == kCmdList) {
        return cmd_list(out, err);
    }
    if (command == kCmdTree) {
        return cmd_tree(out, err);
    }
    if (command == kCmdMetadata) {
        return cmd_metadata(out, err);
    }
    if (command == kCmdAudit) {
        return cmd_audit(out, err);
    }
    print_cli_error(err, "unknown package command");
    return kExitUsage;
}

}  // namespace pscript
