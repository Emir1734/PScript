#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace pscript::package {

struct ProjectPaths {
    std::filesystem::path root;
    std::filesystem::path manifest;
    std::filesystem::path lockfile;
    std::filesystem::path deps_dir;
    std::filesystem::path src_dir;
    std::filesystem::path tests_dir;
};

std::optional<ProjectPaths> find_project_root(const std::filesystem::path& start);
ProjectPaths project_paths_from_root(const std::filesystem::path& root);
std::string default_registry_url();

bool scaffold_project(const std::filesystem::path& directory, std::string_view name, std::string* error = nullptr);

bool scaffold_library_project(const std::filesystem::path& directory, std::string_view name, std::string* error = nullptr);

}  // namespace pscript::package
