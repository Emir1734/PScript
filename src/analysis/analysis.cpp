#include "analysis/query_api.h"

#include "analysis/format.h"
#include "analysis/linter.h"
#include "compiler/compiler.h"
#include "lexer/keywords.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "stdlib/stdlib.h"

#include <algorithm>
#include <optional>
#include <unordered_set>

namespace pscript {
namespace {

Diagnostic diagnostic_from_lexer(const LexerError& error, const std::string& file) {
    Diagnostic diagnostic;
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.code = DiagnosticCode::LexerError;
    diagnostic.message = error.message();
    diagnostic.file = file;
    diagnostic.range.start.line = error.line();
    diagnostic.range.start.column = error.column();
    diagnostic.range.end = diagnostic.range.start;
    return diagnostic;
}

Diagnostic diagnostic_from_parser(const ParserError& error, const std::string& file) {
    Diagnostic diagnostic;
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.code = DiagnosticCode::ParserError;
    diagnostic.message = error.message();
    diagnostic.file = file;
    diagnostic.range.start.line = error.line();
    diagnostic.range.start.column = error.column();
    diagnostic.range.end = diagnostic.range.start;
    return diagnostic;
}

Diagnostic diagnostic_from_compiler(const CompilerError& error, const std::string& file) {
    Diagnostic diagnostic;
    diagnostic.severity = DiagnosticSeverity::Error;
    diagnostic.code = DiagnosticCode::CompilerError;
    diagnostic.message = error.message();
    diagnostic.file = file;
    diagnostic.range.start.line = error.line();
    diagnostic.range.start.column = error.column();
    diagnostic.range.end = diagnostic.range.start;
    if (diagnostic.message.find("Type error") != std::string::npos) {
        diagnostic.code = DiagnosticCode::TypeError;
    } else if (diagnostic.message.find("undefined") != std::string::npos) {
        diagnostic.code = DiagnosticCode::NameError;
    }
    return diagnostic;
}

void trim_line_view(std::string_view& line) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1);
    }
}

std::string_view line_at(std::string_view source, int line_1based) {
    if (line_1based < 1) {
        return {};
    }
    int current = 1;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= source.size(); ++i) {
        if (i == source.size() || source[i] == '\n') {
            if (current == line_1based) {
                return source.substr(start, i - start);
            }
            ++current;
            start = i + 1;
        }
    }
    return {};
}

bool is_ascii_identifier_byte(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || (ch >= '0' && ch <= '9');
}

bool is_lenient_typing_line(std::string_view line) {
    trim_line_view(line);
    if (line.empty()) {
        return true;
    }
    if (line.find(' ') != std::string_view::npos || line.find('\t') != std::string_view::npos) {
        return false;
    }
    for (std::size_t i = 0; i < line.size();) {
        const unsigned char b = static_cast<unsigned char>(line[i]);
        if (b <= 0x7F) {
            if (is_ascii_identifier_byte(b)) {
                ++i;
                continue;
            }
            return false;
        }
        int nbytes = 0;
        if ((b & 0xE0) == 0xC0) {
            nbytes = 2;
        } else if ((b & 0xF0) == 0xE0) {
            nbytes = 3;
        } else if ((b & 0xF8) == 0xF0) {
            nbytes = 4;
        } else {
            return false;
        }
        if (i + static_cast<std::size_t>(nbytes) > line.size()) {
            return true;
        }
        for (int j = 1; j < nbytes; ++j) {
            const unsigned char cont = static_cast<unsigned char>(line[i + static_cast<std::size_t>(j)]);
            if ((cont & 0xC0) != 0x80) {
                return false;
            }
        }
        i += static_cast<std::size_t>(nbytes);
    }
    return true;
}

bool is_identifier_fragment_line(std::string_view line) {
    return is_lenient_typing_line(line);
}

