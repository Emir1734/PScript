#include "package/packaging.h"

#include "package/checksum.h"
#include "package/package_archive.h"
#include "package/package_identity.h"
#include "package/semver.h"

#include <fstream>

namespace pscript::package {
namespace {

bool path_has_excluded_part(const std::filesystem::path& relative) {
    for (const std::filesystem::path& part : relative) {
        const std::string name = part.string();
        if (name == ".pscript" || name == "dist" || name == ".git" || name == "node_modules") {
            return true;
        }
        if (!name.empty() && name[0] == '.' && name != "." && name != "..") {
            if (name == ".env" || name == ".env.local") {
                return true;
            }
        }
    }
    return false;
}

bool copy_tree_filtered(const std::filesystem::path& source, const std::filesystem::path& dest,
                        const std::filesystem::path& source_root, std::size_t* file_count, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        return true;
    }
    if (std::filesystem::is_regular_file(source, ec)) {
        const std::filesystem::path rel = std::filesystem::relative(source, source_root, ec);
        if (ec || path_has_excluded_part(rel)) {
            return !ec;
        }
        std::filesystem::create_directories(dest.parent_path(), ec);
        std::filesystem::copy_file(source, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error) {
                *error = ec.message();
            }
            return false;
        }
        if (file_count) {
            ++*file_count;
        }
        return true;
    }
    if (!std::filesystem::is_directory(source, ec)) {
        return true;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(source, ec)) {
        if (ec) {
            if (error) {
                *error = ec.message();
            }
            return false;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const std::filesystem::path rel = std::filesystem::relative(entry.path(), source_root, ec);
        if (ec || rel.empty() || path_has_excluded_part(rel)) {
            continue;
        }
        const std::filesystem::path out_file = dest / rel;
        std::filesystem::create_directories(out_file.parent_path(), ec);
        std::filesystem::copy_file(entry.path(), out_file, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            if (error) {
                *error = ec.message();
            }
            return false;
        }
        if (file_count) {
            ++*file_count;
        }
    }
    return true;
}

bool src_has_phs_files(const std::filesystem::path& src_dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(src_dir, ec)) {
        return false;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(src_dir, ec)) {
        if (ec || !entry.is_regular_file(ec)) {
            continue;
        }
        if (entry.path().extension() == ".phs") {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string library_module_filename(std::string_view package_name) {
    return install_dir_basename(package_name) + ".phs";
}

bool load_and_validate_project_package(const ProjectPaths& paths, ProjectManifest& manifest, std::string* error) {
    std::string load_error;
    const auto loaded = load_manifest_file(paths.manifest.string(), &load_error);
    if (!loaded) {
        if (error) {
            *error = load_error.empty() ? "invalid pscript.toml" : load_error;
        }
        return false;
    }
    manifest = *loaded;
    if (!validate_package_name(manifest.name, error)) {
        return false;
    }
    if (!SemVer::parse(manifest.version)) {
        if (error) {
            *error = "invalid project version: " + manifest.version;
        }
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(paths.manifest, ec)) {
        if (error) {
            *error = "missing pscript.toml";
        }
        return false;
    }
    if (!std::filesystem::exists(paths.src_dir, ec) || !std::filesystem::is_directory(paths.src_dir, ec)) {
        if (error) {
            *error = "missing src/ directory";
        }
        return false;
    }
    if (!src_has_phs_files(paths.src_dir)) {
        if (error) {
            *error = "src/ must contain at least one .phs file";
        }
        return false;
    }
    return true;
}

bool stage_release_tree(const ProjectPaths& paths, const std::filesystem::path& dest_dir, std::size_t* file_count,
                        std::string* error) {
    std::error_code ec;
    std::filesystem::remove_all(dest_dir, ec);
    std::filesystem::create_directories(dest_dir, ec);
    if (ec) {
        if (error) {
            *error = ec.message();
        }
        return false;
    }
    if (file_count) {
        *file_count = 0;
    }
    const std::filesystem::path manifest_dest = dest_dir / "pscript.toml";
    std::filesystem::copy_file(paths.manifest, manifest_dest, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error) {
            *error = "cannot copy pscript.toml: " + ec.message();
        }
        return false;
    }
    if (file_count) {
        ++*file_count;
    }
    const std::filesystem::path readme = paths.root / "README.md";
    if (std::filesystem::exists(readme, ec)) {
        std::filesystem::copy_file(readme, dest_dir / "README.md", std::filesystem::copy_options::overwrite_existing,
                                  ec);
        if (ec) {
            if (error) {
                *error = "cannot copy README.md: " + ec.message();
            }
            return false;
        }
        if (file_count) {
            ++*file_count;
        }
    }
    return copy_tree_filtered(paths.src_dir, dest_dir / "src", paths.src_dir, file_count, error);
}

bool pack_project(const ProjectPaths& paths, const PackOptions& options, PackResult& result, std::string* error) {
    ProjectManifest manifest;
    if (!load_and_validate_project_package(paths, manifest, error)) {
        return false;
    }
    const std::filesystem::path dist_dir = paths.root / "dist" / (manifest.name + "-" + manifest.version);
    const std::filesystem::path archive_path = dist_dir / "package.tar.gz";
    std::error_code ec;
    if (std::filesystem::exists(archive_path, ec) && !options.force_overwrite) {
        if (error) {
            *error = "archive already exists: " + archive_path.string() + " (use --force to overwrite)";
        }
        return false;
    }
    std::filesystem::create_directories(dist_dir, ec);
    const std::filesystem::path staging = dist_dir / ".pack-staging";
    std::size_t file_count = 0;
    if (!stage_release_tree(paths, staging, &file_count, error)) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    if (!create_directory_tar_gz(staging, archive_path, error)) {
        std::filesystem::remove_all(staging, ec);
        return false;
    }
    const std::string checksum = sha256_directory(staging.string(), error);
    std::filesystem::remove_all(staging, ec);
    if (checksum.empty()) {
        std::filesystem::remove(archive_path, ec);
        return false;
    }
    result.archive_path = archive_path;
    result.checksum = checksum;
    result.file_count = file_count;
    result.name = manifest.name;
    result.version = manifest.version;
    return true;
}

bool dry_run_package(const ProjectPaths& paths, PackageDryRunSummary& summary, std::string* error) {
    ProjectManifest manifest;
    if (!load_and_validate_project_package(paths, manifest, error)) {
        return false;
    }
    const std::filesystem::path dist_dir = paths.root / "dist" / (manifest.name + "-" + manifest.version);
    summary.name = manifest.name;
    summary.version = manifest.version;
    summary.archive_path = dist_dir / "package.tar.gz";
    std::string staging_name = "pscript-pack-dry-" + manifest.name + "-" + manifest.version;
    for (char& ch : staging_name) {
        if (ch == '@' || ch == '/' || ch == '\\' || ch == ':') {
            ch = '_';
        }
    }
    const std::filesystem::path staging = std::filesystem::temp_directory_path() / staging_name;
    std::size_t file_count = 0;
    if (!stage_release_tree(paths, staging, &file_count, error)) {
        std::error_code cleanup_ec;
        std::filesystem::remove_all(staging, cleanup_ec);
        return false;
    }
    summary.file_count = file_count;
    summary.checksum = sha256_directory(staging.string(), error);
    std::error_code cleanup_ec;
    std::filesystem::remove_all(staging, cleanup_ec);
    return !summary.checksum.empty();
}

}  // namespace pscript::package
