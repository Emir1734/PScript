#include "package/atomic_file.h"

#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace pscript::package {

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool write_file_atomically(const std::filesystem::path& destination, const std::string& content, std::string* error) {
    std::error_code ec;
    if (!destination.parent_path().empty()) {
        std::filesystem::create_directories(destination.parent_path(), ec);
        if (ec) {
            if (error) {
                *error = ec.message();
            }
            return false;
        }
    }
    const std::filesystem::path temp =
        destination.parent_path() / (destination.filename().string() + ".pscript-atomic.tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "cannot write temporary file: " + temp.string();
            }
            return false;
        }
        out << content;
        out.flush();
        if (!out) {
            std::filesystem::remove(temp, ec);
            if (error) {
                *error = "cannot write temporary file: " + temp.string();
            }
            return false;
        }
    }
#ifdef _WIN32
    std::filesystem::remove(destination, ec);
    const std::wstring dest_wide = destination.wstring();
    const std::wstring temp_wide = temp.wstring();
    if (!MoveFileExW(temp_wide.c_str(), dest_wide.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(temp, ec);
        if (error) {
            *error = "cannot replace file: " + destination.string();
        }
        return false;
    }
#else
    std::filesystem::rename(temp, destination, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        if (error) {
            *error = "cannot replace file: " + destination.string() + " (" + ec.message() + ")";
        }
        return false;
    }
#endif
    return true;
}

}  // namespace pscript::package
