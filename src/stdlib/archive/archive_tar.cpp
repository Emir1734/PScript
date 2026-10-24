#include "stdlib/archive/archive_tar.h"

#include "stdlib/archive/archive_gzip.h"
#include "stdlib/archive/archive_security.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace pscript::archive {
namespace {

constexpr std::size_t kTarBlock = 512;

struct TarStoredEntry {
    ArchiveEntryInfo info;
    std::uint64_t data_offset = 0;
    std::string symlink_target;
};

std::uint64_t parse_octal_field(const char* field, std::size_t length) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(field[i]);
        if (ch == '\0' || ch == ' ') {
            continue;
        }
        if (ch < '0' || ch > '7') {
            break;
        }
        value = (value << 3) + static_cast<std::uint64_t>(ch - '0');
    }
    return value;
}

void write_octal_field(char* field, std::size_t length, std::uint64_t value) {
    std::snprintf(field, length, "%0*lo", static_cast<int>(length - 1), static_cast<unsigned long>(value));
    field[length - 1] = '\0';
}

void compute_tar_checksum(std::array<char, kTarBlock>& block) {
    std::memset(block.data() + 148, ' ', 8);
    unsigned int sum = 0;
    for (const unsigned char ch : block) {
        sum += ch;
    }
    std::snprintf(block.data() + 148, 9, "%06o", sum);
    block[155] = '\0';
}

bool is_zero_block(const std::array<char, kTarBlock>& block) {
    for (char ch : block) {
        if (ch != '\0') {
            return false;
        }
    }
    return true;
}

std::string trim_tar_string(const char* data, std::size_t max_len) {
    std::size_t len = 0;
    while (len < max_len && data[len] != '\0') {
        ++len;
    }
    return std::string(data, len);
}

std::string parse_pax_payload(const std::string& payload) {
    std::unordered_map<std::string, std::string> fields;
    std::size_t pos = 0;
    while (pos < payload.size()) {
        const std::size_t space = payload.find(' ', pos);
        if (space == std::string::npos) {
            break;
        }
        const std::size_t record_len = static_cast<std::size_t>(std::stoull(payload.substr(pos, space - pos)));
        if (record_len == 0 || pos + record_len > payload.size()) {
            break;
        }
        const std::size_t eq = payload.find('=', space + 1);
        if (eq == std::string::npos || eq >= pos + record_len) {
            break;
        }
        const std::string key = payload.substr(space + 1, eq - space - 1);
        const std::string value = payload.substr(eq + 1, pos + record_len - eq - 2);
        fields[key] = value;
        pos += record_len;
    }
    if (fields.count("path")) {
        return fields["path"];
    }
    return {};
}

std::uint64_t pax_size(const std::unordered_map<std::string, std::string>& fields, std::uint64_t fallback) {
    if (fields.count("size")) {
        return static_cast<std::uint64_t>(std::stoull(fields.at("size")));
    }
    return fallback;
}

std::uint64_t round_up_blocks(std::uint64_t size) {
    return ((size + kTarBlock - 1) / kTarBlock) * kTarBlock;
}

class TarWriterSink {
public:
    virtual ~TarWriterSink() = default;
    virtual void write(const char* data, std::size_t size) = 0;
    virtual std::uint64_t position() const = 0;
};

class FileTarWriterSink final : public TarWriterSink {
public:
    explicit FileTarWriterSink(const std::filesystem::path& path)
        : stream_(path, std::ios::binary | std::ios::trunc) {
        if (!stream_) {
            throw std::runtime_error("failed to open tar file for writing");
        }
    }
    void write(const char* data, std::size_t size) override {
        stream_.write(data, static_cast<std::streamsize>(size));
        if (!stream_) {
            throw std::runtime_error("tar write failed");
        }
        position_ += size;
    }
    std::uint64_t position() const override { return position_; }

private:
    std::ofstream stream_;
    std::uint64_t position_ = 0;
};

class CallbackTarWriterSink final : public TarWriterSink {
public:
    explicit CallbackTarWriterSink(std::function<void(const std::uint8_t*, std::size_t)> callback)
        : callback_(std::move(callback)) {}
    void write(const char* data, std::size_t size) override {
        callback_(reinterpret_cast<const std::uint8_t*>(data), size);
        position_ += size;
    }
    std::uint64_t position() const override { return position_; }

private:
    std::function<void(const std::uint8_t*, std::size_t)> callback_;
    std::uint64_t position_ = 0;
};

