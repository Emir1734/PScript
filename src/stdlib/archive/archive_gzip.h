#pragma once

#include "stdlib/archive/archive_types.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pscript::archive {

class GzipReader {
public:
    explicit GzipReader(const std::filesystem::path& path);
    ~GzipReader();
    GzipReader(const GzipReader&) = delete;
    GzipReader& operator=(const GzipReader&) = delete;
    bool read(std::vector<std::uint8_t>& buffer, std::size_t max_chunk);
    std::vector<std::uint8_t> read_all();
    bool eof() const { return eof_; }

private:
    void* state_ = nullptr;
    bool eof_ = false;
};

void decompress_gzip_file_to_file(const std::filesystem::path& gzip_path, const std::filesystem::path& output_path,
                                  const ArchiveLimits& limits);

class GzipWriter {
public:
    explicit GzipWriter(const std::filesystem::path& path, int compression_level);
    ~GzipWriter();
    GzipWriter(const GzipWriter&) = delete;
    GzipWriter& operator=(const GzipWriter&) = delete;
    void write(const std::uint8_t* data, std::size_t size);
    void finish();

private:
    void* state_ = nullptr;
};

}  // namespace pscript::archive
