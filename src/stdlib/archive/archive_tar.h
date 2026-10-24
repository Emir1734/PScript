#pragma once

#include "stdlib/archive/archive_types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pscript::archive {

std::unique_ptr<ArchiveBackend> make_tar_backend(const std::string& path, ArchiveMode mode,
                                                 const ArchiveLimits& limits);
std::unique_ptr<ArchiveBackend> make_targz_backend(const std::string& path, ArchiveMode mode, int gzip_level,
                                                   const ArchiveLimits& limits);

}  // namespace pscript::archive
