#pragma once

#include "analysis/diagnostics.h"
#include "analysis/symbol_index.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {

struct LspPosition {
    int line = 0;
    int character = 0;
};

struct LspRange {
    LspPosition start;
    LspPosition end;
};

struct LspLocation {
    std::string uri;
    LspRange range;
};

struct LspTextEdit {
    LspRange range;
    std::string new_text;
};

enum class LspSemanticTokenType : std::uint8_t {
    Namespace = 0,
    Type,
    Class,
    Enum,
    Interface,
    Struct,
    TypeParameter,
    Parameter,
    Variable,
    Property,
    EnumMember,
    Function,
    Method,
    Keyword,
    Modifier,
    Comment,
    String,
    Number,
    Operator,
};

struct LspSemanticToken {
    int line = 0;
    int start_char = 0;
    int length = 0;
    LspSemanticTokenType type = LspSemanticTokenType::Variable;
    std::uint32_t modifiers = 0;
};

std::optional<std::string> identifier_at_position(std::string_view source, int line_1based, int column_1based);

struct WordPrefixAtPosition {
    std::string prefix;
    LspRange replace_range;
};

std::optional<WordPrefixAtPosition> word_prefix_at_position(std::string_view source, int line_1based,
                                                            int column_1based);

struct MemberAccessCompletionContext {
    std::string receiver;
};

enum class CompletionLexicalContext {
    Normal,
    ImportModule,
    FromImportModule,
    FromImportBinding,
    StringLiteral,
    Comment,
};

struct CompletionSiteContext {
    bool in_loop = false;
    bool in_function = false;
    CompletionLexicalContext lexical = CompletionLexicalContext::Normal;
    /** Module name parsed from `from <module> import ...` when lexical is FromImportBinding. */
    std::string from_import_module;
    /** When false, suppress statement keywords (if/while/match/...) in expression/identifier contexts. */
    bool allow_statement_keywords = true;
};

CompletionSiteContext completion_site_at(std::string_view source, int line_1based, int column_1based);

std::optional<MemberAccessCompletionContext> member_access_context_at(std::string_view source, int line_1based,
                                                                       int column_1based);

/** Partial identifier typed after the `.` in a member access, not including the receiver name. */
std::string member_access_name_prefix(std::string_view source, int line_1based, int column_1based);

std::vector<LspLocation> find_identifier_occurrences(std::string_view source, std::string_view identifier,
                                                       const std::string& uri);

std::vector<LspTextEdit> rename_identifier_at(std::string_view source, int line_1based, int column_1based,
                                                std::string_view new_name);

std::vector<LspSemanticToken> semantic_tokens_for_source(std::string_view source, const SymbolIndex& symbols);

/** Builtin/type fallback classification for tests (no symbol at position). */
LspSemanticTokenType fallback_semantic_for_identifier(std::string_view lexeme, std::string_view line_prefix);

LspRange source_range_to_lsp(const SourceRange& range);
SourceRange lsp_range_to_source(const LspRange& range);

}  // namespace pscript
