#pragma once

#include "package/semver.h"
#include "package/toml.h"

#include <map>
#include <optional>
#include <string>

namespace pscript::package {

enum class DependencySourceKind {
    Registry,
    Path,
    Git,
};

struct DependencySpec {
    std::string name;
    VersionConstraint constraint;
    DependencySourceKind source = DependencySourceKind::Registry;
    std::string path;
    std::string git_url;
    std::string git_ref;
    bool dev = false;
};

struct ProjectManifest {
    std::string name;
    std::string version;
    std::string description;
    std::string license;
    std::string authors;
    std::map<std::string, DependencySpec> dependencies;
    std::map<std::string, DependencySpec> dev_dependencies;
};

std::optional<ProjectManifest> load_manifest_file(const std::string& path, std::string* error = nullptr);
std::string format_manifest_toml(const ProjectManifest& manifest);
bool write_manifest_file(const std::string& path, const ProjectManifest& manifest, std::string* error = nullptr);

std::optional<DependencySpec> parse_dependency_entry(std::string_view name, const TomlValue& value, bool dev);

}  // namespace pscript::package
