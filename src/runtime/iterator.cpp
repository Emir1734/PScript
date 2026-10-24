#include "runtime/iterator.h"
#include "runtime/sql_ops.h"
#include "stdlib/native_archive.h"

namespace pscript {
namespace {

[[noreturn]] void iterator_fail(const std::string& message) {
    throw RuntimeError(message, RuntimeErrorCategory::Type);
}

const char* iterable_kind_name(ValueKind kind) {
    switch (kind) {
        case ValueKind::Integer:
            return "int";
        case ValueKind::Float:
            return "float";
        case ValueKind::String:
            return "string";
        case ValueKind::Bool:
            return "bool";
        case ValueKind::Null:
            return "null";
        case ValueKind::List:
            return "list";
        case ValueKind::Dict:
            return "dict";
        case ValueKind::Tuple:
            return "tuple";
        case ValueKind::Set:
            return "set";
        case ValueKind::Struct:
            return "struct";
        case ValueKind::Class:
            return "class";
        case ValueKind::Enum:
            return "enum";
        case ValueKind::Range:
            return "range";
        case ValueKind::Iterator:
            return "iterator";
        case ValueKind::Bytes:
            return "bytes";
        case ValueKind::Function:
            return "function";
        case ValueKind::Module:
            return "module";
        case ValueKind::Task:
            return "task";
    }
    return "value";
}

std::int64_t range_length(const RangeData& range) {
    if ((range.step > 0 && range.start >= range.stop) || (range.step < 0 && range.start <= range.stop)) {
        return 0;
    }
    if (range.step > 0) {
        const std::uint64_t diff = static_cast<std::uint64_t>(range.stop - range.start);
        const std::uint64_t step = static_cast<std::uint64_t>(range.step);
        return static_cast<std::int64_t>((diff + step - 1) / step);
    }
    const std::uint64_t diff = static_cast<std::uint64_t>(range.start - range.stop);
    const std::uint64_t step = static_cast<std::uint64_t>(-range.step);
    return static_cast<std::int64_t>((diff + step - 1) / step);
}

NativeIteratorData& require_native_iterator(const Value& value) {
    if (value.kind == ValueKind::Iterator && value.iterator_data) {
        return *value.iterator_data;
    }
    if (value.kind == ValueKind::Class && value.class_value && value.class_value->native_iterator) {
        return *value.class_value->native_iterator;
    }
    iterator_fail("invalid iterator operation");
}

bool is_native_archive_value(const Value& value) {
    return value.kind == ValueKind::Class && value.class_value && value.class_value->native_archive;
}

Value archive_entry_dict(const archive::ArchiveEntryInfo& entry) {
    std::vector<std::pair<std::string, Value>> metadata_fields = {
        {"mode", Runtime::make_integer(static_cast<std::int64_t>(entry.mode))},
        {"modified", Runtime::make_integer(entry.modified_unix)},
        {"is_symlink", Runtime::make_bool(entry.is_symlink)},
        {"tar_typeflag", Runtime::make_string(std::string(1, entry.tar_typeflag))},
    };
    if (!entry.symlink_target.empty()) {
        metadata_fields.push_back({"link_target", Runtime::make_string(entry.symlink_target)});
    }
    return Runtime::make_dict({{"name", Runtime::make_string(entry.name)},
                               {"size", Runtime::make_integer(static_cast<std::int64_t>(entry.size))},
                               {"compressed_size",
                                Runtime::make_integer(static_cast<std::int64_t>(entry.compressed_size))},
                               {"is_file", Runtime::make_bool(entry.is_file)},
                               {"is_directory", Runtime::make_bool(entry.is_directory)},
                               {"is_symlink", Runtime::make_bool(entry.is_symlink)},
                               {"modified", Runtime::make_integer(entry.modified_unix)},
                               {"mode", Runtime::make_integer(static_cast<std::int64_t>(entry.mode))},
                               {"metadata", Runtime::make_dict(std::move(metadata_fields))}});
}

bool advance_native_iterator(NativeIteratorData& data, Value& out) {
    if (data.exhausted) {
        return false;
    }

    switch (data.kind) {
        case NativeIteratorKind::List: {
            if (!data.source.list) {
                iterator_fail("invalid list iterator");
            }
            if (data.index >= static_cast<std::int64_t>(data.source.list->items.size())) {
                data.exhausted = true;
                return false;
            }
            out = data.source.list->items[static_cast<std::size_t>(data.index++)];
            return true;
        }
        case NativeIteratorKind::Tuple: {
            if (!data.source.tuple) {
                iterator_fail("invalid tuple iterator");
            }
            if (data.index >= static_cast<std::int64_t>(data.source.tuple->items.size())) {
                data.exhausted = true;
                return false;
            }
            out = data.source.tuple->items[static_cast<std::size_t>(data.index++)];
            return true;
        }
        case NativeIteratorKind::Dict: {
            if (!data.source.dict) {
                iterator_fail("invalid dictionary iterator");
            }
            if (data.index >= static_cast<std::int64_t>(data.source.dict->entries.size())) {
                data.exhausted = true;
                return false;
            }
            out = Runtime::make_string(data.source.dict->entries[static_cast<std::size_t>(data.index++)].first);
            return true;
        }
        case NativeIteratorKind::Set: {
            if (!data.source.set) {
                iterator_fail("invalid set iterator");
            }
            if (data.index >= static_cast<std::int64_t>(data.source.set->items.size())) {
                data.exhausted = true;
                return false;
            }
            out = data.source.set->items[static_cast<std::size_t>(data.index++)];
            return true;
        }
        case NativeIteratorKind::Range: {
            if (!data.source.range) {
                iterator_fail("invalid range iterator");
            }
            const std::int64_t length = range_length(*data.source.range);
            if (data.index >= length) {
                data.exhausted = true;
                return false;
            }
            out = Runtime::make_integer(data.source.range->start + data.index * data.source.range->step);
            ++data.index;
            return true;
        }
        case NativeIteratorKind::Bytes: {
            if (!data.source.bytes) {
                iterator_fail("invalid bytes iterator");
            }
            if (data.index >= static_cast<std::int64_t>(data.source.bytes->data.size())) {
                data.exhausted = true;
                return false;
            }
            out = Runtime::make_integer(
                static_cast<std::int64_t>(data.source.bytes->data[static_cast<std::size_t>(data.index++)]));
            return true;
        }
        case NativeIteratorKind::SqlRows: {
            if (!data.source.class_value || !data.source.class_value->native_sql_rows ||
                !data.source.class_value->native_sql_rows->state) {
                iterator_fail("invalid SQL rows iterator");
            }
            try {
                out = data.source.class_value->native_sql_rows->state->next_row();
                return true;
            } catch (const RuntimeError& err) {
                if (err.message() == "StopIteration") {
                    data.exhausted = true;
                    return false;
                }
                throw;
            }
        }
        case NativeIteratorKind::Archive: {
            if (!data.source.class_value || !data.source.class_value->native_archive ||
                !data.source.class_value->native_archive->backend) {
                iterator_fail("invalid archive iterator");
            }
            const auto& entries = data.source.class_value->native_archive->backend->entries();
            if (data.index >= static_cast<std::int64_t>(entries.size())) {
                data.exhausted = true;
                return false;
            }
            out = archive_entry_dict(entries[static_cast<std::size_t>(data.index++)]);
            return true;
        }
    }
    iterator_fail("invalid iterator operation");
}

}  // namespace

bool is_native_iterator(const Value& value) {
    return (value.kind == ValueKind::Iterator && value.iterator_data != nullptr) ||
           (value.kind == ValueKind::Class && value.class_value && value.class_value->native_iterator != nullptr);
}

Value make_native_iterator(NativeIteratorKind kind, Value source) {
    auto data = std::make_shared<NativeIteratorData>();
    data->kind = kind;
    data->source = std::move(source);
    data->index = 0;
    data->exhausted = false;

    Value iterator;
    iterator.kind = ValueKind::Iterator;
    iterator.iterator_data = std::move(data);
    return iterator;
}

Value native_iterator_iter(const Value& iterator) {
    require_native_iterator(iterator);
    return iterator;
}

bool native_iterator_try_next(const Value& iterator, Value& out) {
    return advance_native_iterator(require_native_iterator(iterator), out);
}

Value native_iterator_next(const Value& iterator) {
    Value out;
    if (!native_iterator_try_next(iterator, out)) {
        throw RuntimeError("StopIteration", RuntimeErrorCategory::Runtime);
    }
    return out;
}

bool is_native_sql_rows(const Value& value) {
    return value.kind == ValueKind::Class && value.class_value && value.class_value->native_sql_rows &&
           value.class_value->native_sql_rows->state;
}

Value runtime_get_iter(const Value& value) {
    if (is_native_iterator(value)) {
        return value;
    }
    switch (value.kind) {
        case ValueKind::List:
            return make_native_iterator(NativeIteratorKind::List, value);
        case ValueKind::Tuple:
            return make_native_iterator(NativeIteratorKind::Tuple, value);
        case ValueKind::Dict:
            return make_native_iterator(NativeIteratorKind::Dict, value);
        case ValueKind::Set:
            return make_native_iterator(NativeIteratorKind::Set, value);
        case ValueKind::Range:
            return make_native_iterator(NativeIteratorKind::Range, value);
        case ValueKind::Bytes:
            return make_native_iterator(NativeIteratorKind::Bytes, value);
        default:
            break;
    }
    if (is_native_sql_rows(value)) {
        return make_native_iterator(NativeIteratorKind::SqlRows, value);
    }
    if (is_native_archive_value(value)) {
        return make_native_iterator(NativeIteratorKind::Archive, value);
    }
    iterator_fail(std::string("cannot iterate ") + iterable_kind_name(value.kind));
}

}  // namespace pscript