class TarStreamWriter {
public:
    explicit TarStreamWriter(TarWriterSink& sink) : sink_(sink) {}

    void write_entry(const std::string& name, const std::vector<std::uint8_t>& data, char typeflag, std::uint32_t mode,
                     bool is_directory, const std::string& link_target = {}) {
        const auto normalized = normalize_archive_entry_path(name, is_directory);
        if (!normalized) {
            throw std::runtime_error("invalid tar entry path");
        }
        std::string entry_name = *normalized;
        std::string header_name = entry_name;
        if (entry_name.size() > 99) {
            write_gnu_long_name(entry_name);
            header_name = entry_name.substr(0, 99);
        }
        std::unordered_map<std::string, std::string> pax;
        if (data.size() > 0x1FFFFFFFFULL) {
            pax["size"] = std::to_string(data.size());
            write_pax_header(pax);
        }
        write_ustar_header(header_name, data.size(), typeflag, mode, link_target);
        if (!data.empty()) {
            sink_.write(reinterpret_cast<const char*>(data.data()), data.size());
            write_padding(data.size());
        }
    }

    void write_directory(const std::string& name, std::uint32_t mode) {
        write_entry(name, {}, '5', mode, true);
    }

    void write_symlink(const std::string& name, const std::string& target, std::uint32_t mode) {
        if (target.size() > 99) {
            write_gnu_long_link(target);
        }
        write_entry(name, {}, '2', mode, false, target.size() > 99 ? target.substr(0, 99) : target);
    }

    void write_gnu_long_link(const std::string& target) {
        write_ustar_header("././@LongLink", target.size(), 'K', 0644, std::string{});
        if (!target.empty()) {
            sink_.write(target.data(), target.size());
            write_padding(target.size());
        }
    }

    void write_gnu_long_name(const std::string& name) {
        write_ustar_header("././@LongLink", name.size(), 'L', 0644, std::string{});
        if (!name.empty()) {
            sink_.write(name.data(), name.size());
            write_padding(name.size());
        }
    }

    void finish() {
        std::array<char, kTarBlock> zero{};
        sink_.write(zero.data(), zero.size());
        sink_.write(zero.data(), zero.size());
    }

    std::uint64_t position() const { return sink_.position(); }

private:
    void write_pax_header(const std::unordered_map<std::string, std::string>& fields) {
        std::ostringstream payload;
        for (const auto& [key, value] : fields) {
            const std::string record = key + "=" + value + "\n";
            std::size_t len = record.size() + 3;
            std::string line;
            for (int guard = 0; guard < 32; ++guard) {
                const std::string prefix = std::to_string(len) + " ";
                line = prefix + record;
                if (line.size() == len) {
                    break;
                }
                len = line.size();
            }
            if (line.size() != len) {
                throw std::runtime_error("invalid pax record length");
            }
            payload << line;
        }
        const std::string body = payload.str();
        write_ustar_header("././@PaxHeader", body.size(), 'x', 0644, std::string{});
        if (!body.empty()) {
            sink_.write(body.data(), body.size());
            write_padding(body.size());
        }
    }

    void write_ustar_header(const std::string& name, std::uint64_t size, char typeflag, std::uint32_t mode,
                            const std::string& link_target) {
        std::array<char, kTarBlock> block{};
        const std::string short_name = name.size() > 99 ? name.substr(0, 99) : name;
        std::memcpy(block.data(), short_name.c_str(), std::min(short_name.size(), std::size_t{99}));
        write_octal_field(block.data() + 100, 8, mode);
        write_octal_field(block.data() + 108, 8, 0);
        write_octal_field(block.data() + 116, 8, 0);
        write_octal_field(block.data() + 124, 12, size);
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        write_octal_field(block.data() + 136, 12, static_cast<std::uint64_t>(now));
        block[156] = typeflag;
        if (!link_target.empty()) {
            std::memcpy(block.data() + 157, link_target.c_str(), std::min(link_target.size(), std::size_t{99}));
        }
        std::memcpy(block.data() + 257, "ustar", 5);
        block[262] = '0';
        compute_tar_checksum(block);
        sink_.write(block.data(), block.size());
    }

    void write_padding(std::uint64_t size) {
        const std::uint64_t padding = round_up_blocks(size) - size;
        if (padding == 0) {
            return;
        }
        std::vector<char> pad(padding, '\0');
        sink_.write(pad.data(), pad.size());
    }

    TarWriterSink& sink_;
};

