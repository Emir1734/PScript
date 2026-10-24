#include "analysis/lsp_support.h"

#include "lexer/keywords.h"
#include "lexer/lexer.h"
#include "stdlib/stdlib.h"

#include <algorithm>

namespace pscript {

std::optional<MemberAccessCompletionContext> member_access_context_from_line_prefix(std::string_view line_prefix);

namespace {

std::vector<Token> tokenize_for_lsp(std::string_view source) {
    try {
        const std::string copy(source);
        Lexer lexer(copy);
        return lexer.tokenize();
    } catch (const LexerError&) {
        return {};
    }
}

LspPosition to_lsp_pos(int line_1based, int column_1based) {
    LspPosition pos;
    pos.line = std::max(0, line_1based - 1);
    pos.character = std::max(0, column_1based - 1);
    return pos;
}

bool position_in_token(int line_1based, int column_1based, const Token& token) {
    if (token.line != line_1based) {
        return false;
    }
    const int end_col = token.column + static_cast<int>(token.lexeme.size());
    return column_1based >= token.column && column_1based <= end_col;
}

std::optional<Token> find_token_at(std::string_view source, int line_1based, int column_1based) {
    const std::vector<Token> tokens = tokenize_for_lsp(source);
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::Eof) {
            break;
        }
        if (position_in_token(line_1based, column_1based, token)) {
            return token;
        }
    }
    return std::nullopt;
}

LspSemanticTokenType symbol_kind_to_semantic(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Class:
            return LspSemanticTokenType::Class;
        case SymbolKind::Struct:
            return LspSemanticTokenType::Struct;
        case SymbolKind::Enum:
            return LspSemanticTokenType::Enum;
        case SymbolKind::Interface:
            return LspSemanticTokenType::Interface;
        case SymbolKind::Function:
            return LspSemanticTokenType::Function;
        case SymbolKind::Method:
        case SymbolKind::Constructor:
            return LspSemanticTokenType::Method;
        case SymbolKind::Property:
            return LspSemanticTokenType::Property;
        case SymbolKind::Parameter:
            return LspSemanticTokenType::Parameter;
        case SymbolKind::Module:
            return LspSemanticTokenType::Namespace;
        default:
            return LspSemanticTokenType::Variable;
    }
}

const SymbolRecord* find_symbol_for_token(const SymbolIndex& symbols, int line_1based, int column_1based,
                                          std::string_view lexeme) {
    const SymbolRecord* best = nullptr;
    for (const SymbolRecord& symbol : symbols.all()) {
        if (symbol.name != lexeme) {
            continue;
        }
        if (symbol.range.start.line != line_1based) {
            continue;
        }
        const int start = symbol.range.start.column;
        const int end = start + static_cast<int>(symbol.name.size()) - 1;
        if (column_1based < start || column_1based > end) {
            continue;
        }
        if (!best || start >= best->range.start.column) {
            best = &symbol;
        }
    }
    return best;
}

bool is_language_builtin(std::string_view name) {
    for (const std::string_view builtin : language_builtins()) {
        if (builtin == name) {
            return true;
        }
    }
    return false;
}

bool is_language_primitive_type(std::string_view name) {
    for (const std::string_view type_name : language_primitive_types()) {
        if (type_name == name) {
            return true;
        }
    }
    return false;
}

bool type_annotation_precedes_identifier(std::string_view line_prefix) {
    while (!line_prefix.empty() && (line_prefix.back() == ' ' || line_prefix.back() == '\t')) {
        line_prefix.remove_suffix(1);
    }
    if (line_prefix.empty()) {
        return false;
    }
    const char last = line_prefix.back();
    return last == ':' || last == '|';
}

std::string_view line_at(std::string_view source, int line_1based) {
    if (line_1based < 1) {
        return {};
    }
    int line_index = 0;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i <= source.size(); ++i) {
        if (i == source.size() || source[i] == '\n') {
            ++line_index;
            if (line_index == line_1based) {
                return source.substr(line_start, i - line_start);
            }
            line_start = i + 1;
        }
    }
    return {};
}