bool source_has_incomplete_utf8_suffix(std::string_view source) {
    if (source.empty()) {
        return false;
    }
    std::size_t i = source.size();
    int trailing = 0;
    while (i > 0 && trailing < 4) {
        const unsigned char b = static_cast<unsigned char>(source[i - 1]);
        if ((b & 0xC0) == 0x80) {
            --i;
            ++trailing;
            continue;
        }
        break;
    }
    if (trailing == 0) {
        return false;
    }
    const std::string_view tail = source.substr(i);
    for (std::size_t j = 0; j < tail.size();) {
        const unsigned char b = static_cast<unsigned char>(tail[j]);
        int nbytes = 0;
        if (b <= 0x7F) {
            return false;
        }
        if ((b & 0xE0) == 0xC0) {
            nbytes = 2;
        } else if ((b & 0xF0) == 0xE0) {
            nbytes = 3;
        } else if ((b & 0xF8) == 0xF0) {
            nbytes = 4;
        } else {
            return false;
        }
        if (j + static_cast<std::size_t>(nbytes) > tail.size()) {
            return true;
        }
        j += static_cast<std::size_t>(nbytes);
    }
    return false;
}

bool line_starts_with_word(std::string_view line, std::string_view word) {
    if (line.size() < word.size()) {
        return false;
    }
    if (line.substr(0, word.size()) != word) {
        return false;
    }
    if (line.size() == word.size()) {
        return true;
    }
    const char next = line[word.size()];
    return next == ' ' || next == '(' || next == '"';
}

bool is_incomplete_print_line(std::string_view line) {
    trim_line_view(line);
    if (line.empty() || !line_starts_with_word(line, "print")) {
        return false;
    }
    if (line == "print" || line == "print ") {
        return true;
    }
    auto quote_parity_incomplete = [](std::string_view text) {
        int double_quotes = 0;
        int single_quotes = 0;
        for (char ch : text) {
            if (ch == '"') {
                ++double_quotes;
            } else if (ch == '\'') {
                ++single_quotes;
            }
        }
        return (double_quotes % 2) != 0 || (single_quotes % 2) != 0;
    };
    if (quote_parity_incomplete(line)) {
        return true;
    }
    if (line.find(':') != std::string_view::npos) {
        return false;
    }
    const bool has_string = line.find('"') != std::string_view::npos || line.find('\'') != std::string_view::npos;
    if (has_string) {
        return false;
    }
    if (line.find('(') != std::string_view::npos && line.find(')') == std::string_view::npos) {
        return true;
    }
    return line.size() <= 6;
}

bool is_incomplete_control_header_line(std::string_view line) {
    trim_line_view(line);
    if (line.empty()) {
        return false;
    }
    static constexpr std::string_view kHeaders[] = {"while", "if",   "for",  "with", "elif",
                                                      "elseif", "match", "async"};
    for (const std::string_view header : kHeaders) {
        if (!line_starts_with_word(line, header)) {
            continue;
        }
        if (line.find(':') != std::string_view::npos || line.find('{') != std::string_view::npos) {
            return false;
        }
        return true;
    }
    return false;
}

