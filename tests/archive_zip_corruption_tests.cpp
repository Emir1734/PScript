#include "stdlib/archive/archive_types.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

constexpr std::uint64_t kZip64TriggerBytes = (static_cast<std::uint64_t>(1) << 32);
constexpr std::uint64_t kZip64DiskBudgetBytes = 5ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr int kZip64CompressionLevel = 9;
constexpr std::size_t kZipCdhCrc32Ofs = 16;

int g_failed = 0;

void fail_test(const char* name, const std::string& detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(const char* name) {
    std::cout << "PASS  " << name << '\n';
}

std::filesystem::path archive_zip_work_root() {
#if defined(_WIN32)
    return std::filesystem::path(std::getenv("LOCALAPPDATA") ? std::getenv("LOCALAPPDATA") : "C:/Windows/Temp") /
           "pscript-archive-zip-work";
#else
    return std::filesystem::temp_directory_path() / "pscript-archive-zip-work";
#endif
}

std::filesystem::path make_unique_work_dir(const char* label) {
    static int counter = 0;
#if defined(_WIN32)
    const std::string unique =
        std::string(label) + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++counter);
#else
    const std::string unique = std::string(label) + "-" + std::to_string(++counter);
#endif
    const std::filesystem::path root = archive_zip_work_root();
    std::filesystem::create_directories(root);
    const std::filesystem::path dir = root / unique;
    std::filesystem::create_directories(dir);
    return dir;
}

class ScopedWorkDir {
public:
    explicit ScopedWorkDir(const char* label) : path_(make_unique_work_dir(label)) {}
    ~ScopedWorkDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScopedWorkDir(const ScopedWorkDir&) = delete;
    ScopedWorkDir& operator=(const ScopedWorkDir&) = delete;
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

bool test_work_dir_lifecycle() {
    std::filesystem::path dir_path;
    {
        ScopedWorkDir dir("lifecycle-check");
        dir_path = dir.path();
        const auto marker = dir_path / "marker.txt";
        std::ofstream out(marker, std::ios::binary | std::ios::trunc);
        if (!out) {
            fail_test("zip workdir lifecycle", "could not create marker file");
            return false;
        }
        out << "x";
    }
    if (std::filesystem::exists(dir_path)) {
        fail_test("zip workdir lifecycle", "work directory still exists after scope exit");
        return false;
    }
    pass_test("zip workdir lifecycle");
    return true;
}

std::vector<char> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_file_bytes(const std::filesystem::path& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to write test zip bytes");
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out) {
        throw std::runtime_error("failed to flush test zip bytes");
    }
}

std::uint32_t read_le32(const std::vector<char>& bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

std::optional<std::size_t> central_dir_first_entry_offset(const std::vector<char>& bytes) {
    if (bytes.size() < 22) {
        return std::nullopt;
    }
    const std::size_t scan = bytes.size() < (128 * 1024) ? bytes.size() : static_cast<std::size_t>(128 * 1024);
    const std::size_t scan_start = bytes.size() - scan;
    std::optional<std::size_t> eocd;
    for (std::size_t i = scan_start; i + 4 <= bytes.size(); ++i) {
        if (static_cast<unsigned char>(bytes[i]) == 0x50 && static_cast<unsigned char>(bytes[i + 1]) == 0x4b &&
            static_cast<unsigned char>(bytes[i + 2]) == 0x05 && static_cast<unsigned char>(bytes[i + 3]) == 0x06) {
            eocd = i;
        }
    }
    if (!eocd || *eocd + 22 > bytes.size()) {
        return std::nullopt;
    }
    const std::size_t cd_ofs = read_le32(bytes, *eocd + 16);
    if (cd_ofs + 4 > bytes.size()) {
        return std::nullopt;
    }
    if (static_cast<unsigned char>(bytes[cd_ofs]) != 0x50 || static_cast<unsigned char>(bytes[cd_ofs + 1]) != 0x4b ||
        static_cast<unsigned char>(bytes[cd_ofs + 2]) != 0x01 || static_cast<unsigned char>(bytes[cd_ofs + 3]) != 0x02) {
        return std::nullopt;
    }
    return cd_ofs;
}

std::size_t local_header_payload_offset(const std::vector<char>& bytes, std::size_t header_off) {
    const std::uint16_t name_len = static_cast<std::uint8_t>(bytes[header_off + 26]) |
                                   (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[header_off + 27])) << 8);
    const std::uint16_t extra_len = static_cast<std::uint8_t>(bytes[header_off + 28]) |
                                    (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[header_off + 29])) << 8);
    return header_off + 30 + name_len + extra_len;
}

void create_stored_zip(const std::filesystem::path& path) {
    using namespace pscript::archive;
    auto w = open_archive_backend(path.string(), ArchiveFormat::Zip, ArchiveMode::Write, 0, ArchiveLimits{});
    w->write_bytes("data.txt", std::vector<std::uint8_t>(512, static_cast<std::uint8_t>('d')));
    w->finalize();
}

