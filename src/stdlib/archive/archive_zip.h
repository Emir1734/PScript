#pragma once

#include "stdlib/archive/archive_types.h"

#include <memory>
#include <string>

namespace pscript::archive {

std::unique_ptr<ArchiveBackend> make_zip_backend(const std::string& path, ArchiveMode mode, int compression_level,
                                                 const ArchiveLimits& limits);

}  // namespace pscript::archive