class TarArchiveBackend final : public ArchiveBackend {
public:
    TarArchiveBackend(const std::string& path, ArchiveMode mode, std::unique_ptr<TarWriterSink> writer_sink,
                      ArchiveLimits limits = {})
        : path_(path), mode_(mode), writer_sink_(std::move(writer_sink)), limits_(limits) {
        if (mode_ == ArchiveMode::Read) {
            load_index_from_file();
        } else if (writer_sink_) {
            writer_ = std::make_unique<TarStreamWriter>(*writer_sink_);
        } else {
            file_writer_ = std::make_unique<FileTarWriterSink>(path_);
            writer_ = std::make_unique<TarStreamWriter>(*file_writer_);
        }
    }

    void close() override {
        if (closed_) {
            return;
        }
        if (writer_) {
            writer_->finish();
            writer_.reset();
        }
        if (gzip_writer_) {
            gzip_writer_->finish();
            gzip_writer_.reset();
        }
        closed_ = true;
    }

    bool closed() const override { return closed_; }

    const std::vector<ArchiveEntryInfo>& entries() const override { return entries_; }

    std::vector<std::uint8_t> read_bytes(const std::string& name) override {
        const TarStoredEntry* entry = find_entry(name);
        if (entry == nullptr) {
            throw std::runtime_error("archive entry not found");
        }
        if (entry->info.is_directory) {
            return {};
        }
        if (entry->info.is_symlink) {
            return std::vector<std::uint8_t>(entry->symlink_target.begin(), entry->symlink_target.end());
        }
        check_extract_limits(limits_, entries_.size(), entry->info.size, entry->info.size);
        return read_file_bytes(entry->data_offset, entry->info.size);
    }

    void write_bytes(const std::string& name, const std::vector<std::uint8_t>& data) override {
        ensure_writer();
        check_extract_limits(limits_, entries_.size() + 1, 0, data.size());
        writer_->write_entry(name, data, '0', 0644, false);
        add_or_replace_entry(name, data.size(), true, false, 0644, writer_->position());
    }

    void add_path(const std::string& filesystem_path, const std::string& archive_name) override {
        ensure_writer();
        const std::filesystem::path source(filesystem_path);
        if (!std::filesystem::exists(source)) {
            throw std::runtime_error("path does not exist");
        }
        if (std::filesystem::is_regular_file(source)) {
            std::ifstream in(source, std::ios::binary);
            std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const std::string entry_name =
                archive_name.empty() ? source.filename().generic_string() : archive_name;
            write_bytes(entry_name, data);
            return;
        }
        const std::filesystem::path base = archive_name.empty() ? source.filename() : std::filesystem::path(archive_name);
        if (!base.empty()) {
            const auto dir_name = normalize_archive_entry_path(base.generic_string(), true);
            if (dir_name) {
                writer_->write_directory(*dir_name, 0755);
                add_or_replace_entry(*dir_name, 0, false, true, 0755, writer_->position());
            }
        }
        for (const auto& item : std::filesystem::recursive_directory_iterator(source)) {
            const std::filesystem::path rel = std::filesystem::relative(item.path(), source);
            const std::string rel_name = (base.empty() ? rel : base / rel).generic_string();
            if (item.is_directory()) {
                const auto dir_name = normalize_archive_entry_path(rel_name, true);
                if (dir_name) {
                    writer_->write_directory(*dir_name, 0755);
                    add_or_replace_entry(*dir_name, 0, false, true, 0755, writer_->position());
                }
                continue;
            }
            if (!item.is_regular_file()) {
                continue;
            }
            std::ifstream in(item.path(), std::ios::binary);
            std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            const auto file_name = normalize_archive_entry_path(rel_name, false);
            if (!file_name) {
                throw std::runtime_error("invalid tar entry path");
            }
            writer_->write_entry(*file_name, data, '0', 0644, false);
            add_or_replace_entry(*file_name, data.size(), true, false, 0644, writer_->position());
        }
    }

    void extract_entry(const std::string& name, const std::string& destination_dir) override {
        const TarStoredEntry* entry = find_entry(name);
        if (entry == nullptr) {
            throw std::runtime_error("archive entry not found");
        }
        extract_stored_entry(*entry, destination_dir);
    }

    void extract_all(const std::string& destination_dir) override {
        std::uint64_t total = 0;
        for (const auto& stored : stored_entries_) {
            total += stored.info.size;
            check_extract_limits(limits_, stored_entries_.size(), total, stored.info.size);
            extract_stored_entry(stored, destination_dir);
        }
    }

    void finalize() override { close(); }

