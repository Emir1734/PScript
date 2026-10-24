#pragma once

#include "package/manifest.h"
#include "package/project.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace pscript::package {

struct PackOptions {
    bool force_overwrite = false;
};

struct PackResult {
    std::filesystem::path archive_path;
    std::string checksum;
    std::size_t file_count = 0;
    std::string name;
    std::string version;
};

struct PackageDryRunSummary {
    std::string name;
    std::string version;
    std::filesystem::path archive_path;
    std::size_t file_count = 0;
    std::string checksum;
};

bool load_and_validate_project_package(const ProjectPaths& paths, ProjectManifest& manifest, std::string* error);

bool stage_release_tree(const ProjectPaths& paths, const std::filesystem::path& dest_dir, std::size_t* file_count,
                        std::string* error);

bool pack_project(const ProjectPaths& paths, const PackOptions& options, PackResult& result, std::string* error);

bool dry_run_package(const ProjectPaths& paths, PackageDryRunSummary& summary, std::string* error);

std::string library_module_filename(std::string_view package_name);

}  // namespace pscript::package
