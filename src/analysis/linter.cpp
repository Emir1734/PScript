#include "analysis/linter.h"

#include <unordered_map>
#include <unordered_set>

namespace pscript {
namespace {

SourceRange range_from(const Location& loc) {
    SourceRange range;
    range.start = loc;
    range.end = loc;
    return range;
}

void collect_identifiers(const Expr& expr, std::unordered_set<std::string>& out);

void collect_identifiers(const Expr& expr, std::unordered_set<std::string>& out) {
    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(&expr)) {
        out.insert(ident->name);
        return;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (unary->operand) {
            collect_identifiers(*unary->operand, out);
        }
        return;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (binary->left) {
            collect_identifiers(*binary->left, out);
        }
        if (binary->right) {
            collect_identifiers(*binary->right, out);
        }
        return;
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        for (const auto& arg : call->arguments) {
            if (arg) {
                collect_identifiers(*arg, out);
            }
        }
        return;
    }
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expr)) {
        if (field->object) {
            collect_identifiers(*field->object, out);
        }
        return;
    }
    if (const auto* method = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (method->object) {
            collect_identifiers(*method->object, out);
        }
        for (const auto& arg : method->arguments) {
            if (arg) {
                collect_identifiers(*arg, out);
            }
        }
        return;
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr)) {
        if (index->object) {
            collect_identifiers(*index->object, out);
        }
        if (index->index) {
            collect_identifiers(*index->index, out);
        }
        return;
    }
    if (const auto* assign = dynamic_cast<const AssignExpr*>(&expr)) {
        if (assign->target) {
            collect_identifiers(*assign->target, out);
        }
        if (assign->value) {
            collect_identifiers(*assign->value, out);
        }
        return;
    }
    if (const auto* await_expr = dynamic_cast<const AwaitExpr*>(&expr)) {
        if (await_expr->operand) {
            collect_identifiers(*await_expr->operand, out);
        }
        return;
    }
    if (const auto* ternary = dynamic_cast<const TernaryExpr*>(&expr)) {
        if (ternary->condition) {
            collect_identifiers(*ternary->condition, out);
        }
        if (ternary->then_expr) {
            collect_identifiers(*ternary->then_expr, out);
        }
        if (ternary->else_expr) {
            collect_identifiers(*ternary->else_expr, out);
        }
        return;
    }
    if (const auto* list = dynamic_cast<const ListExpr*>(&expr)) {
        for (const auto& element : list->elements) {
            if (element) {
                collect_identifiers(*element, out);
            }
        }
        return;
    }
}

void collect_uses_stmt(const Stmt& stmt, std::unordered_set<std::string>& uses) {
    if (const auto* print_stmt = dynamic_cast<const PrintStmt*>(&stmt)) {
        for (const auto& expr : print_stmt->expressions) {
            if (expr) {
                collect_identifiers(*expr, uses);
            }
        }
        return;
    }
    if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(&stmt)) {
        if (expr_stmt->expression) {
            collect_identifiers(*expr_stmt->expression, uses);
        }
        return;
    }
    if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
        if (assign->value) {
            collect_identifiers(*assign->value, uses);
        }
        return;
    }
    if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
        if (ret->value) {
            collect_identifiers(*ret->value, uses);
        }
    }
}

void lint_block(const std::vector<std::unique_ptr<Stmt>>& body, const std::string& file,
                std::vector<Diagnostic>& diagnostics) {
    std::unordered_map<std::string, Location> locals;
    std::unordered_set<std::string> uses;
    bool terminated = false;
    for (const auto& stmt : body) {
        if (!stmt) {
            continue;
        }
        if (terminated) {
            Diagnostic diagnostic;
            diagnostic.severity = DiagnosticSeverity::Warning;
            diagnostic.code = DiagnosticCode::UnreachableCode;
            diagnostic.message = "unreachable code";
            diagnostic.file = file;
            diagnostic.range = range_from(stmt->loc);
            diagnostics.push_back(std::move(diagnostic));
        }
        if (dynamic_cast<const ReturnStmt*>(stmt.get()) != nullptr ||
            dynamic_cast<const ThrowStmt*>(stmt.get()) != nullptr ||
            dynamic_cast<const BreakStmt*>(stmt.get()) != nullptr ||
            dynamic_cast<const ContinueStmt*>(stmt.get()) != nullptr) {
            terminated = true;
        }
        if (const auto* assign = dynamic_cast<const AssignStmt*>(stmt.get())) {
            if (!assign->name.empty() && !locals.contains(assign->name)) {
                locals.emplace(assign->name, assign->loc);
            }
        }
        collect_uses_stmt(*stmt, uses);
        if (const auto* fn = dynamic_cast<const FuncStmt*>(stmt.get())) {
            lint_block(fn->body, file, diagnostics);
        }
        if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt.get())) {
            lint_block(if_stmt->body, file, diagnostics);
            for (const ElifBranch& branch : if_stmt->elif_branches) {
                lint_block(branch.body, file, diagnostics);
            }
            if (if_stmt->else_body.has_value()) {
                lint_block(*if_stmt->else_body, file, diagnostics);
            }
        }
        if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt.get())) {
            lint_block(while_stmt->body, file, diagnostics);
        }
        if (const auto* for_stmt = dynamic_cast<const ForStmt*>(stmt.get())) {
            lint_block(for_stmt->body, file, diagnostics);
        }
    }
    for (const auto& [name, loc] : locals) {
        if (!uses.contains(name)) {
            Diagnostic diagnostic;
            diagnostic.severity = DiagnosticSeverity::Warning;
            diagnostic.code = DiagnosticCode::UnusedSymbol;
            diagnostic.message = "unused variable '" + name + "'";
            diagnostic.file = file;
            diagnostic.range = range_from(loc);
            diagnostics.push_back(std::move(diagnostic));
        }
    }
}

}  // namespace

std::vector<Diagnostic> lint_program(const Program& program, const std::string& file) {
    std::vector<Diagnostic> diagnostics;
    lint_block(program.statements, file, diagnostics);
    for (const auto& stmt : program.statements) {
        if (!stmt) {
            continue;
        }
        if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt.get())) {
            if (!expr_stmt->expression) {
                Diagnostic diagnostic;
                diagnostic.severity = DiagnosticSeverity::Warning;
                diagnostic.code = DiagnosticCode::EmptyExpression;
                diagnostic.message = "empty expression statement";
                diagnostic.file = file;
                diagnostic.range = range_from(stmt->loc);
                diagnostics.push_back(std::move(diagnostic));
            }
        }
    }
    return diagnostics;
}

}  // namespace pscript
