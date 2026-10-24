#include "package/manifest.h"

#include "package/atomic_file.h"
#include "package/package_identity.h"

#include <fstream>
#include <sstream>

namespace pscript::package {
namespace {

std::optional<std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool write_text_file(const std::string& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << content;
    return static_cast<bool>(out);
}

void load_dependency_table(const TomlValue& table, bool dev, std::map<std::string, DependencySpec>& out) {
    if (table.kind != TomlValue::Kind::Table) {
        return;
    }
    for (const auto& [name, value] : table.table) {
        const auto spec = parse_dependency_entry(name, value, dev);
        if (spec) {
            out[name] = *spec;
        }
    }
}

}  // namespace

std::optional<DependencySpec> parse_dependency_entry(std::string_view name, const TomlValue& value, bool dev) {
    if (!validate_package_name(name, nullptr)) {
        return std::nullopt;
    }
    DependencySpec spec;
    spec.name = std::string(name);
    spec.dev = dev;
    if (value.kind == TomlValue::Kind::String) {
        const auto constraint = VersionConstraint::parse(value.string_value);
        if (!constraint) {
            return std::nullopt;
        }
        spec.constraint = *constraint;
        spec.source = DependencySourceKind::Registry;
        return spec;
    }
    if (value.kind != TomlValue::Kind::Table) {
        return std::nullopt;
    }
    if (const auto path = toml_get_string(value, "path")) {
        spec.source = DependencySourceKind::Path;
        spec.path = *path;
        spec.constraint = VersionConstraint{};
        spec.constraint.kind = VersionConstraintKind::Exact;
        spec.constraint.version = SemVer{0, 0, 0};
        spec.constraint.raw = "0.0.0";
        return spec;
    }
    if (const auto git = toml_get_string(value, "git")) {
        spec.source = DependencySourceKind::Git;
        spec.git_url = *git;
        if (const auto ref = toml_get_string(value, "ref")) {
            spec.git_ref = *ref;
        }
        if (const auto version = toml_get_string(value, "version")) {
            const auto constraint = VersionConstraint::parse(*version);
            if (constraint) {
                spec.constraint = *constraint;
            }
        } else {
            spec.constraint = VersionConstraint{};
            spec.constraint.kind = VersionConstraintKind::Exact;
            spec.constraint.version = SemVer{0, 0, 0};
            spec.constraint.raw = "0.0.0";
        }
        return spec;
    }
    return std::nullopt;
}

std::optional<ProjectManifest> load_manifest_file(const std::string& path, std::string* error) {
    const auto content = read_file(path);
    if (!content) {
        if (error) {
            *error = "cannot read manifest: " + path;
        }
        return std::nullopt;
    }
    const auto document = parse_toml(*content, error);
    if (!document) {
        return std::nullopt;
    }
    ProjectManifest manifest;
    const TomlValue* project = toml_table_lookup(document->root, "project");
    if (project == nullptr) {
        if (error) {
            *error = "missing [project] section";
        }
        return std::nullopt;
    }
    if (const auto name = toml_get_string(*project, "name")) {
        manifest.name = *name;
    }
    if (const auto version = toml_get_string(*project, "version")) {
        manifest.version = *version;
    }
    if (const auto description = toml_get_string(*project, "description")) {
        manifest.description = *description;
    }
    if (const auto license = toml_get_string(*project, "license")) {
        manifest.license = *license;
    }
    if (const auto authors = toml_get_string(*project, "authors")) {
        manifest.authors = *authors;
    }
    if (manifest.name.empty() || manifest.version.empty()) {
        if (error) {
            *error = "project name and version are required";
        }
        return std::nullopt;
    }
    if (!validate_package_name(manifest.name, error)) {
        return std::nullopt;
    }
    if (document->root.kind == TomlValue::Kind::Table) {
        for (const auto& [section, value] : document->root.table) {
            if (section == "dependencies") {
                load_dependency_table(value, false, manifest.dependencies);
                continue;
            }
            if (section.starts_with("dependencies.")) {
                const std::string name = section.substr(std::string_view("dependencies.").size());
                if (const auto spec = parse_dependency_entry(name, value, false)) {
                    manifest.dependencies[name] = *spec;
                }
                continue;
            }
            if (section == "dev-dependencies") {
                load_dependency_table(value, true, manifest.dev_dependencies);
                continue;
            }
            if (section.starts_with("dev-dependencies.")) {
                const std::string name = section.substr(std::string_view("dev-dependencies.").size());
                if (const auto spec = parse_dependency_entry(name, value, true)) {
                    manifest.dev_dependencies[name] = *spec;
                }
            }
        }
    }
    return manifest;
}

std::string format_manifest_toml(const ProjectManifest& manifest) {
    TomlDocument document;
    document.root.kind = TomlValue::Kind::Table;
    TomlValue& project = document.root.table.emplace("project", TomlValue{}).first->second;
    project.kind = TomlValue::Kind::Table;
    project.table["name"].kind = TomlValue::Kind::String;
    project.table["name"].string_value = manifest.name;
    project.table["version"].kind = TomlValue::Kind::String;
    project.table["version"].string_value = manifest.version;
    if (!manifest.description.empty()) {
        project.table["description"].kind = TomlValue::Kind::String;
        project.table["description"].string_value = manifest.description;
    }
    if (!manifest.license.empty()) {
        project.table["license"].kind = TomlValue::Kind::String;
        project.table["license"].string_value = manifest.license;
    }
    if (!manifest.authors.empty()) {
        project.table["authors"].kind = TomlValue::Kind::String;
        project.table["authors"].string_value = manifest.authors;
    }
    if (!manifest.dependencies.empty()) {
        TomlValue& deps = document.root.table.emplace("dependencies", TomlValue{}).first->second;
        deps.kind = TomlValue::Kind::Table;
        for (const auto& [name, spec] : manifest.dependencies) {
            TomlValue& entry = deps.table[name];
            entry.kind = TomlValue::Kind::String;
            if (spec.source == DependencySourceKind::Registry) {
                entry.string_value = spec.constraint.raw.empty() ? "0.0.0" : spec.constraint.raw;
            } else if (spec.source == DependencySourceKind::Path) {
                entry.kind = TomlValue::Kind::Table;
                entry.table["path"].kind = TomlValue::Kind::String;
                entry.table["path"].string_value = spec.path;
            } else {
                entry.kind = TomlValue::Kind::Table;
                entry.table["git"].kind = TomlValue::Kind::String;
                entry.table["git"].string_value = spec.git_url;
                if (!spec.git_ref.empty()) {
                    entry.table["ref"].kind = TomlValue::Kind::String;
                    entry.table["ref"].string_value = spec.git_ref;
                }
            }
        }
    }
    if (!manifest.dev_dependencies.empty()) {
        TomlValue& deps = document.root.table.emplace("dev-dependencies", TomlValue{}).first->second;
        deps.kind = TomlValue::Kind::Table;
        for (const auto& [name, spec] : manifest.dev_dependencies) {
            TomlValue& entry = deps.table[name];
            entry.kind = TomlValue::Kind::String;
            entry.string_value = spec.constraint.raw.empty() ? "0.0.0" : spec.constraint.raw;
        }
    }
    return serialize_toml(document);
}

bool write_manifest_file(const std::string& path, const ProjectManifest& manifest, std::string* error) {
    const std::string text = format_manifest_toml(manifest);
    if (!write_file_atomically(path, text, error)) {
        if (error && error->empty()) {
            *error = "cannot write manifest: " + path;
        }
        return false;
    }
    return true;
}

}  // namespace pscript::package
