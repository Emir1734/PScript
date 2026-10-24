#include "stdlib/native_archive.h"

#include "runtime/iterator.h"
#include "stdlib/native_common.h"
#include "stdlib/stdlib.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace pscript {
namespace {

constexpr std::uint32_t kArchiveClassIndex = 0;

[[noreturn]] void archive_fail(const std::string& message) {
    native_runtime_error(message);
}

NativeArchiveData& require_archive(const Value& object) {
    if (object.kind != ValueKind::Class || !object.class_value || !object.class_value->native_archive) {
        archive_fail("invalid archive operation");
    }
    return *object.class_value->native_archive;
}

NativeArchiveData& require_open_archive(const Value& object) {
    NativeArchiveData& data = require_archive(object);
    if (data.closed || !data.backend) {
        archive_fail("archive is closed");
    }
    return data;
}

std::vector<std::uint8_t> value_to_bytes(const Value& value) {
    if (value.kind == ValueKind::Bytes && value.bytes) {
        return value.bytes->data;
    }
    if (value.kind == ValueKind::String) {
        const std::string& text = value.string;
        return std::vector<std::uint8_t>(text.begin(), text.end());
    }
    native_type_error("expected Bytes or String");
}

archive::ArchiveFormat parse_format(const Value& value) {
    if (value.kind == ValueKind::Null) {
        return archive::ArchiveFormat::Zip;
    }
    const std::string& text = native_require_string(value, "archive.create", "format");
    if (text == "zip") {
        return archive::ArchiveFormat::Zip;
    }
    if (text == "tar") {
        return archive::ArchiveFormat::Tar;
    }
    if (text == "tar.gz" || text == "tgz") {
        return archive::ArchiveFormat::TarGz;
    }
    native_value_error("unsupported archive format '" + text + "'");
}

archive::ArchiveMode parse_open_mode(const std::string& mode_text) {
    if (mode_text == "r" || mode_text == "rb") {
        return archive::ArchiveMode::Read;
    }
    if (mode_text == "w" || mode_text == "wb" || mode_text == "a" || mode_text == "ab") {
        return archive::ArchiveMode::Write;
    }
    native_value_error("unsupported archive mode '" + mode_text + "'");
}

std::uint64_t dict_uint64_field(const DictData& dict, const std::string& key, std::uint64_t fallback) {
    for (const auto& entry : dict.entries) {
        if (entry.first != key) {
            continue;
        }
        if (entry.second.kind != ValueKind::Integer) {
            native_type_error("archive limits field '" + key + "' must be integer");
        }
        if (entry.second.integer < 0) {
            native_value_error("archive limits field '" + key + "' must be non-negative");
        }
        return static_cast<std::uint64_t>(entry.second.integer);
    }
    return fallback;
}

archive::ArchiveLimits limits_from_dict(const DictData& dict, const archive::ArchiveLimits& defaults) {
    archive::ArchiveLimits limits = defaults;
    limits.max_entries = dict_uint64_field(dict, "max_entries", limits.max_entries);
    limits.max_single_file_bytes = dict_uint64_field(dict, "max_single_file_bytes", limits.max_single_file_bytes);
    limits.max_total_extract_bytes =
        dict_uint64_field(dict, "max_total_extract_bytes", limits.max_total_extract_bytes);
    return limits;
}

archive::ArchiveLimits parse_optional_limits(const std::vector<Value>& args, std::size_t start_index) {
    archive::ArchiveLimits limits{};
    for (std::size_t i = start_index; i < args.size(); ++i) {
        if (args[i].kind == ValueKind::Dict && args[i].dict) {
            return limits_from_dict(*args[i].dict, limits);
        }
    }
    return limits;
}

int parse_optional_compression_level(const std::vector<Value>& args, std::size_t start_index) {
    int level = 6;
    for (std::size_t i = start_index; i < args.size(); ++i) {
        if (args[i].kind == ValueKind::Integer) {
            level = static_cast<int>(args[i].integer);
        }
    }
    return level;
}

Value make_metadata_dict(const archive::ArchiveEntryInfo& entry) {
    std::vector<std::pair<std::string, Value>> fields = {
        {"mode", Runtime::make_integer(static_cast<std::int64_t>(entry.mode))},
        {"modified", Runtime::make_integer(entry.modified_unix)},
        {"is_symlink", Runtime::make_bool(entry.is_symlink)},
        {"tar_typeflag", Runtime::make_string(std::string(1, entry.tar_typeflag))},
    };
    if (!entry.symlink_target.empty()) {
        fields.push_back({"link_target", Runtime::make_string(entry.symlink_target)});
    }
    return Runtime::make_dict(std::move(fields));
}

Value make_entry_dict(const archive::ArchiveEntryInfo& entry) {
    return Runtime::make_dict({{"name", Runtime::make_string(entry.name)},
                               {"size", Runtime::make_integer(static_cast<std::int64_t>(entry.size))},
                               {"compressed_size",
                                Runtime::make_integer(static_cast<std::int64_t>(entry.compressed_size))},
                               {"is_file", Runtime::make_bool(entry.is_file)},
                               {"is_directory", Runtime::make_bool(entry.is_directory)},
                               {"is_symlink", Runtime::make_bool(entry.is_symlink)},
                               {"modified", Runtime::make_integer(entry.modified_unix)},
                               {"mode", Runtime::make_integer(static_cast<std::int64_t>(entry.mode))},
                               {"metadata", make_metadata_dict(entry)}});
}

Value make_archive_value(std::unique_ptr<archive::ArchiveBackend> backend, archive::ArchiveFormat format,
                         archive::ArchiveLimits limits) {
    auto data = std::make_shared<NativeArchiveData>();
    data->kind = static_cast<std::uint32_t>(NativeClassKind::Archive);
    data->closed = false;
    data->format = format;
    data->limits = limits;
    data->backend = std::move(backend);
    Value value = Runtime::make_class(kArchiveClassIndex, "Archive", std::vector<std::string>{});
    value.class_value->native_archive = std::move(data);
    return value;
}

std::string optional_destination(const std::vector<Value>& args, std::size_t index) {
    if (args.size() <= index || args[index].kind == ValueKind::Null) {
        return ".";
    }
    return native_require_string_arg(args, index, "archive", "destination");
}

void cleanup_temp_paths(NativeArchiveData& data) {
    for (const std::string& path : data.temp_entry_paths) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    data.temp_entry_paths.clear();
}

void close_archive(NativeArchiveData& data) {
    if (data.closed) {
        return;
    }
    cleanup_temp_paths(data);
    if (data.backend) {
        try {
            data.backend->finalize();
        } catch (const std::exception& ex) {
            data.backend.reset();
            data.closed = true;
            archive_fail(ex.what());
        }
        data.backend.reset();
    }
    data.closed = true;
}

std::string unique_temp_entry_path() {
    static std::atomic<int> counter{0};
    return (std::filesystem::temp_directory_path() /
            ("pscript-archive-entry-" + std::to_string(++counter) + ".bin"))
        .string();
}

}  // namespace