bool is_identifier_start_byte(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

bool is_incomplete_edit_line(std::string_view line) {
    trim_line_view(line);
    if (line.empty()) {
        return false;
    }
    if (is_lenient_typing_line(line)) {
        return true;
    }
    std::size_t i = 0;
    auto take_ident = [&]() {
        if (i >= line.size() || !is_identifier_start_byte(static_cast<unsigned char>(line[i]))) {
            return false;
        }
        ++i;
        while (i < line.size() && is_ascii_identifier_byte(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        return true;
    };
    if (!take_ident()) {
        return false;
    }
    while (i < line.size() && line[i] == '.') {
        ++i;
        if (i >= line.size()) {
            return true;
        }
        if (!take_ident()) {
            return false;
        }
    }
    return i == line.size();
}

std::string source_before_line(std::string_view source, int line_1based) {
    if (line_1based <= 1) {
        return {};
    }
    int line = 1;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '\n') {
            continue;
        }
        ++line;
        if (line == line_1based) {
            return std::string(source.substr(0, i + 1));
        }
    }
    return {};
}

bool should_suppress_parser_diagnostic(std::string_view source, const ParserError& error) {
    const std::string_view line = line_at(source, error.line());
    const std::string& msg = error.message();
    if (msg == "invalid statement" && is_identifier_fragment_line(line)) {
        return true;
    }
    if (msg == "invalid print arguments" && is_incomplete_print_line(line)) {
        return true;
    }
    if (msg == "missing condition" && is_incomplete_control_header_line(line)) {
        return true;
    }
    (void)source;
    return false;
}

bool should_suppress_lexer_diagnostic(std::string_view source, const LexerError& error) {
    const std::string& msg = error.message();
    const std::string_view line = line_at(source, error.line());
    if (msg.find("unterminated string") != std::string::npos) {
        return is_incomplete_print_line(line) || line.find('"') != std::string_view::npos ||
               line.find('\'') != std::string_view::npos;
    }
    if (msg == "invalid character") {
        if (is_lenient_typing_line(line) || source_has_incomplete_utf8_suffix(source)) {
            return true;
        }
    }
    return false;
}

std::string prepare_source(std::string source, const AnalysisOptions& options) {
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) {
        source.erase(0, 3);
    }
    if (options.inject_exception_prelude) {
        source = std::string(exception_prelude_source()) + source;
    }
    return source;
}

}  // namespace

AnalysisResult analyze_prepared_source(const std::string& prepared, const AnalysisOptions& options, int recover_depth);

AnalysisResult analyze_source(std::string source, const AnalysisOptions& options) {
    return analyze_prepared_source(prepare_source(std::move(source), options), options, 0);
}

AnalysisResult analyze_prepared_source(const std::string& prepared, const AnalysisOptions& options, int recover_depth) {
    AnalysisResult result;

    auto recover_incomplete = [&](int error_line) -> std::optional<AnalysisResult> {
        if (!options.lenient_incomplete_source || recover_depth >= 8 || error_line < 1) {
            return std::nullopt;
        }
        if (!is_incomplete_edit_line(line_at(prepared, error_line))) {
            return std::nullopt;
        }
        const std::string prefix = source_before_line(prepared, error_line);
        if (prefix.empty() || prefix == prepared) {
            return std::nullopt;
        }
        AnalysisResult recovered = analyze_prepared_source(prefix, options, recover_depth + 1);
        if (!recovered.parse_ok) {
            return std::nullopt;
        }
        return recovered;
    };

    try {
        Lexer lexer(prepared);
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        Program program = parser.parse();
        result.parse_ok = true;
        build_symbol_index(program, options.file, result.symbols);

        try {
            CompileOptions compile_options;
            compile_options.source_file = options.file;
            compile_options.source_line_base =
                options.inject_exception_prelude ? exception_prelude_line_count() : options.source_line_base;
            const Bytecode compiled = compile(program, compile_options);
            result.compile_ok = true;
            for (const CompileWarning& warning : compiled.warnings) {
                Diagnostic diagnostic;
                diagnostic.severity = DiagnosticSeverity::Warning;
                diagnostic.code = DiagnosticCode::LintOther;
                diagnostic.message = warning.message;
                diagnostic.file = options.file;
                diagnostic.range.start.line = warning.loc.line;
                diagnostic.range.start.column = warning.loc.column;
                diagnostic.range.end = diagnostic.range.start;
                result.diagnostics.push_back(std::move(diagnostic));
            }
        } catch (const CompilerError& error) {
            result.diagnostics.push_back(diagnostic_from_compiler(error, options.file));
        }

        if (options.run_linter) {
            std::vector<Diagnostic> lint = lint_program(program, options.file);
            result.diagnostics.insert(result.diagnostics.end(), lint.begin(), lint.end());
        }
    } catch (const LexerError& error) {
        if (const std::optional<AnalysisResult> recovered = recover_incomplete(error.line())) {
            return *recovered;
        }
        if (!(options.lenient_incomplete_source && should_suppress_lexer_diagnostic(prepared, error))) {
            result.diagnostics.push_back(diagnostic_from_lexer(error, options.file));
        }
    } catch (const ParserError& error) {
        if (const std::optional<AnalysisResult> recovered = recover_incomplete(error.line())) {
            return *recovered;
        }
        if (!(options.lenient_incomplete_source && should_suppress_parser_diagnostic(prepared, error))) {
            result.diagnostics.push_back(diagnostic_from_parser(error, options.file));
        }
    } catch (const std::exception& error) {
        if (options.lenient_incomplete_source) {
            return result;
        }
        Diagnostic diagnostic;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.code = DiagnosticCode::CompilerError;
        diagnostic.message = error.what();
        diagnostic.file = options.file;
        diagnostic.range.start.line = 1;
        diagnostic.range.start.column = 1;
        diagnostic.range.end = diagnostic.range.start;
        result.diagnostics.push_back(std::move(diagnostic));
    } catch (...) {
        if (options.lenient_incomplete_source) {
            return result;
        }
        Diagnostic diagnostic;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.code = DiagnosticCode::CompilerError;
        diagnostic.message = "internal analysis error";
        diagnostic.file = options.file;
        diagnostic.range.start.line = 1;
        diagnostic.range.start.column = 1;
        diagnostic.range.end = diagnostic.range.start;
        result.diagnostics.push_back(std::move(diagnostic));
    }

    return result;
}

