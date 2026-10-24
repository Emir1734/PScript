#include "stdlib/archive/archive_zip.h"

#include "stdlib/archive/archive_security.h"

#include "miniz.h"
#include "miniz_zip.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace pscript::archive {
namespace {

// Allow reading while writing for append/update; miniz switches to ZIP64 automatically when required.
constexpr std::uint32_t kZipWriterFlags = MZ_ZIP_FLAG_WRITE_ALLOW_READING;

std::string path_to_utf8(const std::filesystem::path& path) {
    return path.generic_string();
}

[[noreturn]] void throw_zip_error(mz_zip_archive& archive, const char* context);

constexpr std::size_t kZipLocalHeaderSize = 30;
constexpr mz_uint kMaxZipFilenameLen = 4096;
constexpr mz_uint kMaxZipExtraLen = 65536;

mz_uint16 read_le16(const mz_uint8* data) {
    return static_cast<mz_uint16>(data[0]) | (static_cast<mz_uint16>(data[1]) << 8);
}

void validate_zip_local_header(mz_zip_archive& archive, const mz_zip_archive_file_stat& stat) {
    const mz_uint64 archive_size = mz_zip_get_archive_size(&archive);
    if (stat.m_local_header_ofs + kZipLocalHeaderSize > archive_size) {
        throw std::runtime_error("invalid zip entry metadata");
    }
    mz_uint8 local_header[kZipLocalHeaderSize];
    if (archive.m_pRead(archive.m_pIO_opaque, stat.m_local_header_ofs, local_header, kZipLocalHeaderSize) !=
        kZipLocalHeaderSize) {
        throw_zip_error(archive, "local header read");
    }
    if (local_header[0] != 0x50 || local_header[1] != 0x4b || local_header[2] != 0x03 || local_header[3] != 0x04) {
        throw std::runtime_error("invalid zip local header");
    }
    const mz_uint filename_len = read_le16(local_header + 26);
    const mz_uint extra_len = read_le16(local_header + 28);
    if (filename_len > kMaxZipFilenameLen || extra_len > kMaxZipExtraLen) {
        throw std::runtime_error("invalid zip entry metadata");
    }
    const mz_uint64 data_offset = stat.m_local_header_ofs + kZipLocalHeaderSize + filename_len + extra_len;
    if (data_offset > archive_size) {
        throw std::runtime_error("invalid zip entry metadata");
    }
    if (stat.m_comp_size > 0 && data_offset + stat.m_comp_size > archive_size) {
        throw std::runtime_error("invalid zip entry metadata");
    }
}

[[noreturn]] void throw_zip_error(mz_zip_archive& archive, const char* context) {
    const mz_zip_error code = mz_zip_get_last_error(&archive);
    const char* detail = mz_zip_get_error_string(code);
    if (detail == nullptr || detail[0] == '\0') {
        detail = "corrupt or invalid zip data";
    }
    throw std::runtime_error(std::string("zip archive error (") + context + "): " + detail);
}

void ensure_zip_entry_readable(mz_zip_archive& archive, mz_uint file_index, const ArchiveLimits& limits) {
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, file_index, &stat)) {
        throw_zip_error(archive, "entry metadata");
    }
    if (stat.m_is_directory || stat.m_uncomp_size == 0) {
        return;
    }
    const mz_uint64 archive_size = mz_zip_get_archive_size(&archive);
    if (stat.m_local_header_ofs >= archive_size) {
        throw std::runtime_error("invalid zip entry metadata");
    }
    if (stat.m_comp_size > archive_size) {
        throw std::runtime_error("invalid zip entry metadata");
    }
    if (stat.m_uncomp_size > static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
        throw std::runtime_error("zip entry too large");
    }
    check_extract_limits(limits, 1, stat.m_uncomp_size, stat.m_uncomp_size);
    validate_zip_local_header(archive, stat);
}