bool expect_zip_error(const char* name, const std::function<void()>& fn) {
    try {
        fn();
        fail_test(name, "expected exception");
        return false;
    } catch (const std::exception& ex) {
        if (std::string(ex.what()).find("zip") == std::string::npos) {
            fail_test(name, std::string("unexpected: ") + ex.what());
            return false;
        }
        pass_test(name);
        return true;
    }
}

bool tail_has_zip64_end_record(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const auto file_size = static_cast<std::uint64_t>(in.tellg());
    const std::uint64_t scan = (file_size < 128 * 1024) ? file_size : static_cast<std::uint64_t>(128 * 1024);
    in.seekg(static_cast<std::streamoff>(file_size - scan), std::ios::beg);
    std::vector<char> tail(scan);
    in.read(tail.data(), static_cast<std::streamsize>(tail.size()));
    const std::size_t n = static_cast<std::size_t>(in.gcount());
    for (std::size_t i = 0; i + 4 <= n; ++i) {
        if (static_cast<unsigned char>(tail[i]) == 0x50 && static_cast<unsigned char>(tail[i + 1]) == 0x4b &&
            static_cast<unsigned char>(tail[i + 2]) == 0x06 && static_cast<unsigned char>(tail[i + 3]) == 0x06) {
            return true;
        }
    }
    return false;
}

std::uint64_t dir_total_logical_bytes(const std::filesystem::path& root) {
    std::uint64_t total = 0;
    if (!std::filesystem::exists(root)) {
        return 0;
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::error_code ec;
        const auto size = entry.file_size(ec);
        if (!ec) {
            total += static_cast<std::uint64_t>(size);
        }
    }
    return total;
}

void update_peak_logical_bytes(const std::filesystem::path& root, std::uint64_t& peak) {
    const std::uint64_t total = dir_total_logical_bytes(root);
    if (total > peak) {
        peak = total;
    }
}

bool verify_large_zero_file(const std::filesystem::path& path, std::uint64_t expected_size) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size != expected_size) {
        return false;
    }
#if defined(_WIN32)
    const std::wstring native = path.native();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    auto read_byte = [&](std::uint64_t offset, unsigned char& out) -> bool {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(handle, pos, nullptr, FILE_BEGIN) == 0) {
            return false;
        }
        DWORD read = 0;
        if (!ReadFile(handle, &out, 1, &read, nullptr) || read != 1) {
            return false;
        }
        return true;
    };
    unsigned char first = 0;
    unsigned char last = 0;
    const bool ok = read_byte(0, first) && read_byte(expected_size - 1, last) && first == 0 && last == 0;
    CloseHandle(handle);
    return ok;
#else
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char first = 0;
    in.read(&first, 1);
    if (!in || first != 0) {
        return false;
    }
    in.seekg(static_cast<std::streamoff>(expected_size - 1), std::ios::beg);
    char last = 0;
    in.read(&last, 1);
    return in && last == 0;
#endif
}

