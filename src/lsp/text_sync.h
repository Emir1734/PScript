#pragma once

#include "lsp/json_util.h"

#include <string>
#include <string_view>

namespace pscript::lsp {

// Apply a single LSP TextDocumentContentChangeEvent to UTF-8 document text.
// Range positions use UTF-16 code units (LSP). When range is omitted, replaces the whole document.
std::string apply_text_document_content_change(std::string_view document, const JsonValue& change);

}  // namespace pscript::lsp