class ZipArchiveBackend final : public ArchiveBackend {
public:
    ZipArchiveBackend(const std::string& path, ArchiveMode mode, int compression_level, const ArchiveLimits& limits)
        : path_(path), mode_(mode), compression_level_(compression_level), limits_(limits) {
        std::memset(&archive_, 0, sizeof(archive_));
        if (mode_ == ArchiveMode::Read) {
            if (!mz_zip_reader_init_file(&archive_, path_.c_str(), 0)) {
                throw std::runtime_error("failed to open zip archive for reading");
            }
            refresh_entries();
        } else {
            const bool exists = std::filesystem::exists(path_);
            if (exists) {
                if (!mz_zip_reader_init_file(&archive_, path_.c_str(), 0)) {
                    throw std::runtime_error("failed to open existing zip for update");
                }
                if (!mz_zip_writer_init_from_reader_v2(&archive_, path_.c_str(), kZipWriterFlags)) {
                    mz_zip_reader_end(&archive_);
                    throw std::runtime_error("failed to initialize zip writer for update");
                }
            } else {
                if (!mz_zip_writer_init_file_v2(&archive_, path_.c_str(), 0, kZipWriterFlags)) {
                    throw std::runtime_error("failed to create zip archive");
                }
            }
            writing_ = true;
        }
    }

    ~ZipArchiveBackend() override { close(); }

    void close() override {
        if (closed_) {
            return;
        }
        if (writing_) {
            if (!mz_zip_writer_finalize_archive(&archive_)) {
                mz_zip_writer_end(&archive_);
                closed_ = true;
                throw std::runtime_error("failed to finalize zip archive");
            }
            mz_zip_writer_end(&archive_);
        } else {
            mz_zip_reader_end(&archive_);
        }
        closed_ = true;
        writing_ = false;
    }

    bool closed() const override { return closed_; }

    const std::vector<ArchiveEntryInfo>& entries() const override { return entries_; }

