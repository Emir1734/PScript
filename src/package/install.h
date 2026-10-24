#pragma once

#include "package/lockfile.h"
#include "package/manifest.h"
#include "package/project.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pscript::package {

struct InstallOptions {
    bool include_dev_dependencies = false;
    bool use_lockfile_only = false;
    bool verify_checksums = true;
};

bool install_project(const ProjectPaths& paths, const InstallOptions& options, std::string* error = nullptr);
bool sync_project(const ProjectPaths& paths, std::string* error = nullptr);
bool remove_project_dependency(const ProjectPaths& paths, std::string_view package_name, std::string* error = nullptr);

bool is_install_dir_within_deps(const ProjectPaths& paths, const std::filesystem::path& install_dir);
bool prune_orphan_installed_packages(const ProjectPaths& paths, const Lockfile& previous_lock,
                                     const Lockfile& new_lock, std::string* error = nullptr);

enum class DependencyAuditLevel {
    Info,
    Warn,
    Error,
};

struct DependencyAuditFinding {
    DependencyAuditLevel level = DependencyAuditLevel::Info;
    std::string message;
};

struct DependencyAuditReport {
    std::vector<DependencyAuditFinding> findings;

    bool has_errors() const;
    bool has_warnings() const;
};

DependencyAuditReport audit_project_dependencies(const ProjectPaths& paths);
Lockfile resolve_dependencies(const ProjectPaths& paths, const ProjectManifest& manifest,
                              const InstallOptions& options, std::string* error = nullptr);

std::optional<std::string> resolve_package_module(const std::string& specifier,
                                                    const std::string& importer_file);

std::string package_primary_module_stem(const LockedPackage& entry);
std::filesystem::path package_module_file_for_import(const std::filesystem::path& package_src,
                                                     const std::string& module_stem, std::string_view subpath);

}  // namespace pscript::package
