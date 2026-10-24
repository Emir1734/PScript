#pragma once

#include "compiler/bytecode.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace pscript {

std::vector<std::uint8_t> load_embed_bytes(Bytecode& bytecode, const std::string& source_file,
                                           const Constant& path_literal);
void record_embed_dependency(Bytecode& bytecode, const std::filesystem::path& path);
Constant make_embed_bytes_constant(std::vector<std::uint8_t> bytes);
Constant make_embed_text_constant(std::vector<std::uint8_t> bytes);

}  // namespace pscript
