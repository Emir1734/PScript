#pragma once

#include "parser/ast.h"

#include <string>
#include <vector>

namespace pscript {

enum class DiagnosticSeverity { Error, Warning, Info };

enum class DiagnosticCode {
    LexerError,
    ParserError,
    CompilerError,
    TypeError,
    NameError,
    UnusedSymbol,
    UnreachableCode,
    EmptyExpression,
    LintOther,
};

struct SourceRange {
    Location start{1, 1};
    Location end{1, 1};
};

struct Diagnostic {
    DiagnosticSeverity severity = DiagnosticSeverity::Error;
    DiagnosticCode code = DiagnosticCode::CompilerError;
    std::string message;
    std::string file;
    SourceRange range;
};

std::string diagnostic_severity_label(DiagnosticSeverity severity);
std::string diagnostic_code_label(DiagnosticCode code);
void format_diagnostic(const Diagnostic& diagnostic, std::ostream& out);

}  // namespace pscript
