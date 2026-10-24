#include "package/install.h"

#include "package/atomic_file.h"
#include "package/checksum.h"
#include "package/git_clone.h"
#include "package/manifest.h"
#include "package/package_identity.h"
#include "package/registry.h"

#include <filesystem>
#include <fstream>
#include <set>

namespace pscript::package {
namespace {

std::filesystem::path install_dir_for(const ProjectPaths& paths, const std::string& name,
                                      const std::string& version) {
    return paths.deps_dir / (install_dir_basename(name) + "-" + version);
}

bool remove_tree(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    return !ec;
}

bool copy_tree(const std::filesystem::path& from, const std::filesystem::path& to, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(to, ec);
    std::filesystem::copy(from, to,
                            std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::overwrite_existing,
                            ec);
    if (ec) {
        if (error) {
            *error = ec.message();
        }
        return false;
    }
    return true;
}

bool materialize_registry_package(const DependencySpec& spec, const SemVer& version, const ProjectPaths& paths,
                                  LockedPackage* out, std::string* error) {
    const std::filesystem::path dest = install_dir_for(paths, spec.name, version.to_string());
    const std::filesystem::path staging = dest.parent_path() / (dest.filename().string() + ".staging");
    remove_tree(staging);
    if (!registry_download_package(default_registry_url(), spec.name, version, staging.string(), error)) {
        remove_tree(staging);
        return false;
    }
    remove_tree(dest);
    std::error_code ec;
    std::filesystem::rename(staging, dest, ec);
    if (ec) {
        if (!copy_tree(staging, dest, error)) {
            remove_tree(staging);
            return false;
        }
        remove_tree(staging);
    }
    out->name = spec.name;
    out->version = version.to_string();
    out->source = DependencySourceKind::Registry;
    out->install_dir = std::filesystem::absolute(dest).string();
    out->checksum = sha256_directory(out->install_dir, error);
    return !out->checksum.empty();
}

bool materialize_path_package(const DependencySpec& spec, const ProjectPaths& paths, LockedPackage* out,
                              std::string* error) {
    std::filesystem::path source = std::filesystem::path(spec.path);
    if (source.is_relative()) {
        source = paths.root / source;
    }
    source = std::filesystem::weakly_canonical(source);
    if (!std::filesystem::is_directory(source)) {
        if (error) {
            *error = "path dependency does not exist: " + source.string();
        }
        return false;
    }
    std::string version = "0.0.0";
    const std::filesystem::path manifest = source / "pscript.toml";
    if (std::filesystem::exists(manifest)) {
        if (const auto manifest_data = load_manifest_file(manifest.string(), error)) {
            version = manifest_data->version;
        }
    }
    const std::filesystem::path dest = install_dir_for(paths, spec.name, version);
    remove_tree(dest);
    if (!copy_tree(source, dest, error)) {
        return false;
    }
    out->name = spec.name;
    out->version = version;
    out->source = DependencySourceKind::Path;
    out->path = spec.path;
    out->install_dir = std::filesystem::absolute(dest).string();
    out->checksum = sha256_directory(out->install_dir, error);
    return !out->checksum.empty();
}

bool materialize_git_package(const DependencySpec& spec, const ProjectPaths& paths, LockedPackage* out,
                             std::string* error) {
    const std::filesystem::path dest = install_dir_for(paths, spec.name, "git");
    if (!run_git_clone(spec.git_url, spec.git_ref, dest, error)) {
        return false;
    }
    std::string version = "0.0.0";
    const std::filesystem::path manifest = dest / "pscript.toml";
    if (std::filesystem::exists(manifest)) {
        if (const auto manifest_data = load_manifest_file(manifest.string(), error)) {
            version = manifest_data->version;
        }
    }
    out->name = spec.name;
    out->version = version;
    out->source = DependencySourceKind::Git;
    out->git_url = spec.git_url;
    out->git_ref = spec.git_ref;
    out->install_dir = std::filesystem::absolute(dest).string();
    out->checksum = sha256_directory(out->install_dir, error);
    return !out->checksum.empty();
}

bool install_locked_entry(const LockedPackage& entry, const ProjectPaths& paths, bool verify_checksums,
                          std::string* error) {
    if (!std::filesystem::exists(entry.install_dir)) {
        if (error) {
            *error = "missing installed package: " + entry.name;
        }
        return false;
    }
    if (verify_checksums && !entry.checksum.empty()) {
        return verify_directory_checksum(entry.install_dir, entry.checksum, error);
    }
    return true;
}

}  // namespace

std::string package_primary_module_stem(const LockedPackage& entry) {
    const std::filesystem::path manifest_path = std::filesystem::path(entry.install_dir) / "pscript.toml";
    std::string manifest_error;
    if (const auto manifest = load_manifest_file(manifest_path.string(), &manifest_error)) {
        if (!manifest->name.empty()) {
            return install_dir_basename(manifest->name);
        }
    }
    return install_dir_basename(entry.name);
}

std::filesystem::path package_module_file_for_import(const std::filesystem::path& package_src,
                                                     const std::string& module_stem, std::string_view subpath) {
    if (subpath.empty()) {
        const std::filesystem::path named = package_src / (module_stem + ".phs");
        if (std::filesystem::exists(named)) {
            return named;
        }
        const std::filesystem::path main_file = package_src / "main.phs";
        if (std::filesystem::exists(main_file)) {
            return main_file;
        }
        return named;
    }
    std::filesystem::path relative;
    std::size_t start = 0;
    while (start < subpath.size()) {
        const std::size_t dot = subpath.find('.', start);
        const std::string part =
            dot == std::string_view::npos ? std::string(subpath.substr(start)) : std::string(subpath.substr(start, dot - start));
        relative /= part;
        if (dot == std::string_view::npos) {
            break;
        }
        start = dot + 1;
    }
    relative.replace_extension(".phs");
    return package_src / relative;
}

Lockfile resolve_dependencies(const ProjectPaths& paths, const ProjectManifest& manifest,
                              const InstallOptions& options, std::string* error) {
    Lockfile lockfile;
    const std::string registry = default_registry_url();

    auto resolve_one = [&](const DependencySpec& copy) -> bool {
        LockedPackage locked;
        switch (copy.source) {
            case DependencySourceKind::Path:
                if (!materialize_path_package(copy, paths, &locked, error)) {
                    return false;
                }
                break;
            case DependencySourceKind::Git:
                if (!materialize_git_package(copy, paths, &locked, error)) {
                    return false;
                }
                break;
            case DependencySourceKind::Registry: {
                std::string package_name = copy.name;
                if (!parse_scoped_package_id(package_name)) {
                    const std::vector<std::string> matches =
                        registry_search_packages(registry, package_name, 5, error);
                    if (matches.size() > 1) {
                        if (error) {
                            *error = "ambiguous package name '" + package_name +
                                     "'; use a canonical @username/package id";
                        }
                        return false;
                    }
                    if (matches.size() == 1) {
                        package_name = matches.front();
                    }
                }
                const std::vector<SemVer> versions = registry_list_versions(registry, package_name, error);
                SemVer selected;
                if (!select_highest_matching(versions, copy.constraint, &selected)) {
                    if (error) {
                        *error = "cannot resolve registry version for " + package_name;
                    }
                    return false;
                }
                DependencySpec resolved = copy;
                resolved.name = package_name;
                if (!materialize_registry_package(resolved, selected, paths, &locked, error)) {
                    return false;
                }
                break;
            }
        }
        lockfile.packages.push_back(std::move(locked));
        return true;
    };

    for (const auto& [name, spec] : manifest.dependencies) {
        DependencySpec copy = spec;
        copy.name = name;
        if (!resolve_one(copy)) {
            return {};
        }
    }
    if (options.include_dev_dependencies) {
        for (const auto& [name, spec] : manifest.dev_dependencies) {
            DependencySpec copy = spec;
            copy.name = name;
            copy.dev = true;
            if (!resolve_one(copy)) {
                return {};
            }
        }
    }
    return lockfile;
}

bool install_project(const ProjectPaths& paths, const InstallOptions& options, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(paths.deps_dir, ec);
    if (options.use_lockfile_only) {
        const auto lockfile = load_lockfile(paths.lockfile.string(), error);
        if (!lockfile) {
            return false;
        }
        for (const LockedPackage& entry : lockfile->packages) {
            if (!install_locked_entry(entry, paths, options.verify_checksums, error)) {
                return false;
            }
        }
        return true;
    }
    const auto manifest = load_manifest_file(paths.manifest.string(), error);
    if (!manifest) {
        return false;
    }
    const Lockfile resolved = resolve_dependencies(paths, *manifest, options, error);
    if (!manifest->dependencies.empty() && resolved.packages.empty()) {
        return false;
    }
    if (!write_lockfile(paths.lockfile.string(), resolved, error)) {
        return false;
    }
    return true;
}

bool sync_project(const ProjectPaths& paths, std::string* error) {
    InstallOptions options;
    options.use_lockfile_only = true;
    options.verify_checksums = true;
    return install_project(paths, options, error);
}

bool is_install_dir_within_deps(const ProjectPaths& paths, const std::filesystem::path& install_dir) {
    if (install_dir.empty()) {
        return false;
    }
    std::error_code ec;
    const std::filesystem::path deps_root = std::filesystem::weakly_canonical(paths.deps_dir, ec);
    const std::filesystem::path target = std::filesystem::weakly_canonical(install_dir, ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path relative = std::filesystem::relative(target, deps_root, ec);
    if (ec || relative.empty()) {
        return false;
    }
    for (const std::filesystem::path& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

struct ProjectMetadataBackup {
    bool manifest_existed = false;
    bool lockfile_existed = false;
    std::string manifest_content;
    std::string lockfile_content;
};

ProjectMetadataBackup capture_project_metadata_backup(const ProjectPaths& paths) {
    ProjectMetadataBackup backup;
    std::error_code ec;
    backup.manifest_existed = std::filesystem::exists(paths.manifest, ec);
    if (backup.manifest_existed) {
        if (const auto content = read_text_file(paths.manifest)) {
            backup.manifest_content = *content;
        }
    }
    backup.lockfile_existed = std::filesystem::exists(paths.lockfile, ec);
    if (backup.lockfile_existed) {
        if (const auto content = read_text_file(paths.lockfile)) {
            backup.lockfile_content = *content;
        }
    }
    return backup;
}

bool restore_project_metadata_backup(const ProjectPaths& paths, const ProjectMetadataBackup& backup,
                                     std::string* error) {
    std::error_code ec;
    if (backup.manifest_existed) {
        if (!write_file_atomically(paths.manifest, backup.manifest_content, error)) {
            return false;
        }
    } else {
        std::filesystem::remove(paths.manifest, ec);
    }
    if (backup.lockfile_existed) {
        if (!write_file_atomically(paths.lockfile, backup.lockfile_content, error)) {
            return false;
        }
    } else {
        std::filesystem::remove(paths.lockfile, ec);
    }
    return true;
}

bool prune_orphan_installed_packages(const ProjectPaths& paths, const Lockfile& previous_lock,
                                     const Lockfile& new_lock, std::string* error) {
    for (const LockedPackage& entry : previous_lock.packages) {
        if (find_locked_package(new_lock, entry.name) != nullptr) {
            continue;
        }
        if (entry.install_dir.empty()) {
            continue;
        }
        const std::filesystem::path install_path(entry.install_dir);
        if (!is_install_dir_within_deps(paths, install_path)) {
            if (error) {
                *error = "refusing to remove install path outside .pscript/deps: " + entry.install_dir;
            }
            return false;
        }
        if (!remove_tree(install_path)) {
            if (error) {
                *error = "cannot remove installed package directory: " + entry.install_dir;
            }
            return false;
        }
    }
    return true;
}

bool remove_project_dependency(const ProjectPaths& paths, std::string_view package_name, std::string* error) {
    auto manifest = load_manifest_file(paths.manifest.string(), error);
    if (!manifest) {
        return false;
    }
    const std::string name(package_name);
    const bool in_manifest =
        manifest->dependencies.count(name) != 0 || manifest->dev_dependencies.count(name) != 0;
    if (!in_manifest) {
        if (error) {
            *error = "dependency not found in pscript.toml: " + name;
        }
        return false;
    }

    const ProjectMetadataBackup backup = capture_project_metadata_backup(paths);
    if (backup.manifest_existed && backup.manifest_content.empty()) {
        if (error) {
            *error = "cannot read current pscript.toml";
        }
        return false;
    }
    if (backup.lockfile_existed && backup.lockfile_content.empty()) {
        if (error) {
            *error = "cannot read current pscript.lock";
        }
        return false;
    }

    std::optional<Lockfile> previous_lock;
    std::error_code ec;
    if (backup.lockfile_existed) {
        previous_lock = load_lockfile(paths.lockfile.string(), error);
        if (!previous_lock) {
            return false;
        }
    }

    manifest->dependencies.erase(name);
    manifest->dev_dependencies.erase(name);

    InstallOptions options;
    options.include_dev_dependencies = true;
    const Lockfile new_lock = resolve_dependencies(paths, *manifest, options, error);
    if ((!manifest->dependencies.empty() || !manifest->dev_dependencies.empty()) && new_lock.packages.empty()) {
        if (error && error->empty()) {
            *error = "failed to resolve dependencies after removal";
        }
        return false;
    }

    const std::string manifest_text = format_manifest_toml(*manifest);
    if (!write_file_atomically(paths.manifest, manifest_text, error)) {
        if (error && error->empty()) {
            *error = "cannot update pscript.toml";
        }
        return false;
    }
    const std::string lockfile_text = format_lockfile_toml(new_lock);
    if (!write_file_atomically(paths.lockfile, lockfile_text, error)) {
        std::string restore_error;
        const bool restored = restore_project_metadata_backup(paths, backup, &restore_error);
        if (error) {
            if (restored) {
                *error = "lockfile update failed; restored previous pscript.toml and pscript.lock";
            } else {
                *error = "lockfile update failed and metadata restore also failed: " + restore_error;
            }
        }
        return false;
    }

    if (previous_lock.has_value()) {
        if (!prune_orphan_installed_packages(paths, *previous_lock, new_lock, error)) {
            std::string restore_error;
            const bool restored = restore_project_metadata_backup(paths, backup, &restore_error);
            if (error) {
                if (restored) {
                    *error = "removed package files could not be pruned; restored previous project metadata";
                } else {
                    *error = "prune failed and metadata restore also failed: " + restore_error;
                }
            }
            return false;
        }
    }
    return true;
}

bool DependencyAuditReport::has_errors() const {
    for (const DependencyAuditFinding& finding : findings) {
        if (finding.level == DependencyAuditLevel::Error) {
            return true;
        }
    }
    return false;
}

bool DependencyAuditReport::has_warnings() const {
    for (const DependencyAuditFinding& finding : findings) {
        if (finding.level == DependencyAuditLevel::Warn) {
            return true;
        }
    }
    return false;
}

DependencyAuditReport audit_project_dependencies(const ProjectPaths& paths) {
    DependencyAuditReport report;
    std::string error;
    const auto manifest = load_manifest_file(paths.manifest.string(), &error);
    if (!manifest) {
        report.findings.push_back({DependencyAuditLevel::Error, error.empty() ? "invalid pscript.toml" : error});
        return report;
    }

    std::error_code ec;
    if (!std::filesystem::exists(paths.lockfile, ec)) {
        if (!manifest->dependencies.empty() || !manifest->dev_dependencies.empty()) {
            report.findings.push_back(
                {DependencyAuditLevel::Error, "pscript.lock is missing — run `pscript install`"});
        } else {
            report.findings.push_back({DependencyAuditLevel::Info, "no dependencies declared"});
        }
        return report;
    }

    const auto lockfile = load_lockfile(paths.lockfile.string(), &error);
    if (!lockfile) {
        report.findings.push_back(
            {DependencyAuditLevel::Error, error.empty() ? "cannot read pscript.lock" : error});
        return report;
    }

    for (const auto& [name, spec] : manifest->dependencies) {
        if (find_locked_package(*lockfile, name) == nullptr) {
            report.findings.push_back(
                {DependencyAuditLevel::Error, "manifest dependency not in lockfile: " + name});
        }
    }
    for (const auto& [name, spec] : manifest->dev_dependencies) {
        if (find_locked_package(*lockfile, name) == nullptr) {
            report.findings.push_back(
                {DependencyAuditLevel::Warn, "dev dependency not in lockfile: " + name});
        }
    }

    std::set<std::string> manifest_names;
    for (const auto& [name, spec] : manifest->dependencies) {
        manifest_names.insert(name);
    }
    for (const auto& [name, spec] : manifest->dev_dependencies) {
        manifest_names.insert(name);
    }

    for (const LockedPackage& entry : lockfile->packages) {
        if (manifest_names.count(entry.name) == 0) {
            report.findings.push_back(
                {DependencyAuditLevel::Warn, "lockfile entry not declared in manifest: " + entry.name});
        }
        if (!SemVer::parse(entry.version)) {
            report.findings.push_back(
                {DependencyAuditLevel::Error, "invalid locked version for " + entry.name + ": " + entry.version});
        }
        if (entry.install_dir.empty()) {
            report.findings.push_back(
                {DependencyAuditLevel::Error, "missing install_dir for locked package: " + entry.name});
            continue;
        }
        const std::filesystem::path install_path(entry.install_dir);
        if (!is_install_dir_within_deps(paths, install_path)) {
            report.findings.push_back({DependencyAuditLevel::Error,
                                       "unsafe install path for " + entry.name + ": " + entry.install_dir});
            continue;
        }
        if (!std::filesystem::exists(install_path, ec)) {
            report.findings.push_back(
                {DependencyAuditLevel::Error, "installed package missing on disk: " + entry.name});
            continue;
        }
        if (!entry.checksum.empty()) {
            std::string checksum_error;
            if (!verify_directory_checksum(entry.install_dir, entry.checksum, &checksum_error)) {
                report.findings.push_back({DependencyAuditLevel::Error,
                                           entry.name + ": " +
                                               (checksum_error.empty() ? "checksum mismatch" : checksum_error)});
            }
        }
        const std::filesystem::path dep_manifest = install_path / "pscript.toml";
        if (!std::filesystem::exists(dep_manifest, ec)) {
            report.findings.push_back(
                {DependencyAuditLevel::Warn, entry.name + ": installed tree has no pscript.toml"});
        }
    }

    if (report.findings.empty()) {
        report.findings.push_back({DependencyAuditLevel::Info, "no dependency integrity issues found"});
    }
    report.findings.push_back(
        {DependencyAuditLevel::Info,
         "security advisory scanning is not available in this release (local integrity checks only)"});
    return report;
}

std::optional<std::string> resolve_package_module(const std::string& specifier,
                                                    const std::string& importer_file) {
    const auto paths = find_project_root(std::filesystem::path(importer_file));
    if (!paths) {
        return std::nullopt;
    }
    if (!std::filesystem::exists(paths->lockfile)) {
        return std::nullopt;
    }
    const auto lockfile = load_lockfile(paths->lockfile.string());
    if (!lockfile) {
        return std::nullopt;
    }
    std::string package_name = specifier;
    std::string_view subpath;
    const std::size_t dot = specifier.find('.');
    if (dot != std::string::npos) {
        package_name = specifier.substr(0, dot);
        subpath = std::string_view(specifier).substr(dot + 1);
    }
    const LockedPackage* entry = find_locked_package(*lockfile, package_name);
    if (entry == nullptr) {
        return std::nullopt;
    }
    const std::filesystem::path package_src = std::filesystem::path(entry->install_dir) / "src";
    const std::string module_stem = package_primary_module_stem(*entry);
    const std::filesystem::path module_path = package_module_file_for_import(package_src, module_stem, subpath);
    if (!std::filesystem::exists(module_path)) {
        return std::nullopt;
    }
    return module_path.lexically_normal().string();
}

}  // namespace pscript::package