    void set_memory_tar(std::vector<std::uint8_t> data) {
        memory_tar_ = std::move(data);
        std::stringstream stream;
        stream.write(reinterpret_cast<const char*>(memory_tar_.data()),
                     static_cast<std::streamsize>(memory_tar_.size()));
        load_index_from_stream(stream);
    }

    void load_index_from_stream(std::istream& in) {
        stored_entries_.clear();
        entries_.clear();
        entry_map_.clear();
        std::uint64_t offset = 0;
        std::string pending_pax;
        std::string pending_gnu_name;
        std::string pending_gnu_link;
        while (in) {
            std::array<char, kTarBlock> block{};
            in.read(block.data(), static_cast<std::streamsize>(block.size()));
            if (in.gcount() == 0) {
                break;
            }
            if (in.gcount() != static_cast<std::streamsize>(block.size()) || is_zero_block(block)) {
                break;
            }
            offset += kTarBlock;
            const char typeflag = block[156] == '\0' ? '0' : block[156];
            std::string name = trim_tar_string(block.data(), 100);
            std::uint64_t size = parse_octal_field(block.data() + 124, 12);
            std::uint32_t mode = static_cast<std::uint32_t>(parse_octal_field(block.data() + 100, 8));
            const std::int64_t mtime = static_cast<std::int64_t>(parse_octal_field(block.data() + 136, 12));
            std::string link = trim_tar_string(block.data() + 157, 100);
            if (typeflag == 'x' || typeflag == 'g') {
                std::vector<char> payload(size);
                in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
                const std::uint64_t padded = round_up_blocks(size);
                if (padded > size) {
                    in.seekg(static_cast<std::streamoff>(padded - size), std::ios::cur);
                }
                offset += padded;
                pending_pax = std::string(payload.begin(), payload.end());
                continue;
            }
            if (typeflag == 'L') {
                std::vector<char> payload(size);
                in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
                const std::uint64_t padded = round_up_blocks(size);
                if (padded > size) {
                    in.seekg(static_cast<std::streamoff>(padded - size), std::ios::cur);
                }
                offset += padded;
                pending_gnu_name.assign(payload.begin(), payload.end());
                continue;
            }
            if (typeflag == 'K') {
                std::vector<char> payload(size);
                in.read(payload.data(), static_cast<std::streamsize>(payload.size()));
                const std::uint64_t padded = round_up_blocks(size);
                if (padded > size) {
                    in.seekg(static_cast<std::streamoff>(padded - size), std::ios::cur);
                }
                offset += padded;
                pending_gnu_link.assign(payload.begin(), payload.end());
                continue;
            }
            if (!pending_pax.empty()) {
                const std::string pax_path = parse_pax_payload(pending_pax);
                if (!pax_path.empty()) {
                    name = pax_path;
                }
                std::unordered_map<std::string, std::string> fields;
                std::size_t pos = 0;
                while (pos < pending_pax.size()) {
                    const std::size_t space = pending_pax.find(' ', pos);
                    if (space == std::string::npos) {
                        break;
                    }
                    const std::size_t record_len = static_cast<std::size_t>(std::stoull(pending_pax.substr(pos, space - pos)));
                    const std::size_t eq = pending_pax.find('=', space + 1);
                    if (eq != std::string::npos) {
                        fields[pending_pax.substr(space + 1, eq - space - 1)] =
                            pending_pax.substr(eq + 1, pos + record_len - eq - 2);
                    }
                    pos += record_len;
                }
                size = pax_size(fields, size);
                pending_pax.clear();
            }
            if (!pending_gnu_name.empty()) {
                name = pending_gnu_name;
                pending_gnu_name.clear();
            }
            if (!pending_gnu_link.empty()) {
                link = pending_gnu_link;
                pending_gnu_link.clear();
            }
            const bool is_directory = typeflag == '5' || (!name.empty() && name.back() == '/');
            const auto normalized = normalize_archive_entry_path(name, is_directory);
            if (!normalized) {
                throw std::runtime_error("tar entry has unsafe path");
            }
            TarStoredEntry stored{};
            stored.info.name = *normalized;
            stored.info.size = size;
            stored.info.compressed_size = size;
            stored.info.is_directory = is_directory;
            stored.info.is_file = typeflag == '0' || typeflag == '\0';
            stored.info.is_symlink = typeflag == '2';
            stored.info.tar_typeflag = typeflag;
            stored.info.modified_unix = mtime;
            stored.info.mode = mode;
            stored.data_offset = offset;
            if (stored.info.is_symlink) {
                stored.symlink_target = link;
                stored.info.symlink_target = link;
            }
            if (typeflag == '3' || typeflag == '4' || typeflag == '6' || typeflag == '7') {
                stored.info.is_file = false;
            }
            const std::uint64_t padded = round_up_blocks(size);
            if (size > 0) {
                in.seekg(static_cast<std::streamoff>(padded), std::ios::cur);
            }
            offset += padded;
            entry_map_[stored.info.name] = stored_entries_.size();
            stored_entries_.push_back(stored);
            entries_.push_back(stored.info);
        }
        check_extract_limits(limits_, entries_.size(), 0, 0);
    }

private:
    void load_index_from_file() {
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to open tar archive");
        }
        load_index_from_stream(in);
    }

    std::vector<std::uint8_t> read_file_bytes(std::uint64_t offset, std::uint64_t size) const {
        if (!memory_tar_.empty()) {
            if (offset + size > memory_tar_.size()) {
                throw std::runtime_error("truncated tar entry");
            }
            return std::vector<std::uint8_t>(memory_tar_.begin() + static_cast<std::ptrdiff_t>(offset),
                                             memory_tar_.begin() + static_cast<std::ptrdiff_t>(offset + size));
        }
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
            throw std::runtime_error("failed to read tar entry");
        }
        in.seekg(static_cast<std::streamoff>(offset));
        std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!in) {
            throw std::runtime_error("truncated tar entry");
        }
        return data;
    }

    const TarStoredEntry* find_entry(const std::string& name) const {
        const auto normalized = normalize_archive_entry_path(name, false);
        if (!normalized) {
            return nullptr;
        }
        auto it = entry_map_.find(*normalized);
        if (it != entry_map_.end()) {
            return &stored_entries_[it->second];
        }
        std::string dir = *normalized;
        if (dir.back() != '/') {
            dir.push_back('/');
        }
        it = entry_map_.find(dir);
        if (it != entry_map_.end()) {
            return &stored_entries_[it->second];
        }
        return nullptr;
    }

    void extract_stored_entry(const TarStoredEntry& entry, const std::string& destination_dir) {
        const std::filesystem::path dest_root(destination_dir);
        const std::filesystem::path out_path = secure_extract_path(dest_root, entry.info.name);
        if (entry.info.is_directory) {
            std::filesystem::create_directories(out_path);
            return;
        }
        if (entry.info.is_symlink) {
            if (!is_safe_symlink_target(entry.symlink_target, dest_root)) {
                throw std::runtime_error("unsafe symlink target in archive");
            }
            std::filesystem::create_directories(out_path.parent_path());
            std::filesystem::create_symlink(entry.symlink_target, out_path);
            return;
        }
        if (!entry.info.is_file) {
            throw std::runtime_error("unsupported tar entry type for extraction");
        }
        const auto data = read_file_bytes(entry.data_offset, entry.info.size);
        std::filesystem::create_directories(out_path.parent_path());
        std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!out) {
            throw std::runtime_error("failed to write extracted file");
        }
    }

    void ensure_writer() {
        if (mode_ != ArchiveMode::Write || !writer_) {
            throw std::runtime_error("archive is not open for writing");
        }
    }

    void add_or_replace_entry(const std::string& name, std::uint64_t size, bool is_file, bool is_directory,
                              std::uint32_t mode, std::uint64_t data_offset) {
        ArchiveEntryInfo info{};
        const auto normalized = normalize_archive_entry_path(name, is_directory);
        if (!normalized) {
            return;
        }
        info.name = *normalized;
        info.size = size;
        info.compressed_size = size;
        info.is_file = is_file;
        info.is_directory = is_directory;
        info.mode = mode;
        auto it = entry_map_.find(info.name);
        if (it != entry_map_.end()) {
            stored_entries_[it->second].info = info;
            stored_entries_[it->second].data_offset = data_offset;
            entries_[it->second] = info;
        } else {
            TarStoredEntry stored{};
            stored.info = info;
            stored.data_offset = data_offset;
            entry_map_[info.name] = stored_entries_.size();
            stored_entries_.push_back(stored);
            entries_.push_back(info);
        }
    }

    std::string path_;
    ArchiveMode mode_;
    bool closed_ = false;
    ArchiveLimits limits_{};
    std::vector<ArchiveEntryInfo> entries_;
    std::vector<TarStoredEntry> stored_entries_;
    std::unordered_map<std::string, std::size_t> entry_map_;
    std::unique_ptr<FileTarWriterSink> file_writer_;
    std::unique_ptr<TarWriterSink> writer_sink_;
    std::unique_ptr<TarStreamWriter> writer_;
    std::unique_ptr<GzipWriter> gzip_writer_;
    std::vector<std::uint8_t> memory_tar_;
};

