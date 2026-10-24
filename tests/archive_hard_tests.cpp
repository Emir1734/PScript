#include "stdlib/archive/archive_types.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <cstdlib>
#include <vector>

namespace {

int g_hard_failed = 0;

void hard_fail(const char* name, const std::string& detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_hard_failed;
}

void hard_pass(const char* name) {
    std::cout << "PASS  " << name << '\n';
}

std::filesystem::path hard_dir(const char* label) {
    static int counter = 0;
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("pscript-archive-hard-") + label + "-" + std::to_string(++counter));
    std::filesystem::create_directories(dir);
    return dir;
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

void write_tar_header(std::ofstream& out, const std::string& name, char typeflag, std::uint64_t size,
                      const std::string& link = {}) {
    std::array<char, 512> block{};
    const std::size_t name_len = name.size() < 99 ? name.size() : 99;
    std::memcpy(block.data(), name.c_str(), name_len);
    std::snprintf(block.data() + 124, 12, "%011llo", static_cast<unsigned long long>(size));
    block[156] = typeflag;
    const std::size_t link_len = link.size() < 99 ? link.size() : 99;
    std::memcpy(block.data() + 157, link.c_str(), link_len);
    std::memcpy(block.data() + 257, "ustar", 5);
    std::memset(block.data() + 148, ' ', 8);
    unsigned int sum = 0;
    for (unsigned char ch : block) {
        sum += ch;
    }
    std::snprintf(block.data() + 148, 8, "%06o", sum);
    out.write(block.data(), static_cast<std::streamsize>(block.size()));
}

void expect_throw(const char* name, const std::function<void()>& fn, const char* substring) {
    try {
        fn();
        hard_fail(name, "expected exception");
    } catch (const std::exception& ex) {
        if (std::string(ex.what()).find(substring) == std::string::npos) {
            hard_fail(name, std::string("unexpected: ") + ex.what());
            return;
        }
        hard_pass(name);
    }
}

void hard_test_zip_small(const std::filesystem::path& base) {
    using namespace pscript::archive;
    try {
        const std::string small_zip = (base / "small.zip").string();
        auto writer = open_archive_backend(small_zip, ArchiveFormat::Zip, ArchiveMode::Write, 6, ArchiveLimits{});
        writer->write_bytes("entry.bin", std::vector<std::uint8_t>{9, 8, 7, 6});
        writer->finalize();
        writer.reset();
        if (tail_has_zip64_end_record(small_zip)) {
            hard_fail("zip32 format", "unexpected zip64 end record for small archive");
        } else {
            hard_pass("zip32 format");
        }
        auto reader = open_archive_backend(small_zip, ArchiveFormat::Zip, ArchiveMode::Read, 6, ArchiveLimits{});
        const auto data = reader->read_bytes("entry.bin");
        if (data.size() != 4 || data[0] != 9) {
            hard_fail("zip small read", "content mismatch");
        } else {
            hard_pass("zip small read");
        }
    } catch (const std::exception& ex) {
        hard_fail("zip small roundtrip", ex.what());
    }
}

void hard_test_zip_utf8(const std::filesystem::path& base) {
    using namespace pscript::archive;
    try {
        const std::string utf8_name =
            "\xC3\xA7\x61\x6C\xC4\xB1\xC5\x9F\x6D\x61"
            "/\xC5\x9F\x61\x62\x6C\x6F\x6E\x2E\x74\x78\x74";
        const std::string utf8_zip = (base / "utf8.zip").string();
        auto w = open_archive_backend(utf8_zip, ArchiveFormat::Zip, ArchiveMode::Write, 6, ArchiveLimits{});
        const std::vector<std::uint8_t> body = {1, 2, 3, 4};
        w->write_bytes(utf8_name, body);
        w->finalize();
        w.reset();
        auto r = open_archive_backend(utf8_zip, ArchiveFormat::Zip, ArchiveMode::Read, 6, ArchiveLimits{});
        bool found = false;
        for (const auto& e : r->entries()) {
            if (e.name == utf8_name) {
                found = true;
            }
        }
        if (!found) {
            hard_fail("zip utf8 list", "entry name missing");
        } else {
            hard_pass("zip utf8 list");
        }
        const auto read_back = r->read_bytes(utf8_name);
        if (read_back != body) {
            hard_fail("zip utf8 read", "content mismatch");
        } else {
            hard_pass("zip utf8 read");
        }
    } catch (const std::exception& ex) {
        hard_fail("zip utf8", ex.what());
    }
}

void hard_test_zip_truncated(const std::filesystem::path& base) {
    using namespace pscript::archive;
    try {
        const std::string trunc_zip = (base / "trunc.zip").string();
        {
            std::ofstream out(trunc_zip, std::ios::binary);
            out << "PK\x03\x04";
            out << std::string(20, '\0');
        }
        expect_throw("zip truncated", [&] {
            auto r = open_archive_backend(trunc_zip, ArchiveFormat::Zip, ArchiveMode::Read, 6, ArchiveLimits{});
            (void)r->entries();
        }, "zip");
    } catch (...) {
        hard_fail("zip truncated setup", "unexpected");
    }
}

void hard_test_tar_security(const std::filesystem::path& base) {
    using namespace pscript::archive;
    try {
        const std::string tar_path = (base / "evil.tar").string();
        {
            std::ofstream out(tar_path, std::ios::binary);
            write_tar_header(out, "../../outside.txt", '0', 4);
            out.write("evil", 4);
            std::array<char, 512> zero{};
            out.write(zero.data(), static_cast<std::streamsize>(zero.size()));
            out.write(zero.data(), static_cast<std::streamsize>(zero.size()));
        }
        expect_throw("tar traversal open", [&] {
            (void)open_archive_backend(tar_path, ArchiveFormat::Tar, ArchiveMode::Read, 6, ArchiveLimits{});
        }, "unsafe");
    } catch (const std::exception& ex) {
        hard_fail("tar traversal setup", ex.what());
    }

    try {
        const std::string tar_dev = (base / "device.tar").string();
        {
            std::ofstream out(tar_dev, std::ios::binary);
            write_tar_header(out, "dev/node", '4', 0);
            std::array<char, 512> zero{};
            out.write(zero.data(), static_cast<std::streamsize>(zero.size()));
            out.write(zero.data(), static_cast<std::streamsize>(zero.size()));
        }
        auto r = open_archive_backend(tar_dev, ArchiveFormat::Tar, ArchiveMode::Read, 6, ArchiveLimits{});
        expect_throw("tar device extract", [&] { r->extract_entry("dev/node", hard_dir("tar-dev").string()); },
                      "unsupported");
    } catch (const std::exception& ex) {
        hard_fail("tar device setup", ex.what());
    }

    try {
        const std::string tar_sym = (base / "symlink.tar").string();
        {
            std::ofstream out(tar_sym, std::ios::binary);
            write_tar_header(out, "link", '2', 0, "../../../etc/passwd");
            std::array<char, 512> zero{};
            out.write(zero.data(), static_cast<std::streamsize>(zero.size()));
            out.write(zero.data(), static_cast<std::streamsize>(zero.size()));
        }
        auto r = open_archive_backend(tar_sym, ArchiveFormat::Tar, ArchiveMode::Read, 6, ArchiveLimits{});
        expect_throw("tar unsafe symlink", [&] { r->extract_entry("link", hard_dir("tar-sym").string()); }, "unsafe");
    } catch (const std::exception& ex) {
        hard_fail("tar symlink setup", ex.what());
    }
}

void hard_test_limits(const std::filesystem::path& base) {
    using namespace pscript::archive;
    try {
        ArchiveLimits tight{};
        tight.max_entries = 2;
        tight.max_single_file_bytes = 16;
        tight.max_total_extract_bytes = 32;
        const std::string lim_zip = (base / "limits.zip").string();
        auto w = open_archive_backend(lim_zip, ArchiveFormat::Zip, ArchiveMode::Write, 6, tight);
        w->write_bytes("a", std::vector<std::uint8_t>{1});
        w->write_bytes("b", std::vector<std::uint8_t>{2});
        expect_throw("zip entry count limit", [&] { w->write_bytes("c", std::vector<std::uint8_t>{3}); }, "entry count");
        w->finalize();
        const std::string lim2 = (base / "limits2.zip").string();
        expect_throw("zip single file limit", [&] {
            auto w2 = open_archive_backend(lim2, ArchiveFormat::Zip, ArchiveMode::Write, 6, tight);
            w2->write_bytes("big", std::vector<std::uint8_t>(32, 1));
        }, "entry size");
    } catch (const std::exception& ex) {
        hard_fail("limits", ex.what());
    }
}

void hard_test_tar_gnu_long(const std::filesystem::path& base) {
    using namespace pscript::archive;
    try {
        const std::string gnu_tar = (base / "gnu-long.tar").string();
        std::string long_name(120, 'n');
        auto w = open_archive_backend(gnu_tar, ArchiveFormat::Tar, ArchiveMode::Write, 6, ArchiveLimits{});
        w->write_bytes(long_name, std::vector<std::uint8_t>{9, 8, 7});
        w->finalize();
        w.reset();
        auto r = open_archive_backend(gnu_tar, ArchiveFormat::Tar, ArchiveMode::Read, 6, ArchiveLimits{});
        const auto bytes = r->read_bytes(long_name);
        if (bytes.size() != 3 || bytes[0] != 9) {
            hard_fail("tar long name", "read failed");
        } else {
            hard_pass("tar long name");
        }
    } catch (const std::exception& ex) {
        hard_fail("tar long name", ex.what());
    }
}

void hard_test_targz_invalid(const std::filesystem::path& base) {
    using namespace pscript::archive;
    try {
        const std::string bad_gz = (base / "bad.tar.gz").string();
        { std::ofstream out(bad_gz, std::ios::binary); out << "not gzip"; }
        expect_throw("targz invalid gzip", [&] {
            auto r = open_archive_backend(bad_gz, ArchiveFormat::TarGz, ArchiveMode::Read, 6, ArchiveLimits{});
            (void)r->entries();
        }, "gzip");
    } catch (...) {
        hard_fail("targz invalid gzip setup", "unexpected");
    }
}

}  // namespace

int run_archive_hard_tests() {
    const auto base = hard_dir("base");
    hard_test_zip_small(base);
    hard_test_zip_utf8(base);
    hard_test_zip_truncated(base);
    hard_test_limits(base);
    hard_test_targz_invalid(base);
    hard_test_tar_security(base);
    hard_test_tar_gnu_long(base);
    if (g_hard_failed != 0) {
        std::cerr << g_hard_failed << " archive hard test(s) failed\n";
        return 1;
    }
    std::cout << "archive hard tests passed\n";
    return 0;
}
