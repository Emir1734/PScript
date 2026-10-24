#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace pscript::package {

struct ScopedPackageId {
    std::string username;
    std::string package;

    std::string canonical() const;
};

bool is_valid_username(std::string_view username);
bool is_valid_unscoped_package_name(std::string_view name);
bool is_scoped_package_name(std::string_view name);

std::optional<ScopedPackageId> parse_scoped_package_id(std::string_view name);

// Validates manifest/dependency names. Legacy unscoped names remain allowed for local/path workflows.
bool validate_package_name(std::string_view name, std::string* error = nullptr);

struct RegistryDependencyRef {
    std::string canonical_name;
    std::string version_constraint;
};

std::optional<RegistryDependencyRef> parse_registry_dependency_arg(std::string_view arg, std::string* error = nullptr);

std::filesystem::path registry_package_root(const std::filesystem::path& registry_base, std::string_view package_name);

std::string registry_http_package_path(std::string_view package_name);

std::string install_dir_basename(std::string_view package_name);

bool registry_ensure_username_owner(const std::filesystem::path& registry_base, std::string_view username,
                                    std::string_view token, std::string* error = nullptr);

bool registry_verify_username_token(const std::filesystem::path& registry_base, std::string_view username,
                                    std::string_view token, std::string* error = nullptr);

}  // namespace pscript::package
