#include "package/package_archive.h"

#include "stdlib/archive/archive_security.h"
#include "stdlib/archive/archive_tar.h"
#include "stdlib/archive/archive_types.h"

#include <filesystem>
#include <fstream>
#include <vector>

namespace pscript::package {
namespace {

pscript::archive::ArchiveLimits package_archive_limits() {
    pscript::archive::ArchiveLimits limits;
    limits.max_entries = 100'000;
    limits.max_single_file_bytes = 64ULL * 1024 * 1024;
    limits.max_total_extract_bytes = 256ULL * 1024 * 1024;
    return limits;
}

void remove_tree_quiet(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

bool package_tree_looks_valid(const std::filesystem::path& root) {
    std::error_code ec;
    if (std::filesystem::exists(root / "pscript.toml", ec)) {
        return true;
    }
    if (std::filesystem::exists(root / "src", ec) && std::filesystem::is_directory(root / "src", ec)) {
        return true;
    }
    return false;
}

}  // namespace

bool create_directory_tar_gz(const std::filesystem::path& source_dir, const std::filesystem::path& archive_path,
                             std::string* error) {
    if (!std::filesystem::is_directory(source_dir)) {
        if (error) {
            *error = "package source is not a directory: " + source_dir.string();
        }
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(archive_path.parent_path(), ec);
    if (ec) {
        if (error) {
            *error = ec.message();
        }
        return false;
    }
    try {
        const pscript::archive::ArchiveLimits limits = package_archive_limits();
        std::unique_ptr<pscript::archive::ArchiveBackend> backend =
            pscript::archive::make_targz_backend(archive_path.string(), pscript::archive::ArchiveMode::Write, 6,
                                                 limits);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(source_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::filesystem::path rel = std::filesystem::relative(entry.path(), source_dir);
            const std::optional<std::string> entry_name =
                pscript::archive::normalize_archive_entry_path(rel.generic_string(), false);
            if (!entry_name || entry_name->empty()) {
                if (error) {
                    *error = "unsafe archive path: " + rel.generic_string();
                }
                return false;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            backend->write_bytes(*entry_name, data);
        }
        backend->finalize();
        return true;
    } catch (const std::exception& ex) {
        remove_tree_quiet(archive_path);
        if (error) {
            *error = ex.what();
        }
        return false;
    }
}

bool extract_tar_gz_archive(const std::filesystem::path& archive_path, const std::filesystem::path& destination_dir,
                            std::string* error) {
    if (!std::filesystem::is_regular_file(archive_path)) {
        if (error) {
            *error = "package archive missing: " + archive_path.string();
        }
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(destination_dir, ec);
    if (ec) {
        if (error) {
            *error = ec.message();
        }
        return false;
    }
    remove_tree_quiet(destination_dir);
    std::filesystem::create_directories(destination_dir, ec);
    if (ec) {
        if (error) {
            *error = ec.message();
        }
        return false;
    }
    try {
        const pscript::archive::ArchiveLimits limits = package_archive_limits();
        std::unique_ptr<pscript::archive::ArchiveBackend> backend =
            pscript::archive::make_targz_backend(archive_path.string(), pscript::archive::ArchiveMode::Read, 6, limits);
        backend->extract_all(destination_dir.string());
        backend->close();
    } catch (const std::exception& ex) {
        remove_tree_quiet(destination_dir);
        if (error) {
            *error = ex.what();
        }
        return false;
    }
    if (!package_tree_looks_valid(destination_dir)) {
        remove_tree_quiet(destination_dir);
        if (error) {
            *error = "extracted package does not contain pscript.toml or src/";
        }
        return false;
    }
    return true;
}

}  // namespace pscript::package