std::string_view suffix_after_token_on_line(std::string_view source, const Token& token) {
    if (token.kind != TokenKind::Identifier) {
        return {};
    }
    const std::string_view line = line_at(source, token.line);
    const std::size_t col0 = static_cast<std::size_t>(token.column - 1);
    const std::string_view lex = token.lexeme;
    if (col0 + lex.size() > line.size() || line.substr(col0, lex.size()) != lex) {
        return {};
    }
    return line.substr(col0 + lex.size());
}

bool is_declarable_primitive_type_name(std::string_view lexeme) {
    return is_language_primitive_type(lexeme) || lexeme == "str";
}

bool is_type_cast_builtin_name(std::string_view lexeme) {
    return lexeme == "int" || lexeme == "float" || lexeme == "str" || lexeme == "bool";
}

/** `int x`, `string name`, `str c` declarations — not `int(...)` builtin calls. */
bool looks_like_typed_variable_declaration(std::string_view source, const Token& token) {
    if (token.kind != TokenKind::Identifier) {
        return false;
    }
    if (!is_declarable_primitive_type_name(token.lexeme)) {
        return false;
    }
    std::string_view rest = suffix_after_token_on_line(source, token);
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {
        rest.remove_prefix(1);
    }
    if (rest.empty() || rest[0] == '(') {
        return false;
    }
    const char ch = rest[0];
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

bool looks_like_type_cast_call(std::string_view source, const Token& token) {
    if (token.kind != TokenKind::Identifier || !is_type_cast_builtin_name(token.lexeme)) {
        return false;
    }
    std::string_view rest = suffix_after_token_on_line(source, token);
    while (!rest.empty() && (rest[0] == ' ' || rest[0] == '\t')) {
        rest.remove_prefix(1);
    }
    return !rest.empty() && rest[0] == '(';
}

std::optional<LspSemanticTokenType> semantic_type_for_token_kind(TokenKind kind) {
    switch (kind) {
        case TokenKind::True:
        case TokenKind::False:
        case TokenKind::Null:
        case TokenKind::Const:
        case TokenKind::If:
        case TokenKind::Elif:
        case TokenKind::ElseIf:
        case TokenKind::Else:
        case TokenKind::For:
        case TokenKind::With:
        case TokenKind::While:
        case TokenKind::In:
        case TokenKind::Break:
        case TokenKind::Continue:
        case TokenKind::Func:
        case TokenKind::Async:
        case TokenKind::Await:
        case TokenKind::Return:
        case TokenKind::Struct:
        case TokenKind::Record:
        case TokenKind::Enum:
        case TokenKind::Class:
        case TokenKind::Constructor:
        case TokenKind::This:
        case TokenKind::Super:
        case TokenKind::Parent:
        case TokenKind::Public:
        case TokenKind::Pub:
        case TokenKind::Private:
        case TokenKind::Priv:
        case TokenKind::Protected:
        case TokenKind::Prot:
        case TokenKind::Pass:
        case TokenKind::Import:
        case TokenKind::From:
        case TokenKind::As:
        case TokenKind::Match:
        case TokenKind::Try:
        case TokenKind::Catch:
        case TokenKind::Finally:
        case TokenKind::Throw:
        case TokenKind::Input:
        case TokenKind::And:
        case TokenKind::Or:
        case TokenKind::Not:
        case TokenKind::Interface:
        case TokenKind::Protocol:
            return LspSemanticTokenType::Keyword;
        case TokenKind::Integer:
        case TokenKind::Float:
        case TokenKind::Duration:
            return LspSemanticTokenType::Number;
        case TokenKind::String:
            return LspSemanticTokenType::String;
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Equal:
        case TokenKind::EqualEqual:
        case TokenKind::BangEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::AmpAmp:
        case TokenKind::PipePipe:
        case TokenKind::Bang:
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::Amp:
        case TokenKind::Pipe:
        case TokenKind::PipeGreater:
        case TokenKind::Caret:
        case TokenKind::Tilde:
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater:
        case TokenKind::MinusGreater:
        case TokenKind::At:
        case TokenKind::Ellipsis:
        case TokenKind::Question:
            return LspSemanticTokenType::Operator;
        default:
            return std::nullopt;
    }
}

}  // namespace

