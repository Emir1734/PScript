#include "package/package_identity.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace pscript::package {
namespace {

bool is_segment_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-';
}

bool read_username_record(const std::filesystem::path& path, std::string_view username, std::string* token_out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string_view key(line.data(), eq);
        if (key == username) {
            if (token_out) {
                *token_out = line.substr(eq + 1);
            }
            return true;
        }
    }
    return false;
}

bool write_username_record(const std::filesystem::path& path, std::string_view username, std::string_view token) {
    std::ostringstream updated;
    bool replaced = false;
    {
        std::ifstream in(path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) {
                updated << line << '\n';
                continue;
            }
            const std::string_view key(line.data(), eq);
            if (key == username) {
                updated << username << '=' << token << '\n';
                replaced = true;
            } else {
                updated << line << '\n';
            }
        }
    }
    if (!replaced) {
        updated << username << '=' << token << '\n';
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << updated.str();
    return static_cast<bool>(out);
}

}  // namespace

std::string ScopedPackageId::canonical() const {
    return '@' + username + '/' + package;
}

bool is_valid_username(std::string_view username) {
    if (username.empty() || username.size() > 64) {
        return false;
    }
    if (!std::isalpha(static_cast<unsigned char>(username.front()))) {
        return false;
    }
    for (char ch : username) {
        if (!is_segment_char(ch)) {
            return false;
        }
    }
    return true;
}

bool is_valid_unscoped_package_name(std::string_view name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    if (name.front() == '@' || name.find('/') != std::string_view::npos) {
        return false;
    }
    for (char ch : name) {
        if (!is_segment_char(ch)) {
            return false;
        }
    }
    return true;
}

bool is_scoped_package_name(std::string_view name) {
    return parse_scoped_package_id(name).has_value();
}

std::optional<ScopedPackageId> parse_scoped_package_id(std::string_view name) {
    if (name.empty() || name.front() != '@') {
        return std::nullopt;
    }
    const std::size_t slash = name.find('/');
    if (slash == std::string_view::npos || slash <= 1 || slash + 1 >= name.size()) {
        return std::nullopt;
    }
    const std::string_view username = name.substr(1, slash - 1);
    const std::string_view package = name.substr(slash + 1);
    if (!is_valid_username(username) || !is_valid_unscoped_package_name(package)) {
        return std::nullopt;
    }
    ScopedPackageId id;
    id.username = std::string(username);
    id.package = std::string(package);
    return id;
}

std::optional<RegistryDependencyRef> parse_registry_dependency_arg(std::string_view arg, std::string* error) {
    if (!arg.starts_with('@')) {
        return std::nullopt;
    }
    const std::size_t slash = arg.find('/');
    if (slash == std::string_view::npos) {
        if (error) {
            *error = "expected @username/package";
        }
        return std::nullopt;
    }
    std::string constraint = "^0.0.0";
    std::string_view scoped_part = arg;
    const std::size_t version_at = arg.find('@', slash + 1);
    if (version_at != std::string_view::npos) {
        constraint = std::string(arg.substr(version_at + 1));
        if (constraint.empty()) {
            if (error) {
                *error = "missing version constraint";
            }
            return std::nullopt;
        }
        scoped_part = arg.substr(0, version_at);
    }
    const std::optional<ScopedPackageId> scoped = parse_scoped_package_id(scoped_part);
    if (!scoped) {
        if (error) {
            *error = "invalid scoped package name";
        }
        return std::nullopt;
    }
    RegistryDependencyRef ref;
    ref.canonical_name = scoped->canonical();
    ref.version_constraint = constraint;
    return ref;
}

bool validate_package_name(std::string_view name, std::string* error) {
    if (is_scoped_package_name(name)) {
        return true;
    }
    if (is_valid_unscoped_package_name(name)) {
        return true;
    }
    if (error) {
        *error = "invalid package name '" + std::string(name) +
                 "' (expected @username/package or legacy unscoped name)";
    }
    return false;
}

std::filesystem::path registry_package_root(const std::filesystem::path& registry_base, std::string_view package_name) {
    if (const auto scoped = parse_scoped_package_id(package_name)) {
        return registry_base / "packages" / scoped->username / scoped->package;
    }
    return registry_base / "packages" / package_name;
}

std::string registry_http_package_path(std::string_view package_name) {
    if (const auto scoped = parse_scoped_package_id(package_name)) {
        return "packages/" + scoped->username + "/" + scoped->package;
    }
    return "packages/" + std::string(package_name);
}

std::string install_dir_basename(std::string_view package_name) {
    if (const auto scoped = parse_scoped_package_id(package_name)) {
        return scoped->username + "__" + scoped->package;
    }
    return std::string(package_name);
}

bool registry_ensure_username_owner(const std::filesystem::path& registry_base, std::string_view username,
                                    std::string_view token, std::string* error) {
    if (!is_valid_username(username)) {
        if (error) {
            *error = "invalid registry username";
        }
        return false;
    }
    if (token.empty()) {
        if (error) {
            *error = "publish token required for scoped packages";
        }
        return false;
    }
    if (!std::filesystem::is_directory(registry_base)) {
        return true;
    }
    const std::filesystem::path owners = registry_base / "usernames.txt";
    std::string existing;
    if (read_username_record(owners, username, &existing)) {
        if (existing != token) {
            if (error) {
                *error = "username '" + std::string(username) + "' is owned by another account";
            }
            return false;
        }
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(registry_base, ec);
    if (!write_username_record(owners, username, token)) {
        if (error) {
            *error = "cannot write username registry";
        }
        return false;
    }
    return true;
}

bool registry_verify_username_token(const std::filesystem::path& registry_base, std::string_view username,
                                    std::string_view token, std::string* error) {
    if (!is_valid_username(username)) {
        if (error) {
            *error = "invalid registry username";
        }
        return false;
    }
    if (!std::filesystem::is_directory(registry_base)) {
        return true;
    }
    const std::filesystem::path owners = registry_base / "usernames.txt";
    std::string existing;
    if (!read_username_record(owners, username, &existing)) {
        if (error) {
            *error = "username '" + std::string(username) + "' is not registered";
        }
        return false;
    }
    if (existing != token) {
        if (error) {
            *error = "invalid publish token for username '" + std::string(username) + "'";
        }
        return false;
    }
    return true;
}

}  // namespace pscript::package