AnalysisSession::AnalysisSession(AnalysisResult result) : result_(std::move(result)) {}

const SymbolRecord* AnalysisSession::goto_definition(const std::string& file, int line, int column) const {
    return result_.symbols.find_at_position(file, line, column);
}

std::vector<const SymbolRecord*> AnalysisSession::find_references(const SymbolRecord& symbol) const {
    return result_.symbols.find_by_name(symbol.name);
}

std::vector<const SymbolRecord*> AnalysisSession::completion_candidates(std::string_view prefix) const {
    std::vector<const SymbolRecord*> out;
    for (const SymbolRecord& symbol : result_.symbols.all()) {
        if (symbol.name.rfind(prefix, 0) == 0) {
            out.push_back(&symbol);
        }
    }
    return out;
}

namespace {

LspCompletionItemKind completion_kind_for_symbol(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Function:
            return LspCompletionItemKind::Function;
        case SymbolKind::Method:
            return LspCompletionItemKind::Method;
        case SymbolKind::Constructor:
            return LspCompletionItemKind::Constructor;
        case SymbolKind::Class:
            return LspCompletionItemKind::Class;
        case SymbolKind::Struct:
            return LspCompletionItemKind::Class;
        case SymbolKind::Enum:
            return LspCompletionItemKind::Enum;
        case SymbolKind::Interface:
            return LspCompletionItemKind::Interface;
        case SymbolKind::Property:
            return LspCompletionItemKind::Property;
        case SymbolKind::Parameter:
            return LspCompletionItemKind::Variable;
        case SymbolKind::Module:
            return LspCompletionItemKind::Module;
        default:
            return LspCompletionItemKind::Variable;
    }
}

/** Imported module bindings in normal identifier completion (not `import` module path). */
LspCompletionItemKind completion_kind_for_scope_binding(const SymbolRecord& symbol) {
    if (symbol.kind == SymbolKind::Module) {
        return LspCompletionItemKind::Variable;
    }
    return completion_kind_for_symbol(symbol.kind);
}

bool prefix_matches(std::string_view candidate, std::string_view prefix) {
    if (prefix.empty()) {
        return true;
    }
    if (candidate.size() < prefix.size()) {
        return false;
    }
    return candidate.substr(0, prefix.size()) == prefix;
}

bool keyword_prefix_relevant(std::string_view keyword, std::string_view prefix) {
    if (prefix.empty()) {
        return true;
    }
    if (!prefix_matches(keyword, prefix)) {
        return false;
    }
    if (prefix.size() < 3) {
        return true;
    }
    return keyword.size() - prefix.size() <= 1;
}

