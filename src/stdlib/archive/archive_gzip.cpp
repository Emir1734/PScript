#include "stdlib/archive/archive_gzip.h"

#include "stdlib/archive/archive_security.h"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace pscript::archive {
namespace {

constexpr std::size_t kChunk = 64 * 1024;

struct GzipReaderState {
    std::ifstream file;
    mz_stream stream{};
    std::vector<std::uint8_t> in;
    std::vector<std::uint8_t> out;
    bool initialized = false;
};

struct GzipWriterState {
    std::ofstream file;
    mz_stream stream{};
    std::vector<std::uint8_t> in;
    std::vector<std::uint8_t> out;
    bool initialized = false;
    bool finished = false;
};

}  // namespace

GzipReader::GzipReader(const std::filesystem::path& path) : state_(new GzipReaderState()) {
    auto* state = static_cast<GzipReaderState*>(state_);
    state->file.open(path, std::ios::binary);
    if (!state->file) {
        throw std::runtime_error("failed to open gzip file");
    }
    state->in.resize(kChunk);
    state->out.resize(kChunk);
    std::memset(&state->stream, 0, sizeof(state->stream));
    if (mz_inflateInit2(&state->stream, -MZ_DEFAULT_WINDOW_BITS) != MZ_OK) {
        throw std::runtime_error("gzip inflate init failed");
    }
    state->initialized = true;
}

GzipReader::~GzipReader() {
    if (state_ != nullptr) {
        auto* state = static_cast<GzipReaderState*>(state_);
        if (state->initialized) {
            mz_inflateEnd(&state->stream);
        }
        delete state;
        state_ = nullptr;
    }
}

bool GzipReader::read(std::vector<std::uint8_t>& buffer, std::size_t max_chunk) {
    buffer.clear();
    if (eof_) {
        return false;
    }
    auto* state = static_cast<GzipReaderState*>(state_);
    state->stream.next_out = state->out.data();
    state->stream.avail_out = static_cast<unsigned int>(std::min(max_chunk, state->out.size()));
    while (state->stream.avail_out > 0) {
        if (state->stream.avail_in == 0 && !state->file.eof()) {
            state->file.read(reinterpret_cast<char*>(state->in.data()),
                             static_cast<std::streamsize>(state->in.size()));
            const std::streamsize got = state->file.gcount();
            if (got <= 0) {
                eof_ = true;
            } else {
                state->stream.next_in = state->in.data();
                state->stream.avail_in = static_cast<unsigned int>(got);
            }
        }
        const int status = mz_inflate(&state->stream, MZ_NO_FLUSH);
        if (status == MZ_STREAM_END) {
            eof_ = true;
            break;
        }
        if (status != MZ_OK && status != MZ_BUF_ERROR) {
            throw std::runtime_error("gzip inflate failed");
        }
        if (state->stream.avail_in == 0 && state->file.eof() &&
            state->stream.avail_out == static_cast<unsigned int>(max_chunk)) {
            break;
        }
    }
    const std::size_t produced = max_chunk - state->stream.avail_out;
    if (produced == 0 && eof_) {
        return false;
    }
    buffer.assign(state->out.begin(), state->out.begin() + static_cast<std::ptrdiff_t>(produced));
    return !buffer.empty();
}

void decompress_gzip_file_to_file(const std::filesystem::path& gzip_path, const std::filesystem::path& output_path,
                                  const ArchiveLimits& limits) {
    GzipReader reader(gzip_path);
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open tar spill file");
    }
    std::uint64_t total = 0;
    std::vector<std::uint8_t> chunk;
    while (reader.read(chunk, kChunk)) {
        total += chunk.size();
        check_extract_limits(limits, 0, total, chunk.size());
        out.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        if (!out) {
            throw std::runtime_error("failed to write decompressed tar stream");
        }
    }
    out.flush();
}

std::vector<std::uint8_t> GzipReader::read_all() {
    std::vector<std::uint8_t> all;
    std::vector<std::uint8_t> chunk;
    while (read(chunk, kChunk)) {
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    return all;
}

GzipWriter::GzipWriter(const std::filesystem::path& path, int compression_level) : state_(new GzipWriterState()) {
    auto* state = static_cast<GzipWriterState*>(state_);
    state->file.open(path, std::ios::binary | std::ios::trunc);
    if (!state->file) {
        throw std::runtime_error("failed to open gzip file for writing");
    }
    state->in.resize(kChunk);
    state->out.resize(kChunk);
    std::memset(&state->stream, 0, sizeof(state->stream));
    if (mz_deflateInit2(&state->stream, compression_level, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 9,
                        MZ_DEFAULT_STRATEGY) != MZ_OK) {
        throw std::runtime_error("gzip deflate init failed");
    }
    state->initialized = true;
}

GzipWriter::~GzipWriter() {
    if (state_ != nullptr) {
        auto* state = static_cast<GzipWriterState*>(state_);
        try {
            if (state->initialized && !state->finished) {
                finish();
            }
        } catch (...) {
        }
        if (state->initialized) {
            mz_deflateEnd(&state->stream);
        }
        delete state;
        state_ = nullptr;
    }
}

void GzipWriter::write(const std::uint8_t* data, std::size_t size) {
    auto* state = static_cast<GzipWriterState*>(state_);
    if (state->finished) {
        throw std::runtime_error("gzip writer already finished");
    }
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t chunk = std::min(kChunk, size - offset);
        state->stream.next_in = const_cast<unsigned char*>(data + offset);
        state->stream.avail_in = static_cast<unsigned int>(chunk);
        while (state->stream.avail_in > 0) {
            state->stream.next_out = state->out.data();
            state->stream.avail_out = static_cast<unsigned int>(state->out.size());
            const int status = mz_deflate(&state->stream, MZ_NO_FLUSH);
            if (status != MZ_OK) {
                throw std::runtime_error("gzip deflate failed");
            }
            const std::size_t written = state->out.size() - state->stream.avail_out;
            if (written > 0) {
                state->file.write(reinterpret_cast<const char*>(state->out.data()),
                                  static_cast<std::streamsize>(written));
            }
        }
        offset += chunk;
    }
}

void GzipWriter::finish() {
    auto* state = static_cast<GzipWriterState*>(state_);
    if (state->finished) {
        return;
    }
    int status = MZ_OK;
    do {
        state->stream.next_in = nullptr;
        state->stream.avail_in = 0;
        state->stream.next_out = state->out.data();
        state->stream.avail_out = static_cast<unsigned int>(state->out.size());
        status = mz_deflate(&state->stream, MZ_FINISH);
        const std::size_t written = state->out.size() - state->stream.avail_out;
        if (written > 0) {
            state->file.write(reinterpret_cast<const char*>(state->out.data()),
                              static_cast<std::streamsize>(written));
        }
    } while (status == MZ_OK);
    if (status != MZ_STREAM_END) {
        throw std::runtime_error("gzip finalize failed");
    }
    state->file.flush();
    state->finished = true;
}

}  // namespace pscript::archive