    std::vector<std::uint8_t> read_bytes(const std::string& name) override {
        ensure_read();
        const int index = find_entry_index(name);
        if (index < 0) {
            throw std::runtime_error("archive entry not found");
        }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive_, static_cast<mz_uint>(index), &stat)) {
            throw std::runtime_error("failed to read zip entry metadata");
        }
        check_extract_limits(limits_, entries_.size(), stat.m_uncomp_size, stat.m_uncomp_size);
        if (stat.m_is_directory) {
            return {};
        }
        if (!stat.m_is_supported || stat.m_is_encrypted) {
            throw std::runtime_error("unsupported or encrypted zip entry");
        }
        ensure_zip_entry_readable(archive_, static_cast<mz_uint>(index), limits_);
        const std::size_t size = static_cast<std::size_t>(stat.m_uncomp_size);
        std::vector<std::uint8_t> data(size);
        if (!mz_zip_reader_extract_to_mem(&archive_, static_cast<mz_uint>(index), data.data(), data.size(), 0)) {
            throw_zip_error(archive_, "read entry");
        }
        const mz_uint32 computed_crc =
            static_cast<mz_uint32>(mz_crc32(MZ_CRC32_INIT, data.data(), data.size()));
        if (computed_crc != stat.m_crc32) {
            throw std::runtime_error("zip archive error (read entry): CRC-32 check failed");
        }
        return data;
    }

    void write_bytes(const std::string& name, const std::vector<std::uint8_t>& data) override {
        ensure_write();
        const auto normalized = normalize_archive_entry_path(name, false);
        if (!normalized) {
            throw std::runtime_error("invalid archive entry path");
        }
        check_extract_limits(limits_, entries_.size() + 1, 0, data.size());
        const mz_uint level = static_cast<mz_uint>(std::clamp(compression_level_, 0, 10));
        if (!mz_zip_writer_add_mem(&archive_, normalized->c_str(), data.data(), data.size(), level)) {
            throw std::runtime_error("failed to write zip entry");
        }
        upsert_entry(*normalized, data.size(), data.size(), true, false);
    }

    void add_path(const std::string& filesystem_path, const std::string& archive_name) override {
        ensure_write();
        const std::filesystem::path source(filesystem_path);
        if (!std::filesystem::exists(source)) {
            throw std::runtime_error("path does not exist: " + filesystem_path);
        }
        if (std::filesystem::is_regular_file(source)) {
            const std::string entry_name =
                archive_name.empty() ? normalize_archive_entry_path(source.filename().generic_string(), false).value_or("")
                                     : normalize_archive_entry_path(archive_name, false).value_or("");
            if (entry_name.empty()) {
                throw std::runtime_error("invalid archive entry name");
            }
            const mz_uint level = static_cast<mz_uint>(std::clamp(compression_level_, 0, 10));
            if (!mz_zip_writer_add_file(&archive_, entry_name.c_str(), path_to_utf8(source).c_str(), nullptr, 0,
                                        level)) {
                throw std::runtime_error("failed to add file to zip");
            }
            const auto size = static_cast<std::uint64_t>(std::filesystem::file_size(source));
            upsert_entry(entry_name, size, size, true, false);
            return;
        }
        if (std::filesystem::is_directory(source)) {
            const std::filesystem::path base = archive_name.empty() ? source.filename() : std::filesystem::path(archive_name);
            const std::string base_name = normalize_archive_entry_path(base.generic_string(), true).value_or("");
            if (!base_name.empty()) {
                write_directory_entry(base_name);
            }
            for (const auto& entry : std::filesystem::recursive_directory_iterator(source)) {
                const std::filesystem::path rel = std::filesystem::relative(entry.path(), source);
                const std::string rel_name = (base.empty() ? rel : base / rel).generic_string();
                if (entry.is_directory()) {
                    const auto dir_name = normalize_archive_entry_path(rel_name, true);
                    if (dir_name) {
                        write_directory_entry(*dir_name);
                    }
                    continue;
                }
                if (!entry.is_regular_file()) {
                    continue;
                }
                const auto file_name = normalize_archive_entry_path(rel_name, false);
                if (!file_name) {
                    throw std::runtime_error("invalid archive entry path during directory add");
                }
                const mz_uint level = static_cast<mz_uint>(std::clamp(compression_level_, 0, 10));
                if (!mz_zip_writer_add_file(&archive_, file_name->c_str(), path_to_utf8(entry.path()).c_str(), nullptr,
                                            0, level)) {
                    throw std::runtime_error("failed to add file to zip");
                }
                const auto size = static_cast<std::uint64_t>(entry.file_size());
                upsert_entry(*file_name, size, size, true, false);
            }
        }
    }

    void extract_entry(const std::string& name, const std::string& destination_dir) override {
        ensure_read();
        const int index = find_entry_index(name);
        if (index < 0) {
            throw std::runtime_error("archive entry not found");
        }
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&archive_, static_cast<mz_uint>(index), &stat)) {
            throw std::runtime_error("failed to read zip entry metadata");
        }
        const std::string entry_path = normalize_archive_entry_path(stat.m_filename, stat.m_is_directory).value_or("");
        if (entry_path.empty()) {
            throw std::runtime_error("invalid zip entry path");
        }
        const std::filesystem::path dest_root(destination_dir);
        const std::filesystem::path out_path = secure_extract_path(dest_root, entry_path);
        check_extract_limits(limits_, entries_.size(), stat.m_uncomp_size, stat.m_uncomp_size);
        if (stat.m_is_directory) {
            std::filesystem::create_directories(out_path);
            return;
        }
        std::filesystem::create_directories(out_path.parent_path());
        ensure_zip_entry_readable(archive_, static_cast<mz_uint>(index), limits_);
        if (!mz_zip_reader_extract_to_file(&archive_, static_cast<mz_uint>(index), path_to_utf8(out_path).c_str(), 0)) {
            throw_zip_error(archive_, "extract entry");
        }
    }

    void extract_all(const std::string& destination_dir) override {
        ensure_read();
        std::uint64_t total = 0;
        for (const auto& entry : entries_) {
            total += entry.size;
            check_extract_limits(limits_, entries_.size(), total, entry.size);
            extract_entry(entry.name, destination_dir);
        }
    }

    void finalize() override { close(); }