LspSemanticTokenType fallback_semantic_for_identifier(std::string_view lexeme, std::string_view line_prefix) {
    if (type_annotation_precedes_identifier(line_prefix) && is_language_primitive_type(lexeme)) {
        return LspSemanticTokenType::Type;
    }
    if (is_declarable_primitive_type_name(lexeme)) {
        return LspSemanticTokenType::Type;
    }
    if (is_language_builtin(lexeme)) {
        return LspSemanticTokenType::Function;
    }
    return LspSemanticTokenType::Variable;
}

LspRange source_range_to_lsp(const SourceRange& range) {
    LspRange out;
    out.start = to_lsp_pos(range.start.line, range.start.column);
    const int end_line = range.end.line > 0 ? range.end.line : range.start.line;
    const int end_col = range.end.column > 0 ? range.end.column : range.start.column;
    out.end = to_lsp_pos(end_line, end_col + 1);
    return out;
}

SourceRange lsp_range_to_source(const LspRange& range) {
    SourceRange out;
    out.start.line = range.start.line + 1;
    out.start.column = range.start.character + 1;
    out.end.line = range.end.line + 1;
    out.end.column = range.end.character + 1;
    return out;
}

std::optional<std::string> identifier_at_position(std::string_view source, int line_1based, int column_1based) {
    const std::optional<Token> token = find_token_at(source, line_1based, column_1based);
    if (!token.has_value() || token->kind != TokenKind::Identifier) {
        return std::nullopt;
    }
    return token->lexeme;
}

namespace {

bool token_is_completion_word(const Token& token) {
    if (token.kind == TokenKind::Identifier) {
        return true;
    }
    if (token.kind == TokenKind::Eof || token.kind == TokenKind::Integer || token.kind == TokenKind::Float ||
        token.kind == TokenKind::String) {
        return false;
    }
    switch (token.kind) {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Equal:
        case TokenKind::EqualEqual:
        case TokenKind::BangEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::AmpAmp:
        case TokenKind::PipePipe:
        case TokenKind::Bang:
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
        case TokenKind::PlusEqual:
        case TokenKind::MinusEqual:
        case TokenKind::StarEqual:
        case TokenKind::SlashEqual:
        case TokenKind::PercentEqual:
        case TokenKind::Amp:
        case TokenKind::Pipe:
        case TokenKind::PipeGreater:
        case TokenKind::Colon:
        case TokenKind::Comma:
        case TokenKind::Dot:
        case TokenKind::LeftParen:
        case TokenKind::RightParen:
        case TokenKind::LeftBrace:
        case TokenKind::RightBrace:
        case TokenKind::LeftBracket:
        case TokenKind::RightBracket:
        case TokenKind::MinusGreater:
        case TokenKind::Ellipsis:
        case TokenKind::Question:
        case TokenKind::At:
        case TokenKind::Caret:
        case TokenKind::Tilde:
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater:
        case TokenKind::Eof:
            return false;
        default:
            break;
    }
    return !token.lexeme.empty();
}

bool column_in_or_adjacent_word(const Token& token, int column_1based) {
    if (!token_is_completion_word(token)) {
        return false;
    }
    const int end_col = token.column + static_cast<int>(token.lexeme.size());
    return column_1based >= token.column && column_1based <= end_col;
}

int leading_indent(std::string_view line) {
    int indent = 0;
    for (char ch : line) {
        if (ch == ' ') {
            ++indent;
            continue;
        }
        if (ch == '\t') {
            indent += 4;
            continue;
        }
        break;
    }
    return indent;
}

bool is_ascii_identifier_char(char ch, bool allow_digit) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
        return true;
    }
    return allow_digit && ch >= '0' && ch <= '9';
}

