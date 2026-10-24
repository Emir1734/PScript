#pragma once

#include "package/manifest.h"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pscript::package {

struct LockedPackage {
    std::string name;
    std::string version;
    std::string checksum;
    DependencySourceKind source = DependencySourceKind::Registry;
    std::string path;
    std::string git_url;
    std::string git_ref;
    std::string install_dir;
};

struct Lockfile {
    std::vector<LockedPackage> packages;
};

std::optional<Lockfile> load_lockfile(const std::string& path, std::string* error = nullptr);
std::string format_lockfile_toml(const Lockfile& lockfile);
bool write_lockfile(const std::string& path, const Lockfile& lockfile, std::string* error = nullptr);

const LockedPackage* find_locked_package(const Lockfile& lockfile, std::string_view name);

}  // namespace pscript::package