Value native_archive_open(Runtime& /*runtime*/, const std::vector<Value>& args) {
    native_require_min_argc("archive.open", args, 1);
    const std::string& path = native_require_string_arg(args, 0, "archive.open", "path");
    std::string mode = "r";
    if (args.size() >= 2 && args[1].kind != ValueKind::Null) {
        mode = native_require_string_arg(args, 1, "archive.open", "mode");
    }
    const int compression_level = parse_optional_compression_level(args, 2);
    const archive::ArchiveLimits limits = parse_optional_limits(args, 2);
    const archive::ArchiveFormat format = archive::detect_format_from_path(path);
    try {
        auto backend =
            archive::open_archive_backend(path, format, parse_open_mode(mode), compression_level, limits);
        return make_archive_value(std::move(backend), format, limits);
    } catch (const std::exception& ex) {
        archive_fail(ex.what());
    }
}

Value native_archive_create(Runtime& /*runtime*/, const std::vector<Value>& args) {
    native_require_min_argc("archive.create", args, 1);
    const std::string& path = native_require_string_arg(args, 0, "archive.create", "path");
    archive::ArchiveFormat format = archive::detect_format_from_path(path);
    if (args.size() >= 2 && args[1].kind == ValueKind::String) {
        format = parse_format(args[1]);
    }
    const int compression_level = parse_optional_compression_level(args, 2);
    const archive::ArchiveLimits limits = parse_optional_limits(args, 2);
    try {
        auto backend =
            archive::open_archive_backend(path, format, archive::ArchiveMode::Write, compression_level, limits);
        return make_archive_value(std::move(backend), format, limits);
    } catch (const std::exception& ex) {
        archive_fail(ex.what());
    }
}