std::string_view line_prefix_before_column(std::string_view source, int line_1based, int column_1based) {
    if (line_1based < 1 || column_1based < 1) {
        return {};
    }
    int line_index = 0;
    int line_start = 0;
    for (std::size_t i = 0; i <= source.size(); ++i) {
        if (i == source.size() || source[i] == '\n') {
            ++line_index;
            if (line_index == line_1based) {
                const int line_end = static_cast<int>(i);
                const int cursor = std::min(column_1based - 1, line_end - line_start);
                return source.substr(static_cast<std::size_t>(line_start), static_cast<std::size_t>(cursor));
            }
            line_start = static_cast<int>(i) + 1;
        }
    }
    return {};
}

bool cursor_in_string_literal(std::string_view source, int line_1based, int column_1based) {
    const std::vector<Token> tokens = tokenize_for_lsp(source);
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::String && position_in_token(line_1based, column_1based, token)) {
            return true;
        }
    }
    const std::string_view prefix = line_prefix_before_column(source, line_1based, column_1based);
    bool in_string = false;
    bool escaped = false;
    for (char ch : prefix) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
        }
    }
    return in_string;
}

bool cursor_in_line_comment(std::string_view source, int line_1based, int column_1based) {
    const std::string_view prefix = line_prefix_before_column(source, line_1based, column_1based);
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i] == '#') {
            return true;
        }
    }
    return false;
}

bool line_is_statement_start(std::string_view line_prefix) {
    std::size_t i = 0;
    while (i < line_prefix.size() && (line_prefix[i] == ' ' || line_prefix[i] == '\t')) {
        ++i;
    }
    if (i >= line_prefix.size()) {
        return true;
    }
    for (; i < line_prefix.size(); ++i) {
        const char ch = line_prefix[i];
        if (is_ascii_identifier_char(ch, i > 0 && line_prefix[i - 1] != ' ')) {
            continue;
        }
        if (ch == ' ' || ch == '\t') {
            continue;
        }
        return false;
    }
    return true;
}

void apply_import_lexical_context(std::string_view line_prefix, CompletionSiteContext& ctx) {
    std::string_view trimmed = line_prefix;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.remove_prefix(1);
    }
    if (trimmed.empty()) {
        return;
    }
    if (trimmed.starts_with("from ")) {
        trimmed.remove_prefix(5);
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
            trimmed.remove_prefix(1);
        }
        const std::size_t import_pos = trimmed.find(" import");
        if (import_pos != std::string_view::npos) {
            std::string_view module = trimmed.substr(0, import_pos);
            while (!module.empty() && (module.back() == ' ' || module.back() == '\t')) {
                module.remove_suffix(1);
            }
            ctx.lexical = CompletionLexicalContext::FromImportBinding;
            ctx.from_import_module = std::string(module);
            ctx.allow_statement_keywords = false;
            return;
        }
        ctx.lexical = CompletionLexicalContext::FromImportModule;
        ctx.allow_statement_keywords = false;
        return;
    }
    if (trimmed.starts_with("import ")) {
        ctx.lexical = CompletionLexicalContext::ImportModule;
        ctx.allow_statement_keywords = false;
        return;
    }
    if (trimmed == "import") {
        ctx.lexical = CompletionLexicalContext::ImportModule;
        ctx.allow_statement_keywords = false;
    }
}

void apply_lexical_completion_context(std::string_view source, int line_1based, int column_1based,
                                      CompletionSiteContext& ctx) {
    if (cursor_in_string_literal(source, line_1based, column_1based)) {
        ctx.lexical = CompletionLexicalContext::StringLiteral;
        ctx.allow_statement_keywords = false;
        return;
    }
    if (cursor_in_line_comment(source, line_1based, column_1based)) {
        ctx.lexical = CompletionLexicalContext::Comment;
        ctx.allow_statement_keywords = false;
        return;
    }
    const std::string_view line_prefix = line_prefix_before_column(source, line_1based, column_1based);
    apply_import_lexical_context(line_prefix, ctx);
    if (ctx.lexical != CompletionLexicalContext::Normal) {
        return;
    }
    ctx.allow_statement_keywords = line_is_statement_start(line_prefix);
}

