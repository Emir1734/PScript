#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pscript::archive {

struct ArchiveEntryInfo {
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t compressed_size = 0;
    bool is_file = true;
    bool is_directory = false;
    bool is_symlink = false;
    char tar_typeflag = '0';
    std::int64_t modified_unix = 0;
    std::uint32_t mode = 0;
    std::string symlink_target;
};

struct ArchiveLimits {
    std::uint64_t max_entries = 1'000'000;
    std::uint64_t max_single_file_bytes = 8ULL * 1024 * 1024 * 1024;
    std::uint64_t max_total_extract_bytes = 32ULL * 1024 * 1024 * 1024;
};

enum class ArchiveFormat {
    Zip,
    Tar,
    TarGz,
};

enum class ArchiveMode {
    Read,
    Write,
};

class ArchiveBackend {
public:
    virtual ~ArchiveBackend() = default;
    virtual void close() = 0;
    virtual bool closed() const = 0;
    virtual const std::vector<ArchiveEntryInfo>& entries() const = 0;
    virtual std::vector<std::uint8_t> read_bytes(const std::string& name) = 0;
    virtual void write_bytes(const std::string& name, const std::vector<std::uint8_t>& data) = 0;
    virtual void add_path(const std::string& filesystem_path, const std::string& archive_name) = 0;
    virtual void extract_entry(const std::string& name, const std::string& destination_dir) = 0;
    virtual void extract_all(const std::string& destination_dir) = 0;
    virtual void finalize() = 0;
};

ArchiveFormat detect_format_from_path(const std::string& path);
std::string format_to_string(ArchiveFormat format);

std::unique_ptr<ArchiveBackend> open_archive_backend(const std::string& path, ArchiveFormat format, ArchiveMode mode,
                                                     int zip_compression_level, const ArchiveLimits& limits);

}  // namespace pscript::archive
