#include "package/lockfile.h"

#include "package/atomic_file.h"

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

DependencySourceKind parse_source_kind(const std::string& text) {
    if (text == "path") {
        return DependencySourceKind::Path;
    }
    if (text == "git") {
        return DependencySourceKind::Git;
    }
    return DependencySourceKind::Registry;
}

std::string source_kind_name(DependencySourceKind kind) {
    switch (kind) {
        case DependencySourceKind::Path:
            return "path";
        case DependencySourceKind::Git:
            return "git";
        default:
            return "registry";
    }
}

}  // namespace

std::optional<Lockfile> load_lockfile(const std::string& path, std::string* error) {
    const auto content = read_file(path);
    if (!content) {
        if (error) {
            *error = "cannot read lockfile: " + path;
        }
        return std::nullopt;
    }
    const auto document = parse_toml(*content, error);
    if (!document) {
        return std::nullopt;
    }
    Lockfile lockfile;
    if (document->root.kind != TomlValue::Kind::Table) {
        return lockfile;
    }
    for (const auto& [section, value] : document->root.table) {
        constexpr std::string_view kPrefix = "packages.";
        if (!section.starts_with(kPrefix) || value.kind != TomlValue::Kind::Table) {
            continue;
        }
        LockedPackage entry;
        entry.name = section.substr(kPrefix.size());
        if (const auto version = toml_get_string(value, "version")) {
            entry.version = *version;
        }
        if (const auto checksum = toml_get_string(value, "checksum")) {
            entry.checksum = *checksum;
        }
        if (const auto source = toml_get_string(value, "source")) {
            entry.source = parse_source_kind(*source);
        }
        if (const auto path_value = toml_get_string(value, "path")) {
            entry.path = *path_value;
        }
        if (const auto git = toml_get_string(value, "git")) {
            entry.git_url = *git;
        }
        if (const auto ref = toml_get_string(value, "ref")) {
            entry.git_ref = *ref;
        }
        if (const auto install_dir = toml_get_string(value, "install_dir")) {
            entry.install_dir = *install_dir;
        }
        lockfile.packages.push_back(std::move(entry));
    }
    return lockfile;
}

std::string format_lockfile_toml(const Lockfile& lockfile) {
    TomlDocument document;
    document.root.kind = TomlValue::Kind::Table;
    for (const LockedPackage& entry : lockfile.packages) {
        const std::string section = "packages." + entry.name;
        TomlValue& pkg = document.root.table.emplace(section, TomlValue{}).first->second;
        pkg.kind = TomlValue::Kind::Table;
        pkg.table["version"].kind = TomlValue::Kind::String;
        pkg.table["version"].string_value = entry.version;
        pkg.table["checksum"].kind = TomlValue::Kind::String;
        pkg.table["checksum"].string_value = entry.checksum;
        pkg.table["source"].kind = TomlValue::Kind::String;
        pkg.table["source"].string_value = source_kind_name(entry.source);
        if (!entry.path.empty()) {
            pkg.table["path"].kind = TomlValue::Kind::String;
            pkg.table["path"].string_value = entry.path;
        }
        if (!entry.git_url.empty()) {
            pkg.table["git"].kind = TomlValue::Kind::String;
            pkg.table["git"].string_value = entry.git_url;
        }
        if (!entry.git_ref.empty()) {
            pkg.table["ref"].kind = TomlValue::Kind::String;
            pkg.table["ref"].string_value = entry.git_ref;
        }
        pkg.table["install_dir"].kind = TomlValue::Kind::String;
        pkg.table["install_dir"].string_value = entry.install_dir;
    }
    return serialize_toml(document);
}

bool write_lockfile(const std::string& path, const Lockfile& lockfile, std::string* error) {
    const std::string text = format_lockfile_toml(lockfile);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) {
            *error = "cannot write lockfile: " + path;
        }
        return false;
    }
    out << text;
    return static_cast<bool>(out);
}

const LockedPackage* find_locked_package(const Lockfile& lockfile, std::string_view name) {
    for (const LockedPackage& entry : lockfile.packages) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

}  // namespace pscript::package
