#pragma once

#include <string>
#include <string_view>

namespace pscript::lsp {

std::string uri_to_path(std::string_view uri);
std::string path_to_uri(const std::string& path);

/** True when uri/path refer to a normal on-disk PScript source file (.phs). */
bool is_pscript_document_uri(std::string_view uri, std::string_view path);

}  // namespace pscript::lsp