bool line_starts_with_keyword(std::string_view trimmed, std::string_view keyword) {
    if (trimmed.size() < keyword.size()) {
        return false;
    }
    if (trimmed.substr(0, keyword.size()) != keyword) {
        return false;
    }
    if (trimmed.size() == keyword.size()) {
        return true;
    }
    const char next = trimmed[keyword.size()];
    return next == ' ' || next == '(' || next == ':' || next == '{';
}

}  // namespace

std::optional<WordPrefixAtPosition> word_prefix_at_position(std::string_view source, int line_1based,
                                                            int column_1based) {
    if (line_1based < 1 || column_1based < 1) {
        return std::nullopt;
    }
    if (const std::optional<Token> token = find_token_at(source, line_1based, column_1based); token.has_value()) {
        if (column_in_or_adjacent_word(*token, column_1based)) {
            WordPrefixAtPosition out;
            out.prefix = token->lexeme;
            out.replace_range.start = to_lsp_pos(token->line, token->column);
            out.replace_range.end.line = out.replace_range.start.line;
            out.replace_range.end.character =
                out.replace_range.start.character + static_cast<int>(token->lexeme.size());
            return out;
        }
    }
    {
        const std::vector<Token> tokens = tokenize_for_lsp(source);
        for (const Token& token : tokens) {
            if (token.kind == TokenKind::Eof) {
                break;
            }
            if (!token_is_completion_word(token)) {
                continue;
            }
            if (!column_in_or_adjacent_word(token, column_1based)) {
                continue;
            }
            WordPrefixAtPosition out;
            out.prefix = token.lexeme;
            out.replace_range.start = to_lsp_pos(token.line, token.column);
            out.replace_range.end.line = out.replace_range.start.line;
            out.replace_range.end.character =
                out.replace_range.start.character + static_cast<int>(token.lexeme.size());
            return out;
        }
    }
    int line_index = 0;
    int line_start = 0;
    for (std::size_t i = 0; i <= source.size(); ++i) {
        if (i == source.size() || source[i] == '\n') {
            ++line_index;
            if (line_index == line_1based) {
                const int line_end = static_cast<int>(i);
                const int cursor = std::min(column_1based - 1, line_end - line_start);
                int start = line_start + cursor - 1;
                while (start >= line_start) {
                    const char ch = source[static_cast<std::size_t>(start)];
                    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' ||
                        (start < line_start + cursor - 1 && ch >= '0' && ch <= '9')) {
                        --start;
                        continue;
                    }
                    break;
                }
                const int prefix_start = start + 1;
                const int prefix_end = line_start + cursor;
                if (prefix_end <= prefix_start) {
                    WordPrefixAtPosition empty;
                    empty.prefix = {};
                    empty.replace_range.start = to_lsp_pos(line_1based, column_1based);
                    empty.replace_range.end = empty.replace_range.start;
                    return empty;
                }
                WordPrefixAtPosition out;
                out.prefix = std::string(source.substr(static_cast<std::size_t>(prefix_start),
                                                      static_cast<std::size_t>(prefix_end - prefix_start)));
                out.replace_range.start = to_lsp_pos(line_1based, prefix_start - line_start + 1);
                out.replace_range.end = to_lsp_pos(line_1based, prefix_end - line_start + 1);
                return out;
            }
            line_start = static_cast<int>(i) + 1;
        }
    }
    return std::nullopt;
}