class TarGzReadBackend final : public ArchiveBackend {
public:
    explicit TarGzReadBackend(const std::string& path, ArchiveLimits limits) : limits_(limits) {
        static std::atomic<int> spill_counter{0};
        spill_path_ = std::filesystem::temp_directory_path() /
                      ("pscript-targz-" + std::to_string(++spill_counter) + ".tar");
        decompress_gzip_file_to_file(path, spill_path_, limits_);
        inner_ = std::make_unique<TarArchiveBackend>(spill_path_.string(), ArchiveMode::Read, nullptr, limits_);
    }

    void close() override {
        if (inner_) {
            inner_->close();
        }
        std::error_code ec;
        std::filesystem::remove(spill_path_, ec);
        closed_ = true;
    }
    bool closed() const override { return closed_; }
    const std::vector<ArchiveEntryInfo>& entries() const override { return inner_->entries(); }

    std::vector<std::uint8_t> read_bytes(const std::string& name) override { return inner_->read_bytes(name); }

    void write_bytes(const std::string&, const std::vector<std::uint8_t>&) override {
        throw std::runtime_error("archive is not open for writing");
    }

    void add_path(const std::string&, const std::string&) override {
        throw std::runtime_error("archive is not open for writing");
    }

    void extract_entry(const std::string& name, const std::string& destination_dir) override {
        inner_->extract_entry(name, destination_dir);
    }