bool keyword_allowed_in_context(std::string_view keyword, const CompletionSiteContext& site) {
    if (!site.allow_statement_keywords) {
        if (keyword == "continue" || keyword == "break") {
            return site.in_loop;
        }
        if (keyword == "return") {
            return site.in_function;
        }
        return false;
    }
    if (keyword == "continue" || keyword == "break") {
        return site.in_loop;
    }
    if (keyword == "return") {
        return site.in_function;
    }
    return true;
}

bool top_level_completion_symbol(const SymbolRecord& symbol) {
    switch (symbol.kind) {
        case SymbolKind::Method:
        case SymbolKind::Constructor:
        case SymbolKind::Parameter:
        case SymbolKind::Property:
            return false;
        default:
            return true;
    }
}

bool member_completion_visible(const SymbolRecord& symbol) {
    return symbol.visibility != SymbolVisibility::Private;
}

std::optional<std::string> stdlib_module_specifier_for_binding_name(std::string_view binding_name,
                                                                    const SymbolIndex& symbols) {
    std::optional<std::string> from_import;
    for (const SymbolRecord& symbol : symbols.all()) {
        if (symbol.name != binding_name || symbol.kind != SymbolKind::Module) {
            continue;
        }
        from_import = symbol.qualified_name;
        break;
    }
    if (from_import.has_value() && is_stdlib_module_specifier(*from_import)) {
        return from_import;
    }
    const std::string binding(binding_name);
    if (is_stdlib_module_specifier(binding)) {
        return binding;
    }
    return std::nullopt;
}

LspCompletionItemKind completion_kind_for_stdlib_export(std::string_view export_name) {
    if (export_name == "pi" || export_name == "e") {
        return LspCompletionItemKind::Variable;
    }
    return LspCompletionItemKind::Function;
}

void push_stdlib_module_member_completions(std::string_view module_specifier, std::string_view prefix,
                                           const auto& push_item) {
    for (const std::string_view export_name : stdlib_module_export_names(std::string(module_specifier))) {
        if (!prefix_matches(export_name, prefix)) {
            continue;
        }
        LspCompletionItem item;
        item.label = std::string(export_name);
        item.kind = completion_kind_for_stdlib_export(export_name);
        item.detail = std::string(module_specifier) + " export";
        push_item(std::move(item));
    }
}

int completion_rank(const LspCompletionItem& item, std::string_view prefix) {
    int rank = 100;
    if (!prefix.empty() && item.label == prefix) {
        rank = 0;
    } else if (!prefix.empty() && item.label.rfind(prefix, 0) == 0) {
        rank = 10;
    }
    switch (item.kind) {
        case LspCompletionItemKind::Variable:
            rank += 1;
            break;
        case LspCompletionItemKind::Function:
        case LspCompletionItemKind::Method:
            rank += 2;
            break;
        case LspCompletionItemKind::Class:
        case LspCompletionItemKind::Module:
        case LspCompletionItemKind::Enum:
        case LspCompletionItemKind::Interface:
            rank += 3;
            break;
        case LspCompletionItemKind::Type:
            rank += 4;
            break;
        case LspCompletionItemKind::Keyword:
            rank += 20;
            break;
        default:
            rank += 8;
            break;
    }
    return rank;
}

}  // namespace