private:
    void ensure_read() const {
        if (mode_ != ArchiveMode::Read) {
            throw std::runtime_error("archive is not open for reading");
        }
        if (closed_) {
            throw std::runtime_error("archive is closed");
        }
    }

    void ensure_write() {
        if (mode_ != ArchiveMode::Write) {
            throw std::runtime_error("archive is not open for writing");
        }
        if (closed_) {
            throw std::runtime_error("archive is closed");
        }
    }

    void refresh_entries() {
        entries_.clear();
        entry_index_.clear();
        const mz_uint count = mz_zip_reader_get_num_files(&archive_);
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&archive_, i, &stat)) {
                throw std::runtime_error("invalid zip central directory");
            }
            ArchiveEntryInfo info{};
            const auto normalized =
                normalize_archive_entry_path(stat.m_filename, stat.m_is_directory != 0);
            if (!normalized) {
                throw std::runtime_error("zip entry has unsafe path");
            }
            info.name = *normalized;
            info.size = stat.m_uncomp_size;
            info.compressed_size = stat.m_comp_size;
            info.is_directory = stat.m_is_directory != 0;
            info.is_file = !info.is_directory;
            info.modified_unix = static_cast<std::int64_t>(stat.m_time);
            info.mode = stat.m_external_attr >> 16;
            entry_index_[info.name] = entries_.size();
            entries_.push_back(std::move(info));
        }
        check_extract_limits(limits_, entries_.size(), 0, 0);
    }

    int find_entry_index(const std::string& name) const {
        const auto normalized = normalize_archive_entry_path(name, false);
        if (!normalized) {
            return -1;
        }
        std::string lookup = *normalized;
        auto it = entry_index_.find(lookup);
        if (it != entry_index_.end()) {
            return static_cast<int>(it->second);
        }
        if (lookup.back() != '/') {
            lookup.push_back('/');
            it = entry_index_.find(lookup);
            if (it != entry_index_.end()) {
                return static_cast<int>(it->second);
            }
        }
        return -1;
    }

    void write_directory_entry(const std::string& name) {
        const mz_uint level = static_cast<mz_uint>(std::clamp(compression_level_, 0, 10));
        if (!mz_zip_writer_add_mem(&archive_, name.c_str(), nullptr, 0, level)) {
            throw std::runtime_error("failed to write zip directory entry");
        }
        upsert_entry(name, 0, 0, false, true);
    }

    void upsert_entry(const std::string& name, std::uint64_t size, std::uint64_t compressed, bool is_file,
                      bool is_directory) {
        ArchiveEntryInfo info{};
        info.name = name;
        info.size = size;
        info.compressed_size = compressed;
        info.is_file = is_file;
        info.is_directory = is_directory;
        auto it = entry_index_.find(name);
        if (it != entry_index_.end()) {
            entries_[it->second] = info;
        } else {
            entry_index_[name] = entries_.size();
            entries_.push_back(std::move(info));
        }
    }

    std::string path_;
    ArchiveMode mode_;
    int compression_level_ = 6;
    mz_zip_archive archive_{};
    bool closed_ = false;
    bool writing_ = false;
    ArchiveLimits limits_{};
    std::vector<ArchiveEntryInfo> entries_;
    std::unordered_map<std::string, std::size_t> entry_index_;
};

}  // namespace

std::unique_ptr<ArchiveBackend> make_zip_backend(const std::string& path, ArchiveMode mode, int compression_level,
                                                 const ArchiveLimits& limits) {
    return std::make_unique<ZipArchiveBackend>(path, mode, compression_level, limits);
}

}  // namespace pscript::archive
