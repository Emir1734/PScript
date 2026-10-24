#include "stdlib/archive/archive_security.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace pscript::archive {
namespace {

bool is_windows_drive_absolute(std::string_view path) {
    if (path.size() < 2) {
        return false;
    }
    return std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

std::string replace_backslashes(std::string_view path) {
    std::string out(path);
    for (char& ch : out) {
        if (ch == '\\') {
            ch = '/';
        }
    }
    return out;
}

}  // namespace

bool is_absolute_archive_path(std::string_view path) {
    if (path.empty()) {
        return false;
    }
    if (path.front() == '/') {
        return true;
    }
    if (is_windows_drive_absolute(path)) {
        return true;
    }
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        return true;
    }
    return false;
}

std::optional<std::string> normalize_archive_entry_path(std::string_view raw, bool is_directory) {
    if (raw.empty()) {
        return std::nullopt;
    }
    std::string path = replace_backslashes(raw);
    while (!path.empty() && path.front() == '/') {
        path.erase(path.begin());
    }
    if (path.empty()) {
        return std::nullopt;
    }
    if (is_absolute_archive_path(path)) {
        return std::nullopt;
    }
    if (path.find("..") != std::string::npos) {
        const auto is_dotdot_token = [](const std::string& part) { return part == ".."; };
        std::size_t start = 0;
        while (start < path.size()) {
            const std::size_t end = path.find('/', start);
            const std::string part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (is_dotdot_token(part)) {
                return std::nullopt;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    if (is_directory && path.back() != '/') {
        path.push_back('/');
    }
    return path;
}

std::filesystem::path secure_extract_path(const std::filesystem::path& destination_root,
                                            const std::string& entry_path) {
    const std::filesystem::path root = std::filesystem::absolute(destination_root).lexically_normal();
    const std::filesystem::path resolved =
        std::filesystem::absolute(root / std::filesystem::path(entry_path)).lexically_normal();
    const auto rel = std::filesystem::relative(resolved, root);
    if (rel.empty()) {
        return resolved;
    }
    for (const auto& part : rel) {
        if (part == "..") {
            throw std::runtime_error("archive extraction path escapes destination");
        }
    }
    return resolved;
}

void check_extract_limits(const ArchiveLimits& limits, std::uint64_t entry_count, std::uint64_t total_uncompressed,
                          std::uint64_t single_file_size) {
    if (entry_count > limits.max_entries) {
        throw std::runtime_error("archive entry count exceeds limit");
    }
    if (single_file_size > limits.max_single_file_bytes) {
        throw std::runtime_error("archive entry size exceeds limit");
    }
    if (total_uncompressed > limits.max_total_extract_bytes) {
        throw std::runtime_error("archive total extracted size exceeds limit");
    }
}

bool is_safe_symlink_target(std::string_view target, const std::filesystem::path& destination_root) {
    if (target.empty()) {
        return false;
    }
    const std::string normalized = normalize_archive_entry_path(target, false).value_or("");
    if (normalized.empty() && !target.empty() && target.find("..") != std::string_view::npos) {
        return false;
    }
    if (is_absolute_archive_path(target)) {
        return false;
    }
    try {
        secure_extract_path(destination_root, normalized.empty() ? std::string(target) : normalized);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace pscript::archive