CompletionSiteContext completion_site_at(std::string_view source, int line_1based, int column_1based) {
    CompletionSiteContext ctx;
    if (line_1based < 1) {
        return ctx;
    }
    int line_index = 0;
    int line_start = 0;
    std::vector<int> indents;
    std::vector<bool> loop_levels;
    std::vector<bool> function_levels;
    for (std::size_t i = 0; i <= source.size(); ++i) {
        if (i != source.size() && source[i] != '\n') {
            continue;
        }
        ++line_index;
        const std::string_view line = source.substr(line_start, i - static_cast<std::size_t>(line_start));
        const int indent = leading_indent(line);
        std::string_view trimmed = line;
        while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
            trimmed.remove_prefix(1);
        }
        while (!indents.empty() && indent < indents.back()) {
            indents.pop_back();
            loop_levels.pop_back();
            function_levels.pop_back();
        }
        if (!trimmed.empty() && trimmed.front() != '#') {
            if (line_starts_with_keyword(trimmed, "while") || line_starts_with_keyword(trimmed, "for") ||
                line_starts_with_keyword(trimmed, "with")) {
                indents.push_back(indent);
                loop_levels.push_back(true);
                function_levels.push_back(function_levels.empty() ? false : function_levels.back());
            } else if (line_starts_with_keyword(trimmed, "func") || line_starts_with_keyword(trimmed, "async")) {
                indents.push_back(indent);
                loop_levels.push_back(loop_levels.empty() ? false : loop_levels.back());
                function_levels.push_back(true);
            } else if (!indents.empty() && indent > indents.back()) {
                indents.push_back(indent);
                loop_levels.push_back(loop_levels.back());
                function_levels.push_back(function_levels.back());
            }
        }
        if (line_index == line_1based) {
            ctx.in_loop = !loop_levels.empty() && loop_levels.back();
            ctx.in_function = !function_levels.empty() && function_levels.back();
            apply_lexical_completion_context(source, line_1based, column_1based, ctx);
            break;
        }
        line_start = static_cast<int>(i) + 1;
    }
    return ctx;
}

bool is_identifier_char(char ch, bool allow_digit) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_') {
        return true;
    }
    return allow_digit && ch >= '0' && ch <= '9';
}

std::optional<MemberAccessCompletionContext> member_access_context_from_line_prefix(std::string_view line_prefix) {
    if (line_prefix.empty()) {
        return std::nullopt;
    }
    int end = static_cast<int>(line_prefix.size());
    while (end > 0 && (line_prefix[static_cast<std::size_t>(end - 1)] == ' ' ||
                       line_prefix[static_cast<std::size_t>(end - 1)] == '\t')) {
        --end;
    }
    if (end <= 0) {
        return std::nullopt;
    }
    int pos = end - 1;
    if (is_identifier_char(line_prefix[static_cast<std::size_t>(pos)], true)) {
        while (pos >= 0 && is_identifier_char(line_prefix[static_cast<std::size_t>(pos)], pos > 0)) {
            --pos;
        }
    } else if (line_prefix[static_cast<std::size_t>(pos)] != '.') {
        return std::nullopt;
    }
    if (pos < 0 || line_prefix[static_cast<std::size_t>(pos)] != '.') {
        return std::nullopt;
    }
    --pos;
    while (pos >= 0 && (line_prefix[static_cast<std::size_t>(pos)] == ' ' ||
                        line_prefix[static_cast<std::size_t>(pos)] == '\t')) {
        --pos;
    }
    const int receiver_end = pos + 1;
    while (pos >= 0 && is_identifier_char(line_prefix[static_cast<std::size_t>(pos)], pos > 0)) {
        --pos;
    }
    const int receiver_start = pos + 1;
    if (receiver_start >= receiver_end) {
        return std::nullopt;
    }
    MemberAccessCompletionContext ctx;
    ctx.receiver = std::string(
        line_prefix.substr(static_cast<std::size_t>(receiver_start), static_cast<std::size_t>(receiver_end - receiver_start)));
    return ctx;
}

std::optional<MemberAccessCompletionContext> member_access_context_at(std::string_view source, int line_1based,
                                                                       int column_1based) {
    if (line_1based < 1 || column_1based < 1) {
        return std::nullopt;
    }
    const std::string_view line_prefix = line_prefix_before_column(source, line_1based, column_1based);
    return member_access_context_from_line_prefix(line_prefix);
}

