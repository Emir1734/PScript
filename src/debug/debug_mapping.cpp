#include "debug/debug_mapping.h"

#include <algorithm>

namespace pscript::debug {
namespace {

std::string normalize_file(const Bytecode& bytecode, const std::string& file) {
    if (file.empty()) {
        return bytecode.source_file.empty() ? "<stdin>" : bytecode.source_file;
    }
    return file;
}

bool file_matches(const Bytecode& bytecode, const std::string& candidate, const std::string& requested) {
    const std::string norm = normalize_file(bytecode, requested);
    if (candidate == norm) {
        return true;
    }
    if (bytecode.source_file == candidate && norm == "<stdin>") {
        return true;
    }
    return false;
}

}  // namespace

std::uint32_t function_index_for_ip(const Bytecode& bytecode, std::size_t ip) {
    std::uint32_t best = 0xFFFFFFFFu;
    std::uint32_t best_entry = 0;
    for (std::uint32_t i = 0; i < bytecode.functions.size(); ++i) {
        const std::uint32_t entry = bytecode.functions[i].entry;
        if (entry <= ip && entry >= best_entry) {
            best_entry = entry;
            best = i;
        }
    }
    return best;
}

MappedLocation map_ip_to_source(const Bytecode& bytecode, std::size_t ip) {
    MappedLocation mapped;
    mapped.source_file = bytecode.source_file.empty() ? "<stdin>" : bytecode.source_file;
    mapped.function_index = function_index_for_ip(bytecode, ip);
    if (ip < bytecode.instruction_locations.size()) {
        mapped.location = bytecode.instruction_locations[ip];
    }
    return mapped;
}

std::vector<std::size_t> map_line_to_ips(const Bytecode& bytecode, const std::string& source_file,
                                         std::uint32_t line) {
    std::vector<std::size_t> ips;
    if (line == 0) {
        return ips;
    }
    const std::string norm = normalize_file(bytecode, source_file);
    for (std::size_t ip = 0; ip < bytecode.instructions.size(); ++ip) {
        if (ip >= bytecode.instruction_locations.size()) {
            break;
        }
        const SourceLocation& loc = bytecode.instruction_locations[ip];
        if (loc.line != line) {
            continue;
        }
        const MappedLocation at = map_ip_to_source(bytecode, ip);
        if (file_matches(bytecode, at.source_file, norm)) {
            ips.push_back(ip);
        }
    }
    std::sort(ips.begin(), ips.end());
    ips.erase(std::unique(ips.begin(), ips.end()), ips.end());
    return ips;
}

bool line_is_executable(const Bytecode& bytecode, const std::string& source_file, std::uint32_t line) {
    return !map_line_to_ips(bytecode, source_file, line).empty();
}

std::optional<std::size_t> primary_ip_for_line(const Bytecode& bytecode, const std::string& source_file,
                                               std::uint32_t line) {
    const std::vector<std::size_t> ips = map_line_to_ips(bytecode, source_file, line);
    if (ips.empty()) {
        return std::nullopt;
    }
    return ips.front();
}

}  // namespace pscript::debug
