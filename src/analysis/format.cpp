#include "analysis/format.h"

#include <sstream>
#include <unordered_map>
#include <vector>

namespace pscript {
namespace {

constexpr int kIndentSpaces = 4;

std::string trim_right(std::string line) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.pop_back();
    }
    return line;
}

std::string trim_left(std::string line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
    }
    return line.substr(i);
}

void assign_indent(std::unordered_map<int, int>& line_indent, int line, int indent) {
    if (line <= 0) {
        return;
    }
    const auto found = line_indent.find(line);
    if (found == line_indent.end() || indent > found->second) {
        line_indent[line] = indent;
    }
}

void plan_block_indents(const std::vector<std::unique_ptr<Stmt>>& body, int depth,
                        std::unordered_map<int, int>& line_indent);

void plan_stmt_indents(const Stmt& stmt, int depth, std::unordered_map<int, int>& line_indent) {
    assign_indent(line_indent, stmt.loc.line, depth * kIndentSpaces);
    if (const auto* fn = dynamic_cast<const FuncStmt*>(&stmt)) {
        plan_block_indents(fn->body, depth + 1, line_indent);
        return;
    }
    if (const auto* if_stmt = dynamic_cast<const IfStmt*>(&stmt)) {
        plan_block_indents(if_stmt->body, depth + 1, line_indent);
        for (const ElifBranch& branch : if_stmt->elif_branches) {
            assign_indent(line_indent, branch.loc.line, depth);
            plan_block_indents(branch.body, depth + 1, line_indent);
        }
        if (if_stmt->else_body.has_value()) {
            plan_block_indents(*if_stmt->else_body, depth + 1, line_indent);
        }
        return;
    }
    if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(&stmt)) {
        plan_block_indents(while_stmt->body, depth + 1, line_indent);
        return;
    }
    if (const auto* for_stmt = dynamic_cast<const ForStmt*>(&stmt)) {
        plan_block_indents(for_stmt->body, depth + 1, line_indent);
        return;
    }
    if (const auto* async_for = dynamic_cast<const AsyncForStmt*>(&stmt)) {
        plan_block_indents(async_for->body, depth + 1, line_indent);
        return;
    }
    if (const auto* async_with = dynamic_cast<const AsyncWithStmt*>(&stmt)) {
        plan_block_indents(async_with->body, depth + 1, line_indent);
        return;
    }
    if (const auto* try_stmt = dynamic_cast<const TryStmt*>(&stmt)) {
        plan_block_indents(try_stmt->try_body, depth + 1, line_indent);
        for (const CatchClause& clause : try_stmt->catches) {
            assign_indent(line_indent, clause.loc.line, depth);
            plan_block_indents(clause.body, depth + 1, line_indent);
        }
        if (try_stmt->finally_body.has_value()) {
            plan_block_indents(*try_stmt->finally_body, depth + 1, line_indent);
        }
        return;
    }
    if (const auto* class_stmt = dynamic_cast<const ClassStmt*>(&stmt)) {
        if (class_stmt->constructor) {
            plan_block_indents(class_stmt->constructor->body, depth + 1, line_indent);
        }
        for (const auto& method : class_stmt->methods) {
            if (method) {
                plan_stmt_indents(*method, depth + 1, line_indent);
            }
        }
    }
}

void plan_block_indents(const std::vector<std::unique_ptr<Stmt>>& body, int depth,
                        std::unordered_map<int, int>& line_indent) {
    for (const auto& stmt : body) {
        if (!stmt) {
            continue;
        }
        plan_stmt_indents(*stmt, depth, line_indent);
    }
}

}  // namespace

std::string format_source_layout(const std::string& source, const Program& program) {
    std::unordered_map<int, int> line_indent;
    plan_block_indents(program.statements, 0, line_indent);

    std::istringstream in(source);
    std::ostringstream out;
    std::string line;
    int line_number = 1;
    while (std::getline(in, line)) {
        const std::string trimmed = trim_right(line);
        const std::string content = trim_left(trimmed);
        if (!content.empty()) {
            const auto found = line_indent.find(line_number);
            const int indent = found != line_indent.end() ? found->second : 0;
            out << std::string(static_cast<std::size_t>(indent), ' ') << content;
        }
        out << '\n';
        ++line_number;
    }
    return out.str();
}

}  // namespace pscript
