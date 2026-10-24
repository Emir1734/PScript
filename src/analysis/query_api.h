#pragma once

#include "analysis/diagnostics.h"
#include "analysis/lsp_support.h"
#include "analysis/symbol_index.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pscript {

struct Program;

struct AnalysisResult {
    std::vector<Diagnostic> diagnostics;
    SymbolIndex symbols;
    bool parse_ok = false;
    bool compile_ok = false;
};

struct AnalysisOptions {
    std::string file = "<stdin>";
    bool inject_exception_prelude = true;
    int source_line_base = 0;
    bool run_linter = true;
    /** When true, suppress parser errors for obvious in-progress single-line fragments (LSP typing). */
    bool lenient_incomplete_source = false;
};

/** Values match LSP CompletionItemKind (1-based in JSON). */
enum class LspCompletionItemKind : int {
    Text = 1,
    Method = 2,
    Function = 3,
    Constructor = 4,
    Field = 5,
    Variable = 6,
    Class = 7,
    Interface = 8,
    Module = 9,
    Property = 10,
    Enum = 11,
    Keyword = 14,
    EnumMember = 22,
    Type = 25,
};

struct LspCompletionItem {
    std::string label;
    LspCompletionItemKind kind = LspCompletionItemKind::Text;
    std::optional<std::string> detail;
    std::optional<std::string> insert_text;
};

AnalysisResult analyze_source(std::string source, const AnalysisOptions& options = {});

std::string format_source(std::string source, const AnalysisOptions& options = {});

bool analysis_has_errors(const AnalysisResult& result);

class AnalysisSession {
public:
    explicit AnalysisSession(AnalysisResult result);

    const AnalysisResult& result() const { return result_; }
    const std::vector<Diagnostic>& diagnostics() const { return result_.diagnostics; }
    const SymbolIndex& symbols() const { return result_.symbols; }

    const SymbolRecord* goto_definition(const std::string& file, int line, int column) const;
    std::vector<const SymbolRecord*> find_references(const SymbolRecord& symbol) const;
    std::vector<const SymbolRecord*> completion_candidates(std::string_view prefix) const;
    std::vector<LspCompletionItem> completion_items(std::string_view prefix,
                                                    const MemberAccessCompletionContext* member_context,
                                                    const CompletionSiteContext& site) const;
    std::optional<std::string> hover_text(std::string_view source, const std::string& file, int line,
                                          int column) const;

    std::vector<LspLocation> references_in_source(std::string_view source, const std::string& file,
                                                  const std::string& uri, int line, int column) const;
    std::vector<LspTextEdit> rename_in_source(std::string_view source, int line, int column,
                                             std::string_view new_name) const;
    std::vector<LspSemanticToken> semantic_tokens(std::string_view source) const;

private:
    AnalysisResult result_;
};

}  // namespace pscript