std::string member_access_name_prefix(std::string_view source, int line_1based, int column_1based) {
    if (!member_access_context_at(source, line_1based, column_1based).has_value()) {
        return {};
    }
    const std::string_view line_prefix = line_prefix_before_column(source, line_1based, column_1based);
    const std::size_t dot = line_prefix.rfind('.');
    if (dot == std::string_view::npos) {
        return {};
    }
    std::size_t start = dot + 1;
    while (start < line_prefix.size() && (line_prefix[start] == ' ' || line_prefix[start] == '\t')) {
        ++start;
    }
    if (start >= line_prefix.size()) {
        return {};
    }
    return std::string(line_prefix.substr(start));
}

std::vector<LspLocation> find_identifier_occurrences(std::string_view source, std::string_view identifier,
                                                     const std::string& uri) {
    std::vector<LspLocation> out;
    if (identifier.empty()) {
        return out;
    }
    const std::vector<Token> tokens = tokenize_for_lsp(source);
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::Identifier && token.lexeme == identifier) {
            LspLocation loc;
            loc.uri = uri;
            loc.range.start = to_lsp_pos(token.line, token.column);
            loc.range.end.line = loc.range.start.line;
            loc.range.end.character = loc.range.start.character + static_cast<int>(token.lexeme.size());
            out.push_back(std::move(loc));
        }
    }
    return out;
}

std::vector<LspTextEdit> rename_identifier_at(std::string_view source, int line_1based, int column_1based,
                                              std::string_view new_name) {
    const std::optional<std::string> id = identifier_at_position(source, line_1based, column_1based);
    if (!id.has_value()) {
        return {};
    }
    std::vector<LspTextEdit> edits;
    const std::vector<Token> tokens = tokenize_for_lsp(source);
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::Identifier && token.lexeme == *id) {
            LspTextEdit edit;
            edit.range.start = to_lsp_pos(token.line, token.column);
            edit.range.end.line = edit.range.start.line;
            edit.range.end.character = edit.range.start.character + static_cast<int>(token.lexeme.size());
            edit.new_text = std::string(new_name);
            edits.push_back(std::move(edit));
        }
    }
    return edits;
}

bool is_scope_binding(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Method:
        case SymbolKind::Constructor:
        case SymbolKind::Property:
        case SymbolKind::Import:
            return false;
        default:
            return true;
    }
}

bool binding_precedes_token(const SymbolRecord& symbol, int line_1based, int column_1based) {
    if (symbol.range.start.line < line_1based) {
        return true;
    }
    if (symbol.range.start.line > line_1based) {
        return false;
    }
    return symbol.range.start.column <= column_1based;
}

const SymbolRecord* nearest_scope_binding(const SymbolIndex& symbols, std::string_view lexeme, int line_1based,
                                          int column_1based) {
    const SymbolRecord* best = nullptr;
    for (const SymbolRecord& symbol : symbols.all()) {
        if (symbol.name != lexeme || !is_scope_binding(symbol.kind)) {
            continue;
        }
        if (!binding_precedes_token(symbol, line_1based, column_1based)) {
            continue;
        }
        if (best == nullptr || symbol.range.start.line > best->range.start.line ||
            (symbol.range.start.line == best->range.start.line &&
             symbol.range.start.column >= best->range.start.column)) {
            best = &symbol;
        }
    }
    return best;
}

const SymbolRecord* member_symbol_on_receiver(const SymbolIndex& symbols, std::string_view receiver,
                                              std::string_view member) {
    for (const SymbolRecord* owner : symbols.find_by_name(receiver)) {
        if (owner == nullptr || owner->kind == SymbolKind::Module) {
            continue;
        }
        for (const SymbolRecord& symbol : symbols.all()) {
            if (symbol.parent_id == owner->id && symbol.name == member) {
                return &symbol;
            }
        }
    }
    return nullptr;
}

