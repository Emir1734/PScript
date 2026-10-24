#include "stdlib/archive/archive_types.h"

#include "stdlib/archive/archive_tar.h"
#include "stdlib/archive/archive_zip.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace pscript::archive {
namespace {

std::string lower_copy(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return out;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

ArchiveFormat detect_format_from_path(const std::string& path) {
    const std::string lower = lower_copy(path);
    if (ends_with(lower, ".tar.gz") || ends_with(lower, ".tgz")) {
        return ArchiveFormat::TarGz;
    }
    if (ends_with(lower, ".tar")) {
        return ArchiveFormat::Tar;
    }
    if (ends_with(lower, ".zip")) {
        return ArchiveFormat::Zip;
    }
    return ArchiveFormat::Zip;
}

std::string format_to_string(ArchiveFormat format) {
    switch (format) {
        case ArchiveFormat::Zip:
            return "zip";
        case ArchiveFormat::Tar:
            return "tar";
        case ArchiveFormat::TarGz:
            return "tar.gz";
    }
    return "zip";
}

std::unique_ptr<ArchiveBackend> open_archive_backend(const std::string& path, ArchiveFormat format, ArchiveMode mode,
                                                     int zip_compression_level, const ArchiveLimits& limits) {
    switch (format) {
        case ArchiveFormat::Zip:
            return make_zip_backend(path, mode, zip_compression_level, limits);
        case ArchiveFormat::Tar:
            return make_tar_backend(path, mode, limits);
        case ArchiveFormat::TarGz:
            return make_targz_backend(path, mode, zip_compression_level, limits);
    }
    throw std::runtime_error("unsupported archive format");
}

}  // namespace pscript::archive