bool create_sparse_file(const std::filesystem::path& path, std::uint64_t size) {
#if defined(_WIN32)
    const std::wstring native = path.native();
    HANDLE handle =
        CreateFileW(native.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(size);
    const bool ok = SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) != 0 && SetEndOfFile(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    if (ftruncate(fd, static_cast<off_t>(size)) != 0) {
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
#endif
}

void test_zip_crc_corruption(const std::filesystem::path& base) {
    using namespace pscript::archive;
    const auto zip_path = base / "crc.zip";
    create_stored_zip(zip_path);
    auto bytes = read_file_bytes(zip_path);
    const auto central = central_dir_first_entry_offset(bytes);
    if (!central) {
        fail_test("zip crc corruption setup", "central directory header missing");
        return;
    }
    const std::uint32_t crc_before = read_le32(bytes, *central + kZipCdhCrc32Ofs);
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[*central + kZipCdhCrc32Ofs + i] = static_cast<char>(~bytes[*central + kZipCdhCrc32Ofs + i]);
    }
    if (read_le32(bytes, *central + kZipCdhCrc32Ofs) == crc_before) {
        fail_test("zip crc corruption setup", "crc not modified");
        return;
    }
    write_file_bytes(zip_path, bytes);
    const auto on_disk = read_file_bytes(zip_path);
    if (on_disk.size() != bytes.size() ||
        read_le32(on_disk, *central + kZipCdhCrc32Ofs) != read_le32(bytes, *central + kZipCdhCrc32Ofs)) {
        fail_test("zip crc corruption setup", "corrupted bytes not persisted");
        return;
    }
    expect_zip_error("zip crc corruption", [&] {
        auto r = open_archive_backend(zip_path.string(), ArchiveFormat::Zip, ArchiveMode::Read, 0, ArchiveLimits{});
        (void)r->read_bytes("data.txt");
    });
}

void test_zip_payload_corruption(const std::filesystem::path& base) {
    using namespace pscript::archive;
    const auto zip_path = base / "payload.zip";
    create_stored_zip(zip_path);
    auto bytes = read_file_bytes(zip_path);
    const std::size_t payload = local_header_payload_offset(bytes, 0);
    if (payload >= bytes.size()) {
        fail_test("zip payload corruption setup", "payload offset invalid");
        return;
    }
    bytes[payload] = static_cast<char>(~bytes[payload]);
    write_file_bytes(zip_path, bytes);
    expect_zip_error("zip payload corruption", [&] {
        auto r = open_archive_backend(zip_path.string(), ArchiveFormat::Zip, ArchiveMode::Read, 0, ArchiveLimits{});
        (void)r->read_bytes("data.txt");
    });
}

void test_zip_truncated_valid(const std::filesystem::path& base) {
    using namespace pscript::archive;
    const auto zip_path = base / "trunc-valid.zip";
    create_stored_zip(zip_path);
    auto bytes = read_file_bytes(zip_path);
    if (bytes.size() < 64) {
        fail_test("zip truncated valid setup", "archive too small");
        return;
    }
    bytes.resize(bytes.size() - 48);
    write_file_bytes(zip_path, bytes);
    expect_zip_error("zip truncated valid archive", [&] {
        (void)open_archive_backend(zip_path.string(), ArchiveFormat::Zip, ArchiveMode::Read, 0, ArchiveLimits{});
    });
}

void test_zip64_roundtrip(const std::filesystem::path& base, std::uint64_t& peak_logical_bytes) {
    using namespace pscript::archive;
    const auto zip64_base = base / "zip64";
    std::filesystem::create_directories(zip64_base);
    const auto sparse_path = zip64_base / "large.bin";
    if (!create_sparse_file(sparse_path, kZip64TriggerBytes)) {
        fail_test("zip64 sparse file", "could not create sparse 4GiB source file");
        return;
    }
    update_peak_logical_bytes(base, peak_logical_bytes);
    const std::string zip_path = (zip64_base / "large.zip").string();
    ArchiveLimits limits{};
    limits.max_single_file_bytes = kZip64TriggerBytes + 4096;
    limits.max_total_extract_bytes = kZip64TriggerBytes + 4096;
    try {
        auto writer = open_archive_backend(zip_path, ArchiveFormat::Zip, ArchiveMode::Write, kZip64CompressionLevel,
                                          limits);
        writer->add_path(sparse_path.string(), "large.bin");
        writer->finalize();
        writer.reset();
        update_peak_logical_bytes(base, peak_logical_bytes);
        std::error_code remove_sparse_ec;
        std::filesystem::remove(sparse_path, remove_sparse_ec);
        update_peak_logical_bytes(base, peak_logical_bytes);
        if (!tail_has_zip64_end_record(zip_path)) {
            fail_test("zip64 eocd present", "zip64 end record not found");
            return;
        }
        pass_test("zip64 eocd present");
        auto reader = open_archive_backend(zip_path, ArchiveFormat::Zip, ArchiveMode::Read, 0, limits);
        const ArchiveEntryInfo* large_entry = nullptr;
        for (const auto& entry : reader->entries()) {
            if (entry.name == "large.bin") {
                large_entry = &entry;
            }
        }
        if (large_entry == nullptr || large_entry->size != kZip64TriggerBytes) {
            fail_test("zip64 list", "large.bin missing or wrong size");
            return;
        }
        pass_test("zip64 list");
        reader->extract_entry("large.bin", zip64_base.string());
        update_peak_logical_bytes(base, peak_logical_bytes);
        const auto extracted = zip64_base / "large.bin";
        if (!verify_large_zero_file(extracted, kZip64TriggerBytes)) {
            fail_test("zip64 extract", "extracted size or zero-byte sample mismatch");
            return;
        }
        pass_test("zip64 extract");
    } catch (const std::exception& ex) {
        fail_test("zip64 roundtrip", ex.what());
    }
}

}  // namespace

int run_archive_zip_corruption_tests() {
    g_failed = 0;
    if (!test_work_dir_lifecycle()) {
        ++g_failed;
    }
    ScopedWorkDir base("corruption");
    test_zip_crc_corruption(base.path());
    test_zip_payload_corruption(base.path());
    test_zip_truncated_valid(base.path());
    if (g_failed != 0) {
        std::cerr << g_failed << " archive zip corruption test(s) failed\n";
        return 1;
    }
    return 0;
}

int run_archive_zip64_tests() {
    g_failed = 0;
    std::uint64_t peak_logical_bytes = 0;
    ScopedWorkDir base("zip64-suite");
    test_zip64_roundtrip(base.path(), peak_logical_bytes);
    update_peak_logical_bytes(base.path(), peak_logical_bytes);
    if (peak_logical_bytes >= kZip64DiskBudgetBytes) {
        fail_test("zip64 disk budget",
                  "peak logical bytes in work dir exceeded 5GiB: " + std::to_string(peak_logical_bytes));
    } else {
        pass_test("zip64 disk budget");
    }
    if (g_failed != 0) {
        std::cerr << g_failed << " archive zip64 test(s) failed\n";
        return 1;
    }
    return 0;
}

void cleanup_archive_zip_work_root_if_empty() {
    const std::filesystem::path root = archive_zip_work_root();
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || ec) {
        return;
    }
    if (std::filesystem::is_empty(root, ec) && !ec) {
        std::filesystem::remove(root, ec);
    }
}
