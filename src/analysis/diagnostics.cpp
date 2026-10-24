#include "analysis/diagnostics.h"

#include <ostream>

namespace pscript {

std::string diagnostic_severity_label(DiagnosticSeverity severity) {
    switch (severity) {
        case DiagnosticSeverity::Error:
            return "error";
        case DiagnosticSeverity::Warning:
            return "warning";
        case DiagnosticSeverity::Info:
            return "info";
    }
    return "error";
}

std::string diagnostic_code_label(DiagnosticCode code) {
    switch (code) {
        case DiagnosticCode::LexerError:
            return "lexer";
        case DiagnosticCode::ParserError:
            return "parser";
        case DiagnosticCode::CompilerError:
            return "compile";
        case DiagnosticCode::TypeError:
            return "type";
        case DiagnosticCode::NameError:
            return "name";
        case DiagnosticCode::UnusedSymbol:
            return "unused";
        case DiagnosticCode::UnreachableCode:
            return "unreachable";
        case DiagnosticCode::EmptyExpression:
            return "empty-expr";
        case DiagnosticCode::LintOther:
            return "lint";
    }
    return "unknown";
}

void format_diagnostic(const Diagnostic& diagnostic, std::ostream& out) {
    out << diagnostic.file << ':' << diagnostic.range.start.line << ':' << diagnostic.range.start.column << ": "
        << diagnostic_severity_label(diagnostic.severity) << " [" << diagnostic_code_label(diagnostic.code) << "]: "
        << diagnostic.message << '\n';
}

}  // namespace pscript