std::optional<std::string> module_specifier_for_receiver(std::string_view receiver, const SymbolIndex& symbols) {
    for (const SymbolRecord& symbol : symbols.all()) {
        if (symbol.kind == SymbolKind::Module && symbol.name == receiver && !symbol.qualified_name.empty()) {
            return symbol.qualified_name;
        }
    }
    const std::string binding(receiver);
    if (is_stdlib_module_specifier(binding)) {
        return binding;
    }
    return std::nullopt;
}

LspSemanticTokenType classify_member(const SymbolIndex& symbols, std::string_view receiver, std::string_view member) {
    if (const SymbolRecord* child = member_symbol_on_receiver(symbols, receiver, member)) {
        return symbol_kind_to_semantic(child->kind);
    }
    if (const std::optional<std::string> module = module_specifier_for_receiver(receiver, symbols)) {
        for (const std::string_view export_name : stdlib_module_export_names(*module)) {
            if (export_name != member) {
                continue;
            }
            if (export_name == "pi" || export_name == "e") {
                return LspSemanticTokenType::Variable;
            }
            return LspSemanticTokenType::Function;
        }
    }
    return LspSemanticTokenType::Property;
}

LspSemanticTokenType classify_identifier(const SymbolIndex& symbols, const Token& token, std::string_view line_prefix,
                                         std::string_view source) {
    if (const std::optional<MemberAccessCompletionContext> member = member_access_context_from_line_prefix(line_prefix)) {
        return classify_member(symbols, member->receiver, token.lexeme);
    }
    if (const SymbolRecord* at = find_symbol_for_token(symbols, token.line, token.column, token.lexeme)) {
        return symbol_kind_to_semantic(at->kind);
    }
    if (const SymbolRecord* binding = nearest_scope_binding(symbols, token.lexeme, token.line, token.column)) {
        return symbol_kind_to_semantic(binding->kind);
    }
    if (looks_like_typed_variable_declaration(source, token)) {
        return LspSemanticTokenType::Type;
    }
    if (type_annotation_precedes_identifier(line_prefix) && is_language_primitive_type(token.lexeme)) {
        return LspSemanticTokenType::Type;
    }
    if (looks_like_type_cast_call(source, token)) {
        return LspSemanticTokenType::Function;
    }
    if (is_declarable_primitive_type_name(token.lexeme)) {
        return LspSemanticTokenType::Type;
    }
    if (is_language_builtin(token.lexeme)) {
        return LspSemanticTokenType::Function;
    }
    if (is_language_primitive_type(token.lexeme)) {
        return LspSemanticTokenType::Type;
    }
    return LspSemanticTokenType::Variable;
}

std::vector<LspSemanticToken> semantic_tokens_for_source(std::string_view source, const SymbolIndex& symbols) {
    std::vector<LspSemanticToken> out;
    const std::vector<Token> tokens = tokenize_for_lsp(source);
    for (const Token& token : tokens) {
        if (token.kind == TokenKind::Eof) {
            break;
        }
        LspSemanticToken semantic;
        semantic.line = token.line - 1;
        semantic.start_char = token.column - 1;
        semantic.length = static_cast<int>(token.lexeme.size());
        if (token.kind == TokenKind::Identifier) {
            const std::string_view line_prefix = line_prefix_before_column(source, token.line, token.column);
            semantic.type = classify_identifier(symbols, token, line_prefix, source);
            out.push_back(semantic);
            continue;
        }
        if (const std::optional<LspSemanticTokenType> mapped = semantic_type_for_token_kind(token.kind)) {
            semantic.type = *mapped;
            out.push_back(semantic);
        }
    }
    std::sort(out.begin(), out.end(), [](const LspSemanticToken& a, const LspSemanticToken& b) {
        if (a.line != b.line) {
            return a.line < b.line;
        }
        return a.start_char < b.start_char;
    });
    return out;
}

}  // namespace pscript