std::vector<LspCompletionItem> AnalysisSession::completion_items(
    std::string_view prefix, const MemberAccessCompletionContext* member_context,
    const CompletionSiteContext& site) const {
    std::vector<LspCompletionItem> out;
    std::unordered_set<std::string> seen;

    auto push_item = [&](LspCompletionItem item) {
        if (!seen.insert(item.label).second) {
            return;
        }
        out.push_back(std::move(item));
    };

    if (member_context != nullptr) {
        if (const std::optional<std::string> module_spec =
                stdlib_module_specifier_for_binding_name(member_context->receiver, result_.symbols)) {
            push_stdlib_module_member_completions(*module_spec, prefix, push_item);
            std::sort(out.begin(), out.end(), [&](const LspCompletionItem& a, const LspCompletionItem& b) {
                const int ra = completion_rank(a, prefix);
                const int rb = completion_rank(b, prefix);
                if (ra != rb) {
                    return ra < rb;
                }
                return a.label < b.label;
            });
            return out;
        }
        const std::vector<const SymbolRecord*> receivers = result_.symbols.find_by_name(member_context->receiver);
        const SymbolRecord* receiver = receivers.empty() ? nullptr : receivers.front();
        if (receiver != nullptr && receiver->kind != SymbolKind::Module) {
            for (const SymbolRecord& symbol : result_.symbols.all()) {
                if (symbol.parent_id != receiver->id) {
                    continue;
                }
                if (!member_completion_visible(symbol)) {
                    continue;
                }
                if (!prefix_matches(symbol.name, prefix)) {
                    continue;
                }
                LspCompletionItem item;
                item.label = symbol.name;
                item.kind = completion_kind_for_symbol(symbol.kind);
                item.detail = symbol.type_signature;
                push_item(std::move(item));
            }
        }
        for (const SymbolRecord& symbol : result_.symbols.all()) {
            if (!symbol.qualified_name.starts_with(member_context->receiver + ".")) {
                continue;
            }
            if (symbol.name == member_context->receiver) {
                continue;
            }
            if (!member_completion_visible(symbol)) {
                continue;
            }
            if (!prefix_matches(symbol.name, prefix)) {
                continue;
            }
            LspCompletionItem item;
            item.label = symbol.name;
            item.kind = completion_kind_for_symbol(symbol.kind);
            item.detail = symbol.type_signature;
            push_item(std::move(item));
        }
        std::sort(out.begin(), out.end(), [&](const LspCompletionItem& a, const LspCompletionItem& b) {
            const int ra = completion_rank(a, prefix);
            const int rb = completion_rank(b, prefix);
            if (ra != rb) {
                return ra < rb;
            }
            return a.label < b.label;
        });
        return out;
    }

    if (site.lexical == CompletionLexicalContext::StringLiteral ||
        site.lexical == CompletionLexicalContext::Comment) {
        return out;
    }

    if (site.lexical == CompletionLexicalContext::ImportModule ||
        site.lexical == CompletionLexicalContext::FromImportModule) {
        for (const std::string_view module : stdlib_module_specifiers()) {
            if (!prefix_matches(module, prefix)) {
                continue;
            }
            LspCompletionItem item;
            item.label = std::string(module);
            item.kind = LspCompletionItemKind::Module;
            item.detail = "stdlib module";
            push_item(std::move(item));
        }
        for (const SymbolRecord& symbol : result_.symbols.all()) {
            if (symbol.kind != SymbolKind::Module) {
                continue;
            }
            if (!prefix_matches(symbol.name, prefix)) {
                continue;
            }
            LspCompletionItem item;
            item.label = symbol.name;
            item.kind = LspCompletionItemKind::Module;
            push_item(std::move(item));
        }
        std::sort(out.begin(), out.end(), [&](const LspCompletionItem& a, const LspCompletionItem& b) {
            const int ra = completion_rank(a, prefix);
            const int rb = completion_rank(b, prefix);
            if (ra != rb) {
                return ra < rb;
            }
            return a.label < b.label;
        });
        return out;
    }

    if (site.lexical == CompletionLexicalContext::FromImportBinding) {
        for (const std::string_view export_name : stdlib_module_export_names(site.from_import_module)) {
            if (!prefix_matches(export_name, prefix)) {
                continue;
            }
            LspCompletionItem item;
            item.label = std::string(export_name);
            item.kind = LspCompletionItemKind::Variable;
            push_item(std::move(item));
        }
        std::sort(out.begin(), out.end(), [&](const LspCompletionItem& a, const LspCompletionItem& b) {
            const int ra = completion_rank(a, prefix);
            const int rb = completion_rank(b, prefix);
            if (ra != rb) {
                return ra < rb;
            }
            return a.label < b.label;
        });
        return out;
    }

    for (const std::string_view keyword : language_keywords()) {
        if (!keyword_prefix_relevant(keyword, prefix)) {
            continue;
        }
        if (!keyword_allowed_in_context(keyword, site)) {
            continue;
        }
        LspCompletionItem item;
        item.label = std::string(keyword);
        item.kind = LspCompletionItemKind::Keyword;
        push_item(std::move(item));
    }

    for (const std::string_view builtin : language_builtins()) {
        if (!prefix_matches(builtin, prefix)) {
            continue;
        }
        LspCompletionItem item;
        item.label = std::string(builtin);
        item.kind = LspCompletionItemKind::Function;
        push_item(std::move(item));
    }

    for (const std::string_view type_name : language_primitive_types()) {
        if (!prefix_matches(type_name, prefix)) {
            continue;
        }
        LspCompletionItem item;
        item.label = std::string(type_name);
        item.kind = LspCompletionItemKind::Type;
        push_item(std::move(item));
    }

    for (const SymbolRecord& symbol : result_.symbols.all()) {
        if (!top_level_completion_symbol(symbol)) {
            continue;
        }
        if (!prefix_matches(symbol.name, prefix)) {
            continue;
        }
        LspCompletionItem item;
        item.label = symbol.name;
        item.kind = completion_kind_for_scope_binding(symbol);
        if (symbol.kind == SymbolKind::Module) {
            item.detail = symbol.qualified_name;
        } else {
            item.detail = symbol.type_signature;
        }
        push_item(std::move(item));
    }

    std::sort(out.begin(), out.end(), [&](const LspCompletionItem& a, const LspCompletionItem& b) {
        const int ra = completion_rank(a, prefix);
        const int rb = completion_rank(b, prefix);
        if (ra != rb) {
            return ra < rb;
        }
        return a.label < b.label;
    });
    return out;
}

