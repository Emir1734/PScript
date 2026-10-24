#include "compiler/embed_codegen.h"

#include "stdlib/archive/archive_security.h"

#include <algorithm>
#include <fstream>
#include <filesystem>

namespace pscript {
namespace {

[[noreturn]] void embed_fail(const std::string& message) {
    throw std::runtime_error(message);
}

bool is_valid_utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        if ((byte & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (std::size_t j = 1; j <= extra; ++j) {
            const unsigned char cont = static_cast<unsigned char>(text[i + j]);
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

std::filesystem::path resolve_embed_path(const std::string& source_file, std::string_view relative_raw) {
    if (relative_raw.empty()) {
        embed_fail("embed path must not be empty");
    }
    if (archive::is_absolute_archive_path(relative_raw)) {
        embed_fail("embed path must be relative to the source file directory");
    }
    if (!archive::normalize_archive_entry_path(relative_raw, false).has_value()) {
        embed_fail("embed path is not allowed");
    }
    const std::filesystem::path source_path =
        source_file == "<stdin>" ? std::filesystem::current_path() : std::filesystem::path(source_file);
    const std::filesystem::path base_dir =
        source_file == "<stdin>" ? source_path : source_path.parent_path();
    const std::filesystem::path combined = base_dir / std::filesystem::path(std::string(relative_raw));
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(combined, ec);
    if (ec) {
        embed_fail("embed path could not be resolved");
    }
    const std::filesystem::path base_canonical = std::filesystem::weakly_canonical(base_dir, ec);
    if (ec) {
        embed_fail("embed source directory could not be resolved");
    }
    const std::string canon = canonical.generic_string();
    const std::string base = base_canonical.generic_string();
    if (canon.size() < base.size() || canon.rfind(base, 0) != 0) {
        embed_fail("embed path escapes the source directory");
    }
    return canonical;
}

std::vector<std::uint8_t> read_embed_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        embed_fail("embed resource not found: " + path.generic_string());
    }
    input.seekg(0, std::ios::end);
    const std::streamoff size = input.tellg();
    if (size < 0) {
        embed_fail("embed resource could not be read");
    }
    input.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!data.empty()) {
        input.read(reinterpret_cast<char*>(data.data()), size);
    }
    if (!input) {
        embed_fail("embed resource could not be read");
    }
    return data;
}

std::string decode_string_literal(std::string_view lexeme) {
    if (lexeme.size() < 2 || lexeme.front() != '"' || lexeme.back() != '"') {
        embed_fail("embed path must be a string literal");
    }
    std::string out;
    for (std::size_t i = 1; i + 1 < lexeme.size(); ++i) {
        if (lexeme[i] == '\\' && i + 1 < lexeme.size() - 1) {
            const char esc = lexeme[++i];
            if (esc == 'n') {
                out.push_back('\n');
            } else if (esc == 't') {
                out.push_back('\t');
            } else if (esc == 'r') {
                out.push_back('\r');
            } else if (esc == '\\' || esc == '"') {
                out.push_back(esc);
            } else {
                out.push_back(esc);
            }
            continue;
        }
        out.push_back(lexeme[i]);
    }
    return out;
}

}  // namespace

std::vector<std::uint8_t> load_embed_bytes(Bytecode& bytecode, const std::string& source_file,
                                           const Constant& path_literal) {
    if (path_literal.kind != ConstantKind::String) {
        embed_fail("embed path must be a string literal");
    }
    const std::string relative = decode_string_literal(path_literal.lexeme);
    const std::filesystem::path resolved = resolve_embed_path(source_file, relative);
    record_embed_dependency(bytecode, resolved);
    return read_embed_file(resolved);
}

void record_embed_dependency(Bytecode& bytecode, const std::filesystem::path& path) {
    const std::string normalized = path.generic_string();
    if (std::find(bytecode.compile_dependencies.begin(), bytecode.compile_dependencies.end(), normalized) ==
        bytecode.compile_dependencies.end()) {
        bytecode.compile_dependencies.push_back(normalized);
    }
}

Constant make_embed_bytes_constant(std::vector<std::uint8_t> bytes) {
    Constant constant;
    constant.kind = ConstantKind::Bytes;
    constant.bytes_value = std::move(bytes);
    return constant;
}

Constant make_embed_text_constant(std::vector<std::uint8_t> bytes) {
    const std::string text(bytes.begin(), bytes.end());
    if (!is_valid_utf8(text)) {
        embed_fail("embed.text resource is not valid UTF-8");
    }
    Constant constant;
    constant.kind = ConstantKind::String;
    constant.lexeme = "\"" + text + "\"";
    return constant;
}

}  // namespace pscript
