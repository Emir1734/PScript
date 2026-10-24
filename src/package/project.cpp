#include "package/project.h"

#include "package/manifest.h"
#include "package/package_identity.h"

#include <cstdlib>
#include <fstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pscript::package {

std::optional<ProjectPaths> find_project_root(const std::filesystem::path& start) {
    std::filesystem::path cursor = std::filesystem::absolute(start);
    if (std::filesystem::is_regular_file(cursor)) {
        cursor = cursor.parent_path();
    }
    std::error_code ec;
    while (!cursor.empty()) {
        const std::filesystem::path manifest = cursor / "pscript.toml";
        if (std::filesystem::exists(manifest, ec)) {
            return project_paths_from_root(cursor);
        }
        const std::filesystem::path parent = cursor.parent_path();
        if (parent == cursor) {
            break;
        }
        cursor = parent;
    }
    return std::nullopt;
}

ProjectPaths project_paths_from_root(const std::filesystem::path& root) {
    ProjectPaths paths;
    paths.root = root;
    paths.manifest = root / "pscript.toml";
    paths.lockfile = root / "pscript.lock";
    paths.deps_dir = root / ".pscript" / "deps";
    paths.src_dir = root / "src";
    paths.tests_dir = root / "tests";
    return paths;
}

std::string default_registry_url() {
    std::string env_value;
#ifdef _WIN32
    char buffer[32768];
    const DWORD length = GetEnvironmentVariableA("PSCRIPT_REGISTRY", buffer, static_cast<DWORD>(sizeof(buffer)));
    if (length > 0 && length < sizeof(buffer)) {
        env_value.assign(buffer, length);
    }
#endif
    if (env_value.empty()) {
        if (const char* env = std::getenv("PSCRIPT_REGISTRY")) {
            env_value = env;
        }
    }
    if (!env_value.empty()) {
        std::filesystem::path path(env_value);
        if (path.is_relative()) {
            path = std::filesystem::absolute(path);
        }
        return path.lexically_normal().string();
    }
    return "https://packages.pscript.dev/v1";
}

bool scaffold_project(const std::filesystem::path& directory, std::string_view name, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(directory / "src", ec);
    std::filesystem::create_directories(directory / "tests", ec);
    if (ec) {
        if (error) {
            *error = ec.message();
        }
        return false;
    }
    ProjectManifest manifest;
    manifest.name = std::string(name);
    manifest.version = "1.0.0";
    manifest.description = "A PScript project";
    const ProjectPaths paths = project_paths_from_root(directory);
    if (!write_manifest_file(paths.manifest.string(), manifest, error)) {
        return false;
    }
    const std::filesystem::path main_file = paths.src_dir / "main.phs";
    if (!std::filesystem::exists(main_file, ec)) {
        std::ofstream out(main_file, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "cannot write main.phs";
            }
            return false;
        }
        out << "print(\"Hello from PScript\")\n";
    }
    return true;
}

bool scaffold_library_project(const std::filesystem::path& directory, std::string_view name, std::string* error) {
    std::error_code ec;
    if (std::filesystem::exists(directory / "pscript.toml", ec)) {
        if (error) {
            *error = "pscript.toml already exists in " + directory.lexically_normal().string();
        }
        return false;
    }
    if (std::filesystem::exists(directory, ec) && !std::filesystem::is_directory(directory, ec)) {
        if (error) {
            *error = "destination is not a directory: " + directory.string();
        }
        return false;
    }
    if (!validate_package_name(name, error)) {
        return false;
    }
    std::filesystem::create_directories(directory / "src", ec);
    std::filesystem::create_directories(directory / "tests", ec);
    if (ec) {
        if (error) {
            *error = ec.message();
        }
        return false;
    }
    ProjectManifest manifest;
    manifest.name = std::string(name);
    manifest.version = "0.1.0";
    manifest.description = "A PScript library";
    manifest.license = "MIT";
    const ProjectPaths paths = project_paths_from_root(directory);
    if (!write_manifest_file(paths.manifest.string(), manifest, error)) {
        return false;
    }
    const std::filesystem::path module_file = paths.src_dir / (install_dir_basename(name) + ".phs");
    if (std::filesystem::exists(module_file, ec)) {
        if (error) {
            *error = "library module already exists: " + module_file.string();
        }
        return false;
    }
    std::ofstream out(module_file, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "cannot write " + module_file.string();
        }
        return false;
    }
    out << "# Library module for " << manifest.name << "\n"
           "pub func version():\n"
           "    return \"" << manifest.version << "\"\n";
    return true;
}

}  // namespace pscript::package
