#pragma once

#include "compiler/bytecode.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pscript::debug {

struct MappedLocation {
    std::string source_file;
    SourceLocation location;
    std::uint32_t function_index = 0xFFFFFFFFu;
};

std::uint32_t function_index_for_ip(const Bytecode& bytecode, std::size_t ip);

MappedLocation map_ip_to_source(const Bytecode& bytecode, std::size_t ip);

std::vector<std::size_t> map_line_to_ips(const Bytecode& bytecode, const std::string& source_file,
                                         std::uint32_t line);

bool line_is_executable(const Bytecode& bytecode, const std::string& source_file, std::uint32_t line);

std::optional<std::size_t> primary_ip_for_line(const Bytecode& bytecode, const std::string& source_file,
                                               std::uint32_t line);

}  // namespace pscript::debug