std::optional<std::string> AnalysisSession::hover_text(std::string_view source, const std::string& file, int line,
                                                     int column) const {
    const SymbolRecord* symbol = goto_definition(file, line, column);
    if (!symbol) {
        const std::optional<std::string> id = identifier_at_position(source, line, column);
        if (id.has_value()) {
            const std::vector<const SymbolRecord*> matches = result_.symbols.find_by_name(*id);
            if (!matches.empty()) {
                symbol = matches.front();
            }
        }
    }
    if (!symbol) {
        return std::nullopt;
    }
    std::string text = symbol->qualified_name;
    if (symbol->type_signature.has_value()) {
        text += ": " + *symbol->type_signature;
    }
    return text;
}

std::vector<LspLocation> AnalysisSession::references_in_source(std::string_view source, const std::string& file,
                                                             const std::string& uri, int line, int column) const {
    const SymbolRecord* symbol = goto_definition(file, line, column);
    if (symbol) {
        return find_identifier_occurrences(source, symbol->name, uri);
    }
    const std::optional<std::string> id = identifier_at_position(source, line, column);
    if (!id.has_value()) {
        return {};
    }
    return find_identifier_occurrences(source, *id, uri);
}

std::vector<LspTextEdit> AnalysisSession::rename_in_source(std::string_view source, int line, int column,
                                                           std::string_view new_name) const {
    return rename_identifier_at(source, line, column, new_name);
}

std::vector<LspSemanticToken> AnalysisSession::semantic_tokens(std::string_view source) const {
    return semantic_tokens_for_source(source, result_.symbols);
}

bool analysis_has_errors(const AnalysisResult& result) {
    for (const Diagnostic& diagnostic : result.diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) {
            return true;
        }
    }
    return !result.parse_ok || !result.compile_ok;
}

std::string format_source(std::string source, const AnalysisOptions& options) {
    const std::string prepared = prepare_source(std::move(source), options);
    Lexer lexer(prepared);
    std::vector<Token> tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    Program program = parser.parse();
    return format_source_layout(prepared, program);
}

}  // namespace pscript