    void extract_all(const std::string& destination_dir) override { inner_->extract_all(destination_dir); }

    void finalize() override { close(); }

private:
    bool closed_ = false;
    ArchiveLimits limits_{};
    std::filesystem::path spill_path_;
    std::unique_ptr<TarArchiveBackend> inner_;
};

class TarGzWriteBackend final : public ArchiveBackend {
public:
    explicit TarGzWriteBackend(const std::string& path, int level, ArchiveLimits limits)
        : path_(path), gzip_level_(level), limits_(limits) {
        gzip_ = std::make_unique<GzipWriter>(path_, gzip_level_);
        std::unique_ptr<TarWriterSink> sink = std::make_unique<CallbackTarWriterSink>(
            [this](const std::uint8_t* data, std::size_t size) { gzip_->write(data, size); });
        tar_ = std::make_unique<TarArchiveBackend>("", ArchiveMode::Write, std::move(sink), limits_);
    }

    void close() override {
        if (!closed_) {
            tar_->finalize();
            gzip_->finish();
            closed_ = true;
        }
    }
    bool closed() const override { return closed_; }
    const std::vector<ArchiveEntryInfo>& entries() const override { return tar_->entries(); }
    std::vector<std::uint8_t> read_bytes(const std::string& name) override { return tar_->read_bytes(name); }
    void write_bytes(const std::string& name, const std::vector<std::uint8_t>& data) override {
        tar_->write_bytes(name, data);
    }
    void add_path(const std::string& filesystem_path, const std::string& archive_name) override {
        tar_->add_path(filesystem_path, archive_name);
    }
    void extract_entry(const std::string&, const std::string&) override {
        throw std::runtime_error("archive is not open for reading");
    }
    void extract_all(const std::string&) override {
        throw std::runtime_error("archive is not open for reading");
    }
    void finalize() override { close(); }

private:
    std::string path_;
    int gzip_level_ = 6;
    bool closed_ = false;
    ArchiveLimits limits_{};
    std::unique_ptr<GzipWriter> gzip_;
    std::unique_ptr<TarArchiveBackend> tar_;
};

}  // namespace

std::unique_ptr<ArchiveBackend> make_tar_backend(const std::string& path, ArchiveMode mode,
                                                 const ArchiveLimits& limits) {
    return std::make_unique<TarArchiveBackend>(path, mode, nullptr, limits);
}

std::unique_ptr<ArchiveBackend> make_targz_backend(const std::string& path, ArchiveMode mode, int gzip_level,
                                                   const ArchiveLimits& limits) {
    if (mode == ArchiveMode::Read) {
        return std::make_unique<TarGzReadBackend>(path, limits);
    }
    return std::make_unique<TarGzWriteBackend>(path, gzip_level, limits);
}

}  // namespace pscript::archive
