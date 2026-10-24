#include "package/registry.h"

#include "package/checksum.h"
#include "package/package_archive.h"
#include "package/package_identity.h"
#include "package/registry_client.h"
#include "runtime/http_ops.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace pscript::package {
namespace {

std::optional<std::filesystem::path> local_registry_directory(const std::string& registry_base) {
    std::error_code ec;
    std::filesystem::path path(registry_base);
    if (path.is_relative()) {
        path = std::filesystem::absolute(path);
    }
    path = path.lexically_normal();
    if (std::filesystem::is_directory(path, ec)) {
        return path;
    }
    return std::nullopt;
}

std::string join_url(const std::string& base, const std::string& suffix) {
    if (base.empty()) {
        return suffix;
    }
    if (base.back() == '/') {
        return base + suffix;
    }
    return base + '/' + suffix;
}

std::vector<SemVer> parse_version_list_json(const std::string& body) {
    std::vector<SemVer> versions;
    std::size_t index = 0;
    while (index < body.size()) {
        const std::size_t quote = body.find('"', index);
        if (quote == std::string::npos) {
            break;
        }
        const std::size_t end_quote = body.find('"', quote + 1);
        if (end_quote == std::string::npos) {
            break;
        }
        const std::string_view token = body.substr(quote + 1, end_quote - quote - 1);
        const auto version = SemVer::parse(token);
        if (version) {
            versions.push_back(*version);
        }
        index = end_quote + 1;
    }
    return versions;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

bool verify_registry_checksum_file(const std::filesystem::path& checksum_file,
                                   const std::filesystem::path& installed_dir, std::string* error) {
    if (!std::filesystem::exists(checksum_file)) {
        return true;
    }
    std::ifstream in(checksum_file, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot read registry checksum: " + checksum_file.string();
        }
        return false;
    }
    std::string expected;
    in >> expected;
    if (!verify_directory_checksum(installed_dir.string(), expected, error)) {
        return false;
    }
    return true;
}

bool extract_registry_tar_gz(const std::filesystem::path& archive, const std::filesystem::path& destination,
                             const std::optional<std::filesystem::path>& checksum_file, std::string* error) {
    if (!extract_tar_gz_archive(archive, destination, error)) {
        return false;
    }
    if (checksum_file.has_value()) {
        if (!verify_registry_checksum_file(*checksum_file, destination, error)) {
            std::error_code ec;
            std::filesystem::remove_all(destination, ec);
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<SemVer> registry_list_versions(const std::string& registry_base, const std::string& package_name,
                                           std::string* error) {
    if (const std::optional<std::filesystem::path> local = local_registry_directory(registry_base)) {
        const std::filesystem::path index =
            registry_package_root(*local, package_name) / "versions.json";
        std::ifstream in(index, std::ios::binary);
        if (!in) {
            if (error) {
                *error = "local registry index missing: " + index.string();
            }
            return {};
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::vector<SemVer> versions = parse_version_list_json(buffer.str());
        if (!versions.empty()) {
            return versions;
        }
        const std::filesystem::path package_root = registry_package_root(*local, package_name);
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(package_root, ec)) {
            if (!entry.is_directory()) {
                continue;
            }
            const auto version = SemVer::parse(entry.path().filename().string());
            if (version) {
                versions.push_back(*version);
            }
        }
        return versions;
    }
    const std::string url = join_url(registry_base, registry_http_package_path(package_name) + "/versions");
    try {
        const HttpClientResponse response = http_request_sync("GET", url, {}, {});
        if (response.status < 200 || response.status >= 300) {
            if (error) {
                *error = "registry versions request failed with status " + std::to_string(response.status);
            }
            return {};
        }
        const std::string body(reinterpret_cast<const char*>(response.body.data()), response.body.size());
        return parse_version_list_json(body);
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return {};
    }
}

bool registry_download_package(const std::string& registry_base, const std::string& package_name,
                               const SemVer& version, const std::string& destination_dir, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(destination_dir, ec);
    if (const std::optional<std::filesystem::path> local = local_registry_directory(registry_base)) {
        const std::filesystem::path package_root = registry_package_root(*local, package_name);
        const std::filesystem::path archive = package_root / version.to_string() / "package.tar.gz";
        if (!std::filesystem::exists(archive)) {
            const std::filesystem::path source = package_root / version.to_string() / "source";
            if (std::filesystem::exists(source)) {
                std::filesystem::copy(source, destination_dir,
                                      std::filesystem::copy_options::recursive |
                                          std::filesystem::copy_options::overwrite_existing,
                                      ec);
                if (ec) {
                    if (error) {
                        *error = ec.message();
                    }
                    return false;
                }
                const std::filesystem::path checksum_file = package_root / version.to_string() / "checksum.txt";
                if (!verify_registry_checksum_file(checksum_file, destination_dir, error)) {
                    std::filesystem::remove_all(destination_dir, ec);
                    return false;
                }
                return true;
            }
            if (error) {
                *error = "local registry package missing: " + archive.string();
            }
            return false;
        }
        const std::filesystem::path checksum_file = package_root / version.to_string() / "checksum.txt";
        const bool ok = extract_registry_tar_gz(archive, destination_dir, checksum_file, error);
        return ok;
    }
    const std::string url =
        join_url(registry_base, registry_http_package_path(package_name) + "/" + version.to_string() + "/download");
    try {
        const HttpClientResponse response = http_request_sync("GET", url, {}, {});
        if (response.status < 200 || response.status >= 300) {
            if (error) {
                *error = "registry download failed with status " + std::to_string(response.status);
            }
            return false;
        }
        const std::filesystem::path archive = std::filesystem::path(destination_dir) / "package.tar.gz";
        if (!write_bytes(archive, response.body)) {
            if (error) {
                *error = "cannot write downloaded archive";
            }
            return false;
        }
        if (!extract_tar_gz_archive(archive, destination_dir, error)) {
            std::filesystem::remove(archive, ec);
            return false;
        }
        std::filesystem::remove(archive, ec);
        return true;
    } catch (const std::exception& ex) {
        if (error) {
            *error = ex.what();
        }
        return false;
    }
}

bool registry_publish_package(const std::string& registry_base, const std::string& package_name,
                              const SemVer& version, const std::string& source_dir, const std::string& token,
                              std::string* error) {
    if (!validate_package_name(package_name, error)) {
        return false;
    }
    if (const auto scoped = parse_scoped_package_id(package_name)) {
        if (local_registry_directory(registry_base).has_value()) {
            if (!registry_ensure_username_owner(registry_base, scoped->username, token, error)) {
                return false;
            }
        } else if (token.empty()) {
            if (error) {
                *error = "publish token required for scoped packages";
            }
            return false;
        }
    }
    const std::string checksum = sha256_directory(source_dir, error);
    if (checksum.empty()) {
        return false;
    }
    if (const std::optional<std::filesystem::path> local = local_registry_directory(registry_base)) {
        const std::filesystem::path package_root = registry_package_root(*local, package_name);
        const std::filesystem::path version_dir = package_root / version.to_string();
        if (std::filesystem::exists(version_dir)) {
            if (error) {
                *error = "package version already published: " + package_name + "@" + version.to_string();
            }
            return false;
        }
        const std::filesystem::path dest = version_dir / "source";
        std::error_code ec;
        std::filesystem::create_directories(dest, ec);
        if (ec) {
            if (error) {
                *error = ec.message();
            }
            return false;
        }
        std::filesystem::copy(source_dir, dest,
                              std::filesystem::copy_options::recursive |
                                  std::filesystem::copy_options::overwrite_existing,
                              ec);
        if (ec) {
            if (error) {
                *error = ec.message();
            }
            return false;
        }
        std::ofstream meta(dest.parent_path() / "checksum.txt", std::ios::trunc);
        meta << checksum;
        const std::filesystem::path archive = dest.parent_path() / "package.tar.gz";
        if (!create_directory_tar_gz(std::filesystem::path(source_dir), archive, error)) {
            std::filesystem::remove_all(version_dir, ec);
            return false;
        }
        const std::filesystem::path versions_file = package_root / "versions.json";
        std::filesystem::create_directories(versions_file.parent_path(), ec);
        std::ofstream versions(versions_file, std::ios::trunc);
        versions << "[\"" << version.to_string() << "\"]";
        versions.flush();
        return versions.good();
    }
    const std::filesystem::path temp_archive =
        std::filesystem::temp_directory_path() / "pscript_publish_upload.tar.gz";
    if (!create_directory_tar_gz(std::filesystem::path(source_dir), temp_archive, error)) {
        return false;
    }
    std::ifstream in(temp_archive, std::ios::binary);
    const std::vector<std::uint8_t> payload((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::error_code ec;
    std::filesystem::remove(temp_archive, ec);
    std::string base = registry_base;
    if (base.rfind("http://", 0) == 0 || base.rfind("https://", 0) == 0) {
        if (base.size() < 3 || base.substr(base.size() - 3) != "/v1") {
            base = join_url(base, "v1");
        }
    }
    const std::string url = join_url(base, registry_http_package_path(package_name) + "/publish");
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/gzip";
    headers["X-PScript-Version"] = version.to_string();
    if (!token.empty()) {
        headers["Authorization"] = "Bearer " + token;
    }
    try {
        const HttpClientResponse response = http_request_sync("POST", url, payload, headers);
        if (response.status == 409) {
            if (error) {
                *error = "package version already published";
            }
            return false;
        }
        if (response.status < 200 || response.status >= 300) {
            if (error) {
                *error = "publish failed";
            }
            return false;
        }
        return true;
    } catch (const std::exception&) {
        if (error) {
            *error = "publish failed";
        }
        return false;
    }
}

std::vector<std::string> registry_search_packages(const std::string& registry_base, const std::string& query, int limit,
                                                  std::string* error) {
    if (local_registry_directory(registry_base).has_value()) {
        if (error) {
            *error = "search is not supported for local directory registries";
        }
        return {};
    }
    return registry_remote_search(registry_base, query, limit, error);
}

}  // namespace pscript::package
