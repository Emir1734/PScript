#pragma once

#include "runtime/runtime.h"
#include "stdlib/archive/archive_types.h"

#include <memory>

namespace pscript {

struct NativeArchiveData {
    std::uint32_t kind = 0;
    bool closed = true;
    archive::ArchiveFormat format = archive::ArchiveFormat::Zip;
    archive::ArchiveLimits limits{};
    std::unique_ptr<archive::ArchiveBackend> backend;
    std::vector<std::string> temp_entry_paths;
};

bool is_native_archive(const Value& object);
Value call_native_archive_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                 Runtime& runtime);

}  // namespace pscript