bool is_native_archive(const Value& object) {
    return object.kind == ValueKind::Class && object.class_value && object.class_value->native_archive;
}

Value call_native_archive_method(const Value& object, std::string_view name, const std::vector<Value>& args,
                                 Runtime& /*runtime*/) {
    if (name == "close") {
        NativeArchiveData& data = require_archive(object);
        close_archive(data);
        return Runtime::make_null();
    }
    if (name == "is_open") {
        const NativeArchiveData& data = require_archive(object);
        return Runtime::make_bool(!data.closed && data.backend);
    }
    if (name == "list") {
        const NativeArchiveData& data = require_open_archive(object);
        std::vector<Value> names;
        for (const auto& entry : data.backend->entries()) {
            names.push_back(Runtime::make_string(entry.name));
        }
        return Runtime::make_list(std::move(names));
    }
    if (name == "entries") {
        const NativeArchiveData& data = require_open_archive(object);
        std::vector<Value> entries;
        for (const auto& entry : data.backend->entries()) {
            entries.push_back(make_entry_dict(entry));
        }
        return Runtime::make_list(std::move(entries));
    }
    if (name == "read") {
        native_require_argc("archive.read", args, 1);
        NativeArchiveData& data = require_open_archive(object);
        const std::string& entry_name = native_require_string_arg(args, 0, "archive.read", "name");
        try {
            return Runtime::make_bytes(data.backend->read_bytes(entry_name));
        } catch (const std::exception& ex) {
            archive_fail(ex.what());
        }
    }
    if (name == "open_entry") {
        native_require_argc("archive.open_entry", args, 1);
        NativeArchiveData& data = require_open_archive(object);
        const std::string& entry_name = native_require_string_arg(args, 0, "archive.open_entry", "name");
        try {
            const std::vector<std::uint8_t> bytes = data.backend->read_bytes(entry_name);
            const std::string temp_path = unique_temp_entry_path();
            std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
            if (!out) {
                archive_fail("failed to create temporary file for archive entry");
            }
            if (!bytes.empty()) {
                out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
            if (!out) {
                archive_fail("failed to write temporary file for archive entry");
            }
            data.temp_entry_paths.push_back(temp_path);
            return make_native_file_handle(0, temp_path, "r");
        } catch (const std::exception& ex) {
            archive_fail(ex.what());
        }
    }
    if (name == "write") {
        native_require_argc("archive.write", args, 2);
        NativeArchiveData& data = require_open_archive(object);
        const std::string& entry_name = native_require_string_arg(args, 0, "archive.write", "name");
        const std::vector<std::uint8_t> bytes = value_to_bytes(args[1]);
        try {
            data.backend->write_bytes(entry_name, bytes);
            return Runtime::make_null();
        } catch (const std::exception& ex) {
            archive_fail(ex.what());
        }
    }
    if (name == "add") {
        native_require_min_argc("archive.add", args, 1);
        NativeArchiveData& data = require_open_archive(object);
        const std::string& source_path = native_require_string_arg(args, 0, "archive.add", "path");
        std::string archive_name;
        if (args.size() >= 2 && args[1].kind != ValueKind::Null) {
            archive_name = native_require_string_arg(args, 1, "archive.add", "name");
        }
        try {
            data.backend->add_path(source_path, archive_name);
            return Runtime::make_null();
        } catch (const std::exception& ex) {
            archive_fail(ex.what());
        }
    }
    if (name == "extract") {
        native_require_argc("archive.extract", args, 1);
        NativeArchiveData& data = require_open_archive(object);
        const std::string& entry_name = native_require_string_arg(args, 0, "archive.extract", "name");
        const std::string destination = optional_destination(args, 1);
        try {
            data.backend->extract_entry(entry_name, destination);
            return Runtime::make_null();
        } catch (const std::exception& ex) {
            archive_fail(ex.what());
        }
    }
    if (name == "extract_all") {
        NativeArchiveData& data = require_open_archive(object);
        const std::string destination = optional_destination(args, 0);
        try {
            data.backend->extract_all(destination);
            return Runtime::make_null();
        } catch (const std::exception& ex) {
            archive_fail(ex.what());
        }
    }
    if (name == "iter") {
        require_open_archive(object);
        return runtime_get_iter(object);
    }
    archive_fail("Archive has no method '" + std::string(name) + "'");
}

}  // namespace pscript
