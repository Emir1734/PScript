#include "parser/parser.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using pscript::AssignExpr;
using pscript::MatchPatternKind;
using pscript::AssignStmt;
using pscript::BinaryExpr;
using pscript::BreakStmt;
using pscript::CallExpr;
using pscript::ContinueStmt;
using pscript::DestructPatternKind;
using pscript::DestructureAssignStmt;
using pscript::DictExpr;
using pscript::ElifBranch;
using pscript::ExprStmt;
using pscript::ForStmt;
using pscript::FuncStmt;
using pscript::IdentifierExpr;
using pscript::IfStmt;
using pscript::ImportStmt;
using pscript::IndexAssignStmt;
using pscript::IndexExpr;
using pscript::ListExpr;
using pscript::LiteralExpr;
using pscript::MatchStmt;
using pscript::MembershipExpr;
using pscript::MethodCallExpr;
using pscript::parse;
using pscript::ParserError;
using pscript::PrintStmt;
using pscript::Program;
using pscript::ReturnStmt;
using pscript::SetExpr;
using pscript::StructStmt;
using pscript::ClassStmt;
using pscript::EnumStmt;
using pscript::FieldAccessExpr;
using pscript::MemberAccess;
using pscript::PassStmt;
using pscript::TernaryExpr;
using pscript::TokenKind;
using pscript::TupleExpr;
using pscript::TypeAnnotationKind;
using pscript::UnaryExpr;
using pscript::UpdateExpr;
using pscript::WhileStmt;
using pscript::TryStmt;
using pscript::ThrowStmt;
using pscript::AwaitExpr;

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

template <typename T>
const T* as(const pscript::Expr* expr) {
    return dynamic_cast<const T*>(expr);
}

template <typename T>
const T* as_stmt(const pscript::Stmt* stmt) {
    return dynamic_cast<const T*>(stmt);
}

const AssignStmt* only_assign(const Program& program) {
    if (program.statements.size() != 1) {
        return nullptr;
    }
    return as_stmt<AssignStmt>(program.statements[0].get());
}

const PrintStmt* only_print(const Program& program) {
    if (program.statements.size() != 1) {
        return nullptr;
    }
    return as_stmt<PrintStmt>(program.statements[0].get());
}

const IfStmt* only_if(const Program& program) {
    if (program.statements.size() != 1) {
        return nullptr;
    }
    return as_stmt<IfStmt>(program.statements[0].get());
}

const IdentifierExpr* ident(const pscript::Expr* expr) {
    return as<IdentifierExpr>(expr);
}

const BinaryExpr* comparison(const pscript::Expr* expr, TokenKind op) {
    const auto* bin = as<BinaryExpr>(expr);
    if (!bin || bin->op != op) {
        return nullptr;
    }
    return bin;
}

bool print_string(const pscript::Stmt* stmt, std::string_view lexeme) {
    const auto* print = as_stmt<PrintStmt>(stmt);
    if (!print || print->expressions.size() != 1) {
        return false;
    }
    const auto* lit = as<LiteralExpr>(print->expressions[0].get());
    return lit && lit->lexeme == lexeme;
}

void expect_error(std::string_view name, const std::string& source, std::string_view prefix, int line,
                  int column) {
    try {
        parse(source);
        fail_test(name, "expected parser error");
    } catch (const ParserError& err) {
        const bool message_ok = err.message().rfind(prefix, 0) == 0;
        if (!message_ok || err.line() != line || err.column() != column) {
            fail_test(name, "got \"" + err.message() + "\" at " + std::to_string(err.line()) + ":" +
                                std::to_string(err.column()));
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

}  // namespace

int main() {
    try {
        const Program empty = parse("");
        if (!empty.statements.empty()) {
            fail_test("empty program", "expected no statements");
        } else {
            pass_test("empty program");
        }

        {
            const Program program = parse("x = 10");
            const AssignStmt* stmt = only_assign(program);
            const auto* lit = stmt ? as<LiteralExpr>(stmt->value.get()) : nullptr;
            if (!stmt || stmt->name != "x" || stmt->annotation != TypeAnnotationKind::None ||
                stmt->value == nullptr || !lit || lit->kind != LiteralExpr::Kind::Integer ||
                lit->lexeme != "10") {
                fail_test("integer assignment", "unexpected AST");
            } else {
                pass_test("integer assignment");
            }
        }

        {
            const Program program = parse("x = 1.5");
            const AssignStmt* stmt = only_assign(program);
            const auto* lit = stmt ? as<LiteralExpr>(stmt->value.get()) : nullptr;
            if (!lit || lit->kind != LiteralExpr::Kind::Float || lit->lexeme != "1.5") {
                fail_test("float literal", "unexpected AST");
            } else {
                pass_test("float literal");
            }
        }

        {
            const Program program = parse("x = \"hi\"");
            const AssignStmt* stmt = only_assign(program);
            const auto* lit = stmt ? as<LiteralExpr>(stmt->value.get()) : nullptr;
            if (!lit || lit->kind != LiteralExpr::Kind::String || lit->lexeme != "\"hi\"") {
                fail_test("string literal", "unexpected AST");
            } else {
                pass_test("string literal");
            }
        }

        {
            const Program program = parse("a = true b = false c = null");
            if (program.statements.size() != 3) {
                fail_test("bool and null literals", "expected 3 statements");
            } else {
                const auto* t = as<LiteralExpr>(as_stmt<AssignStmt>(program.statements[0].get())->value.get());
                const auto* f = as<LiteralExpr>(as_stmt<AssignStmt>(program.statements[1].get())->value.get());
                const auto* n = as<LiteralExpr>(as_stmt<AssignStmt>(program.statements[2].get())->value.get());
                if (!t || !f || !n || t->kind != LiteralExpr::Kind::True ||
                    f->kind != LiteralExpr::Kind::False || n->kind != LiteralExpr::Kind::Null) {
                    fail_test("bool and null literals", "unexpected literal kinds");
                } else {
                    pass_test("bool and null literals");
                }
            }
        }

        {
            const Program program = parse("x = value");
            const AssignStmt* stmt = only_assign(program);
            const auto* ident = stmt ? as<IdentifierExpr>(stmt->value.get()) : nullptr;
            if (!ident || ident->name != "value") {
                fail_test("identifier expression", "unexpected AST");
            } else {
                pass_test("identifier expression");
            }
        }

        {
            const Program program = parse("x = a + 5");
            const AssignStmt* stmt = only_assign(program);
            const auto* bin = stmt ? as<BinaryExpr>(stmt->value.get()) : nullptr;
            const auto* left = bin ? as<IdentifierExpr>(bin->left.get()) : nullptr;
            const auto* right = bin ? as<LiteralExpr>(bin->right.get()) : nullptr;
            if (!bin || bin->op != TokenKind::Plus || !left || left->name != "a" || !right ||
                right->lexeme != "5") {
                fail_test("assignment with arithmetic", "unexpected AST");
            } else {
                pass_test("assignment with arithmetic");
            }
        }

        {
            const Program program = parse("int x");
            const AssignStmt* stmt = only_assign(program);
            if (!stmt || stmt->annotation != TypeAnnotationKind::Prefix || !stmt->type_name ||
                *stmt->type_name != "int" || stmt->name != "x" || stmt->value) {
                fail_test("prefix type annotation", "unexpected AST");
            } else {
                pass_test("prefix type annotation");
            }
        }

        {
            const Program program = parse("x: int");
            const AssignStmt* stmt = only_assign(program);
            if (!stmt || stmt->annotation != TypeAnnotationKind::Colon || !stmt->type_name ||
                *stmt->type_name != "int" || stmt->name != "x" || stmt->value) {
                fail_test("colon type annotation", "unexpected AST");
            } else {
                pass_test("colon type annotation");
            }
        }

        {
            const Program program = parse("int x = 10");
            const AssignStmt* stmt = only_assign(program);
            const auto* lit = stmt ? as<LiteralExpr>(stmt->value.get()) : nullptr;
            if (!stmt || stmt->annotation != TypeAnnotationKind::Prefix || !stmt->type_name ||
                *stmt->type_name != "int" || stmt->name != "x" || !lit || lit->lexeme != "10") {
                fail_test("prefix typed assignment", "unexpected AST");
            } else {
                pass_test("prefix typed assignment");
            }
        }

        {
            const Program program = parse("x: int = 10");
            const AssignStmt* stmt = only_assign(program);
            const auto* lit = stmt ? as<LiteralExpr>(stmt->value.get()) : nullptr;
            if (!stmt || stmt->annotation != TypeAnnotationKind::Colon || !stmt->type_name ||
                *stmt->type_name != "int" || !lit || lit->lexeme != "10") {
                fail_test("colon typed assignment", "unexpected AST");
            } else {
                pass_test("colon typed assignment");
            }
        }

        {
            const Program program = parse("x = 1 + 2 * 3");
            const auto* add = as<BinaryExpr>(only_assign(program)->value.get());
            const auto* mul = add ? as<BinaryExpr>(add->right.get()) : nullptr;
            const auto* one = add ? as<LiteralExpr>(add->left.get()) : nullptr;
            const auto* two = mul ? as<LiteralExpr>(mul->left.get()) : nullptr;
            const auto* three = mul ? as<LiteralExpr>(mul->right.get()) : nullptr;
            if (!add || add->op != TokenKind::Plus || !mul || mul->op != TokenKind::Star || !one ||
                one->lexeme != "1" || !two || two->lexeme != "2" || !three || three->lexeme != "3") {
                fail_test("arithmetic precedence", "expected 1 + (2 * 3)");
            } else {
                pass_test("arithmetic precedence");
            }
        }

        {
            const Program program = parse("x = 1 + 2 == 3");
            const auto* cmp = as<BinaryExpr>(only_assign(program)->value.get());
            const auto* add = cmp ? as<BinaryExpr>(cmp->left.get()) : nullptr;
            if (!cmp || cmp->op != TokenKind::EqualEqual || !add || add->op != TokenKind::Plus) {
                fail_test("comparison precedence", "expected (1 + 2) == 3");
            } else {
                pass_test("comparison precedence");
            }
        }

        {
            const Program program = parse("x = (1 + 2) * 3");
            const auto* mul = as<BinaryExpr>(only_assign(program)->value.get());
            const auto* add = mul ? as<BinaryExpr>(mul->left.get()) : nullptr;
            if (!mul || mul->op != TokenKind::Star || !add || add->op != TokenKind::Plus) {
                fail_test("parentheses", "expected (1 + 2) * 3");
            } else {
                pass_test("parentheses");
            }
        }

        {
            const Program program = parse("x = -a + +2");
            const auto* add = as<BinaryExpr>(only_assign(program)->value.get());
            const auto* neg = add ? as<UnaryExpr>(add->left.get()) : nullptr;
            const auto* pos = add ? as<UnaryExpr>(add->right.get()) : nullptr;
            const auto* ident = neg ? as<IdentifierExpr>(neg->operand.get()) : nullptr;
            if (!add || add->op != TokenKind::Plus || !neg || neg->op != TokenKind::Minus || !pos ||
                pos->op != TokenKind::Plus || !ident || ident->name != "a") {
                fail_test("unary operators", "unexpected AST");
            } else {
                pass_test("unary operators");
            }
        }

        {
            const Program program = parse("print(\"Hello\")");
            const PrintStmt* stmt = only_print(program);
            const auto* lit = (stmt && stmt->expressions.size() == 1) ? as<LiteralExpr>(stmt->expressions[0].get())
                                                                      : nullptr;
            if (!stmt || !stmt->parenthesized || !lit || lit->lexeme != "\"Hello\"") {
                fail_test("print with parentheses", "unexpected AST");
            } else {
                pass_test("print with parentheses");
            }
        }

        {
            const Program program = parse("print \"Hello\"");
            const PrintStmt* stmt = only_print(program);
            const auto* lit = (stmt && stmt->expressions.size() == 1) ? as<LiteralExpr>(stmt->expressions[0].get())
                                                                      : nullptr;
            if (!stmt || stmt->parenthesized || !lit || lit->lexeme != "\"Hello\"") {
                fail_test("print without parentheses", "unexpected AST");
            } else {
                pass_test("print without parentheses");
            }
        }

        {
            const Program program = parse("x = 10");
            const AssignStmt* stmt = only_assign(program);
            const auto* lit = stmt ? as<LiteralExpr>(stmt->value.get()) : nullptr;
            if (!stmt || stmt->loc.line != 1 || stmt->loc.column != 1 || !lit || lit->loc.line != 1 ||
                lit->loc.column != 5) {
                fail_test("source locations", "unexpected locations");
            } else {
                pass_test("source locations");
            }
        }

        {
            const Program program = parse("x = 1 y = 2");
            if (program.statements.size() != 2 || !as_stmt<AssignStmt>(program.statements[0].get()) ||
                !as_stmt<AssignStmt>(program.statements[1].get()) ||
                as_stmt<AssignStmt>(program.statements[1].get())->name != "y") {
                fail_test("statement sequence", "expected two assignments");
            } else {
                pass_test("statement sequence");
            }
        }

        {
            const Program program = parse("print(\"A\", 1, true)");
            const PrintStmt* stmt = only_print(program);
            if (!stmt || !stmt->parenthesized || stmt->expressions.size() != 3) {
                fail_test("print multiple parenthesized", "expected 3 expressions");
            } else {
                const auto* a = as<LiteralExpr>(stmt->expressions[0].get());
                const auto* b = as<LiteralExpr>(stmt->expressions[1].get());
                const auto* c = as<LiteralExpr>(stmt->expressions[2].get());
                if (!a || a->lexeme != "\"A\"" || !b || b->lexeme != "1" || !c ||
                    c->kind != LiteralExpr::Kind::True) {
                    fail_test("print multiple parenthesized", "unexpected arguments");
                } else {
                    pass_test("print multiple parenthesized");
                }
            }
        }

        {
            const Program program = parse("print \"A\", 1, true");
            const PrintStmt* stmt = only_print(program);
            if (!stmt || stmt->parenthesized || stmt->expressions.size() != 3) {
                fail_test("print multiple bare", "expected 3 expressions");
            } else {
                const auto* a = as<LiteralExpr>(stmt->expressions[0].get());
                const auto* b = as<LiteralExpr>(stmt->expressions[1].get());
                const auto* c = as<LiteralExpr>(stmt->expressions[2].get());
                if (!a || a->lexeme != "\"A\"" || !b || b->lexeme != "1" || !c ||
                    c->kind != LiteralExpr::Kind::True) {
                    fail_test("print multiple bare", "unexpected arguments");
                } else {
                    pass_test("print multiple bare");
                }
            }
        }

        {
            const Program program = parse("print (x * y) / 2");
            const PrintStmt* stmt = only_print(program);
            const auto* div = stmt && stmt->expressions.size() == 1
                                  ? comparison(stmt->expressions[0].get(), TokenKind::Slash)
                                  : nullptr;
            const auto* mul = div ? comparison(div->left.get(), TokenKind::Star) : nullptr;
            if (!stmt || stmt->parenthesized || !div || !mul) {
                fail_test("bare print grouped arithmetic", "expected (x * y) / 2 as one argument");
            } else {
                pass_test("bare print grouped arithmetic");
            }
        }

        {
            const Program program = parse("print((x * y) / 2)");
            const PrintStmt* stmt = only_print(program);
            const auto* div = stmt && stmt->expressions.size() == 1
                                  ? comparison(stmt->expressions[0].get(), TokenKind::Slash)
                                  : nullptr;
            if (!stmt || !stmt->parenthesized || !div) {
                fail_test("paren print grouped arithmetic", "expected print((x * y) / 2)");
            } else {
                pass_test("paren print grouped arithmetic");
            }
        }

        {
            const Program program = parse("print x + y * 2");
            const PrintStmt* stmt = only_print(program);
            const auto* add = stmt && stmt->expressions.size() == 1
                                  ? comparison(stmt->expressions[0].get(), TokenKind::Plus)
                                  : nullptr;
            const auto* mul = add ? comparison(add->right.get(), TokenKind::Star) : nullptr;
            if (!add || !mul) {
                fail_test("bare print precedence", "expected x + (y * 2)");
            } else {
                pass_test("bare print precedence");
            }
        }

        {
            const Program program = parse("print x + 10, y * 2, (x + y) / 2");
            const PrintStmt* stmt = only_print(program);
            if (!stmt || stmt->parenthesized || stmt->expressions.size() != 3 ||
                !comparison(stmt->expressions[0].get(), TokenKind::Plus) ||
                !comparison(stmt->expressions[1].get(), TokenKind::Star) ||
                !comparison(stmt->expressions[2].get(), TokenKind::Slash)) {
                fail_test("bare print multi expressions", "expected three full expressions");
            } else {
                pass_test("bare print multi expressions");
            }
        }

        {
            const Program program = parse("print \"Result:\", (x * y) / 2");
            const PrintStmt* stmt = only_print(program);
            if (!stmt || stmt->expressions.size() != 2 || !comparison(stmt->expressions[1].get(), TokenKind::Slash)) {
                fail_test("bare print label and expression", "expected label plus (x * y) / 2");
            } else {
                pass_test("bare print label and expression");
            }
        }

        {
            const Program program = parse("print x > 10");
            const PrintStmt* stmt = only_print(program);
            if (!stmt || !comparison(stmt->expressions[0].get(), TokenKind::Greater)) {
                fail_test("bare print comparison", "expected x > 10");
            } else {
                pass_test("bare print comparison");
            }
        }

        {
            const Program program = parse("abs -1");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<ExprStmt>(program.statements[0].get()) : nullptr;
            const auto* call = stmt ? as<CallExpr>(stmt->expression.get()) : nullptr;
            const auto* unary = call && call->arguments.size() == 1 ? as<UnaryExpr>(call->arguments[0].get()) : nullptr;
            if (!call || call->name != "abs" || !unary || unary->op != TokenKind::Minus) {
                fail_test("whitespace call abs -1", "expected abs(unary -1)");
            } else {
                pass_test("whitespace call abs -1");
            }
        }

        {
            const Program program = parse("print abs -1");
            const PrintStmt* stmt = only_print(program);
            const auto* call =
                stmt && stmt->expressions.size() == 1 ? as<CallExpr>(stmt->expressions[0].get()) : nullptr;
            const auto* unary = call && call->arguments.size() == 1 ? as<UnaryExpr>(call->arguments[0].get()) : nullptr;
            if (!call || call->name != "abs" || !unary || unary->op != TokenKind::Minus) {
                fail_test("whitespace call print abs -1", "expected print(abs(-1))");
            } else {
                pass_test("whitespace call print abs -1");
            }
        }

        {
            const Program program = parse("min 1, 2");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<ExprStmt>(program.statements[0].get()) : nullptr;
            const auto* call = stmt ? as<CallExpr>(stmt->expression.get()) : nullptr;
            if (!call || call->name != "min" || call->arguments.size() != 2) {
                fail_test("whitespace call min 1, 2", "expected min(1, 2)");
            } else {
                pass_test("whitespace call min 1, 2");
            }
        }

        {
            const Program program = parse("func main():\n    a - 5\n");
            const auto* fn = program.statements.size() == 1 ? as_stmt<FuncStmt>(program.statements[0].get()) : nullptr;
            const auto* stmt = fn && fn->body.size() == 1 ? as_stmt<ExprStmt>(fn->body[0].get()) : nullptr;
            const auto* binary = stmt ? as<BinaryExpr>(stmt->expression.get()) : nullptr;
            if (!binary || binary->op != TokenKind::Minus) {
                fail_test("subtraction a - 5", "expected binary minus");
            } else {
                pass_test("subtraction a - 5");
            }
        }

        {
            const Program program = parse("async func main() {\n    await delay(1.5s)\n}\n");
            const auto* fn = program.statements.size() == 1 ? as_stmt<FuncStmt>(program.statements[0].get()) : nullptr;
            const auto* stmt = fn && fn->body.size() == 1 ? as_stmt<ExprStmt>(fn->body[0].get()) : nullptr;
            const auto* await_expr = stmt ? as<AwaitExpr>(stmt->expression.get()) : nullptr;
            const auto* call =
                await_expr && await_expr->operand ? as<CallExpr>(await_expr->operand.get()) : nullptr;
            const auto* dur =
                call && call->arguments.size() == 1 ? as<LiteralExpr>(call->arguments[0].get()) : nullptr;
            if (!call || call->name != "delay" || !dur || dur->kind != LiteralExpr::Kind::Duration ||
                dur->lexeme != "1.5s") {
                fail_test("await delay duration", "expected await delay(1.5s)");
            } else {
                pass_test("await delay duration");
            }
        }

        {
            const Program program = parse("int x = 10 float y = 1.5 string name = \"A\" bool ok = true");
            if (program.statements.size() != 4) {
                fail_test("prefix typed literals", "expected 4 statements");
            } else {
                const auto* x = as_stmt<AssignStmt>(program.statements[0].get());
                const auto* y = as_stmt<AssignStmt>(program.statements[1].get());
                const auto* name = as_stmt<AssignStmt>(program.statements[2].get());
                const auto* ok = as_stmt<AssignStmt>(program.statements[3].get());
                const auto* xv = x ? as<LiteralExpr>(x->value.get()) : nullptr;
                const auto* yv = y ? as<LiteralExpr>(y->value.get()) : nullptr;
                const auto* nv = name ? as<LiteralExpr>(name->value.get()) : nullptr;
                const auto* ov = ok ? as<LiteralExpr>(ok->value.get()) : nullptr;
                if (!x || x->annotation != TypeAnnotationKind::Prefix || *x->type_name != "int" || !xv ||
                    xv->lexeme != "10" || !y || *y->type_name != "float" || !yv || yv->lexeme != "1.5" || !name ||
                    *name->type_name != "string" || !nv || nv->lexeme != "\"A\"" || !ok || *ok->type_name != "bool" ||
                    !ov || ov->kind != LiteralExpr::Kind::True) {
                    fail_test("prefix typed literals", "unexpected AST");
                } else {
                    pass_test("prefix typed literals");
                }
            }
        }

        {
            const Program program = parse("x: int = 10 y: float = 1.5 name: string = \"A\" ok: bool = true");
            if (program.statements.size() != 4) {
                fail_test("colon typed literals", "expected 4 statements");
            } else {
                const auto* x = as_stmt<AssignStmt>(program.statements[0].get());
                const auto* y = as_stmt<AssignStmt>(program.statements[1].get());
                const auto* name = as_stmt<AssignStmt>(program.statements[2].get());
                const auto* ok = as_stmt<AssignStmt>(program.statements[3].get());
                if (!x || x->annotation != TypeAnnotationKind::Colon || *x->type_name != "int" ||
                    !y || y->annotation != TypeAnnotationKind::Colon || *y->type_name != "float" || !name ||
                    *name->type_name != "string" || !ok || *ok->type_name != "bool") {
                    fail_test("colon typed literals", "unexpected AST");
                } else {
                    pass_test("colon typed literals");
                }
            }
        }

        {
            const Program program = parse("if score > 90:\n    print(\"Excellent\")");
            const IfStmt* stmt = only_if(program);
            const auto* cmp = stmt ? comparison(stmt->condition.get(), TokenKind::Greater) : nullptr;
            if (!stmt || !cmp || !ident(cmp->left.get()) || ident(cmp->left.get())->name != "score" ||
                stmt->body.size() != 1 || !print_string(stmt->body[0].get(), "\"Excellent\"") ||
                !stmt->elif_branches.empty() || stmt->else_body) {
                fail_test("simple if", "unexpected AST");
            } else {
                pass_test("simple if");
            }
        }

        {
            const Program program =
                parse("if score > 90:\n    print(\"Excellent\")\n    print(\"Good job\")\nprint(\"Done\")");
            if (program.statements.size() != 2) {
                fail_test("colon multiple statements", "Done should be outside the if");
            } else {
                const IfStmt* stmt = as_stmt<IfStmt>(program.statements[0].get());
                if (!stmt || stmt->body.size() != 2 || !print_string(stmt->body[0].get(), "\"Excellent\"") ||
                    !print_string(stmt->body[1].get(), "\"Good job\"") ||
                    !print_string(program.statements[1].get(), "\"Done\"")) {
                    fail_test("colon multiple statements", "unexpected AST");
                } else {
                    pass_test("colon multiple statements");
                }
            }
        }

        {
            const Program program = parse("if score > 90:\n    print(\"Excellent\")\nelse:\n    print(\"Try again\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->body.size() != 1 || !stmt->else_body || stmt->else_body->size() != 1 ||
                !print_string((*stmt->else_body)[0].get(), "\"Try again\"") || !stmt->elif_branches.empty()) {
                fail_test("if else", "unexpected AST");
            } else {
                pass_test("if else");
            }
        }

        {
            const Program program =
                parse("if score > 90:\n    print(\"Excellent\")\nelif score > 70:\n    print(\"Good\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->elif_branches.size() != 1 || stmt->else_body || stmt->elif_branches[0].body.size() != 1 ||
                !print_string(stmt->elif_branches[0].body[0].get(), "\"Good\"") ||
                !comparison(stmt->elif_branches[0].condition.get(), TokenKind::Greater)) {
                fail_test("if elif", "unexpected AST");
            } else {
                pass_test("if elif");
            }
        }

        {
            const Program program = parse(
                "if score > 90:\n    print(\"Excellent\")\nelif score > 70:\n    print(\"Good\")\nelse:\n    print(\"Try "
                "again\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->elif_branches.size() != 1 || !stmt->else_body || stmt->else_body->size() != 1 ||
                !print_string((*stmt->else_body)[0].get(), "\"Try again\"")) {
                fail_test("if elif else", "unexpected AST");
            } else {
                pass_test("if elif else");
            }
        }

        {
            const Program program = parse(
                "if score > 90:\n    print(\"A\")\nelif score > 80:\n    print(\"B\")\nelif score > 70:\n    print(\"C\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->elif_branches.size() != 2 ||
                !print_string(stmt->elif_branches[0].body[0].get(), "\"B\"") ||
                !print_string(stmt->elif_branches[1].body[0].get(), "\"C\"")) {
                fail_test("multiple elif", "unexpected AST");
            } else {
                pass_test("multiple elif");
            }
        }

        {
            const Program program =
                parse("if score > 90:\n    print(\"Excellent\")\nelseif score > 70:\n    print(\"Good\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->elif_branches.size() != 1 ||
                !print_string(stmt->elif_branches[0].body[0].get(), "\"Good\"") ||
                !comparison(stmt->elif_branches[0].condition.get(), TokenKind::Greater)) {
                fail_test("elseif", "elseif must produce the same branch AST as elif");
            } else {
                pass_test("elseif");
            }
        }

        {
            const Program program = parse("if score > 90:\n    print(\"Excellent\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->body.size() != 1) {
                fail_test("colon block", "expected colon body");
            } else {
                pass_test("colon block");
            }
        }

        {
            const Program program = parse(
                "if score > 90 {\n    print(\"Excellent\")\n} elif score > 70 {\n    print(\"Good\")\n} else {\n    "
                "print(\"Try again\")\n}");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->body.size() != 1 || stmt->elif_branches.size() != 1 || !stmt->else_body ||
                !print_string(stmt->body[0].get(), "\"Excellent\"") ||
                !print_string(stmt->elif_branches[0].body[0].get(), "\"Good\"") ||
                !print_string((*stmt->else_body)[0].get(), "\"Try again\"")) {
                fail_test("brace block", "unexpected AST");
            } else {
                pass_test("brace block");
            }
        }

        {
            const Program program = parse("if (score > 90):\n    print(\"Excellent\")");
            const IfStmt* stmt = only_if(program);
            const auto* cmp = stmt ? comparison(stmt->condition.get(), TokenKind::Greater) : nullptr;
            if (!cmp || !ident(cmp->left.get()) || ident(cmp->left.get())->name != "score") {
                fail_test("parenthesized condition", "expected grouped comparison");
            } else {
                pass_test("parenthesized condition");
            }
        }

        {
            const Program program = parse("if score > 90:\n    print(\"Excellent\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || !comparison(stmt->condition.get(), TokenKind::Greater)) {
                fail_test("bare condition", "expected comparison");
            } else {
                pass_test("bare condition");
            }
        }

        {
            const Program program = parse("if a:\n    if b:\n        print(1)\n    else:\n        print(2)");
            const IfStmt* outer = only_if(program);
            const IfStmt* inner = outer && outer->body.size() == 1 ? as_stmt<IfStmt>(outer->body[0].get()) : nullptr;
            if (!outer || outer->else_body || !inner || !inner->else_body || inner->body.size() != 1 ||
                !print_string(inner->body[0].get(), "1") || !print_string((*inner->else_body)[0].get(), "2")) {
                fail_test("nested if", "inner if/else should nest inside outer then-body");
            } else {
                pass_test("nested if");
            }
        }

        {
            const Program program = parse(
                "if true:\n    print(\"outer\")\n    if true:\n        print(\"inner\")\n    print(\"outer again\")\nprint(\"done\")");
            if (program.statements.size() != 2) {
                fail_test("nested indentation", "done should be a top-level statement");
            } else {
                const IfStmt* outer = as_stmt<IfStmt>(program.statements[0].get());
                const IfStmt* inner = outer && outer->body.size() == 3 ? as_stmt<IfStmt>(outer->body[1].get()) : nullptr;
                if (!outer || !inner || !print_string(outer->body[0].get(), "\"outer\"") ||
                    !print_string(inner->body[0].get(), "\"inner\"") ||
                    !print_string(outer->body[2].get(), "\"outer again\"") ||
                    !print_string(program.statements[1].get(), "\"done\"")) {
                    fail_test("nested indentation", "unexpected AST");
                } else {
                    pass_test("nested indentation");
                }
            }
        }

        {
            const Program program =
                parse("if score > 90 {\n    print(\"Excellent\")\n} elseif score > 70 {\n    print(\"Good\")\n}");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->elif_branches.size() != 1 ||
                !print_string(stmt->elif_branches[0].body[0].get(), "\"Good\"")) {
                fail_test("brace elseif", "unexpected AST");
            } else {
                pass_test("brace elseif");
            }
        }

        {
            const Program program = parse("if true {\n    if true {\n        print(\"inner\")\n    }\n    print(\"outer\")\n}");
            const IfStmt* outer = only_if(program);
            const IfStmt* inner = outer && outer->body.size() == 2 ? as_stmt<IfStmt>(outer->body[0].get()) : nullptr;
            if (!inner || inner->body.size() != 1 || !print_string(inner->body[0].get(), "\"inner\"") ||
                !print_string(outer->body[1].get(), "\"outer\"")) {
                fail_test("nested brace", "unexpected AST");
            } else {
                pass_test("nested brace");
            }
        }

        {
            const Program program = parse("if (a > b):\n    print(\"if\")");
            const IfStmt* stmt = only_if(program);
            const auto* cmp = stmt ? comparison(stmt->condition.get(), TokenKind::Greater) : nullptr;
            if (!cmp || !ident(cmp->left.get()) || ident(cmp->left.get())->name != "a" ||
                !ident(cmp->right.get()) || ident(cmp->right.get())->name != "b") {
                fail_test("if parenthesized condition", "expected grouped a > b");
            } else {
                pass_test("if parenthesized condition");
            }
        }

        {
            const Program program = parse("if false:\n    print(\"if\")\nelif ((a - 5) / 3):\n    print(\"elif\")");
            const IfStmt* stmt = only_if(program);
            const auto* div = stmt && stmt->elif_branches.size() == 1
                                  ? as<BinaryExpr>(stmt->elif_branches[0].condition.get())
                                  : nullptr;
            const auto* sub = div ? as<BinaryExpr>(div->left.get()) : nullptr;
            if (!div || div->op != TokenKind::Slash || !sub || sub->op != TokenKind::Minus) {
                fail_test("elif parenthesized condition", "expected ((a - 5) / 3)");
            } else {
                pass_test("elif parenthesized condition");
            }
        }

        {
            const Program program =
                parse("if false:\n    print(\"if\")\nelseif (a + b * 2) > 10:\n    print(\"elseif\")");
            const IfStmt* stmt = only_if(program);
            const auto* gt = stmt && stmt->elif_branches.size() == 1
                                 ? comparison(stmt->elif_branches[0].condition.get(), TokenKind::Greater)
                                 : nullptr;
            const auto* add = gt ? as<BinaryExpr>(gt->left.get()) : nullptr;
            const auto* mul = add ? as<BinaryExpr>(add->right.get()) : nullptr;
            if (!gt || !add || add->op != TokenKind::Plus || !mul || mul->op != TokenKind::Star) {
                fail_test("elseif parenthesized condition", "expected (a + b * 2) > 10");
            } else {
                pass_test("elseif parenthesized condition");
            }
        }

        {
            const Program program = parse("if a + 1:\n    print(\"if\")");
            const IfStmt* stmt = only_if(program);
            const auto* add = stmt ? as<BinaryExpr>(stmt->condition.get()) : nullptr;
            if (!add || add->op != TokenKind::Plus) {
                fail_test("if math condition", "expected a + 1");
            } else {
                pass_test("if math condition");
            }
        }

        {
            const Program program = parse("if false:\n    print(\"if\")\nelif a - 5:\n    print(\"elif\")");
            const IfStmt* stmt = only_if(program);
            const auto* sub = stmt && stmt->elif_branches.size() == 1
                                  ? as<BinaryExpr>(stmt->elif_branches[0].condition.get())
                                  : nullptr;
            if (!sub || sub->op != TokenKind::Minus) {
                fail_test("elif math condition", "expected a - 5");
            } else {
                pass_test("elif math condition");
            }
        }

        {
            const Program program = parse("if false:\n    print(\"if\")\nelseif a * 2:\n    print(\"elseif\")");
            const IfStmt* stmt = only_if(program);
            const auto* mul = stmt && stmt->elif_branches.size() == 1
                                  ? as<BinaryExpr>(stmt->elif_branches[0].condition.get())
                                  : nullptr;
            if (!mul || mul->op != TokenKind::Star) {
                fail_test("elseif math condition", "expected a * 2");
            } else {
                pass_test("elseif math condition");
            }
        }

        {
            const Program program =
                parse("if false:\n    print(\"no\")\nelseif (a + b * 2) > 10:\n    print(\"yes\")");
            const IfStmt* stmt = only_if(program);
            const auto* gt = stmt && stmt->elif_branches.size() == 1
                                 ? comparison(stmt->elif_branches[0].condition.get(), TokenKind::Greater)
                                 : nullptr;
            const auto* add = gt ? as<BinaryExpr>(gt->left.get()) : nullptr;
            const auto* mul = add ? as<BinaryExpr>(add->right.get()) : nullptr;
            if (!add || add->op != TokenKind::Plus || !mul || mul->op != TokenKind::Star ||
                !ident(add->left.get()) || ident(add->left.get())->name != "a" || !ident(mul->left.get()) ||
                ident(mul->left.get())->name != "b") {
                fail_test("condition operator precedence", "expected a + (b * 2) compared to 10");
            } else {
                pass_test("condition operator precedence");
            }
        }

        {
            const Program program =
                parse("if false:\n    print(\"no\")\nelseif ((a + b) * 2) > 10:\n    print(\"yes\")");
            const IfStmt* stmt = only_if(program);
            const auto* gt = stmt && stmt->elif_branches.size() == 1
                                 ? comparison(stmt->elif_branches[0].condition.get(), TokenKind::Greater)
                                 : nullptr;
            const auto* mul = gt ? as<BinaryExpr>(gt->left.get()) : nullptr;
            const auto* add = mul ? as<BinaryExpr>(mul->left.get()) : nullptr;
            if (!mul || mul->op != TokenKind::Star || !add || add->op != TokenKind::Plus) {
                fail_test("condition nested parentheses", "expected ((a + b) * 2) > 10");
            } else {
                pass_test("condition nested parentheses");
            }
        }

        {
            const Program program = parse(
                "if a > b:\n    print(\"if\")\nelif (a + 2) > b:\n    print(\"elif\")\nelseif (a * 2) > b:\n    "
                "print(\"elseif\")\nelse:\n    print(\"else\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->elif_branches.size() != 2 || !stmt->else_body ||
                !print_string(stmt->body[0].get(), "\"if\"") ||
                !print_string(stmt->elif_branches[0].body[0].get(), "\"elif\"") ||
                !print_string(stmt->elif_branches[1].body[0].get(), "\"elseif\"") ||
                !print_string((*stmt->else_body)[0].get(), "\"else\"") ||
                !comparison(stmt->elif_branches[0].condition.get(), TokenKind::Greater) ||
                !comparison(stmt->elif_branches[1].condition.get(), TokenKind::Greater)) {
                fail_test("if elif elseif else indent", "unexpected AST");
            } else {
                pass_test("if elif elseif else indent");
            }
        }

        {
            const Program program = parse(
                "if a > b {\n    print(\"if\")\n} elif (a + 2) > b {\n    print(\"elif\")\n} elseif (a * 2) > b {\n    "
                "print(\"elseif\")\n} else {\n    print(\"else\")\n}");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->elif_branches.size() != 2 || !stmt->else_body ||
                !print_string(stmt->body[0].get(), "\"if\"") ||
                !print_string(stmt->elif_branches[0].body[0].get(), "\"elif\"") ||
                !print_string(stmt->elif_branches[1].body[0].get(), "\"elseif\"") ||
                !print_string((*stmt->else_body)[0].get(), "\"else\"")) {
                fail_test("if elif elseif else brace", "unexpected AST");
            } else {
                pass_test("if elif elseif else brace");
            }
        }

        {
            const Program program = parse("numbers = [10, 20, 30]");
            const AssignStmt* stmt = only_assign(program);
            const auto* list = stmt ? as<ListExpr>(stmt->value.get()) : nullptr;
            const auto* a = list && list->elements.size() == 3 ? as<LiteralExpr>(list->elements[0].get()) : nullptr;
            const auto* b = list ? as<LiteralExpr>(list->elements[1].get()) : nullptr;
            const auto* c = list ? as<LiteralExpr>(list->elements[2].get()) : nullptr;
            if (!list || !a || a->lexeme != "10" || !b || b->lexeme != "20" || !c || c->lexeme != "30") {
                fail_test("list literal", "unexpected AST");
            } else {
                pass_test("list literal");
            }
        }

        {
            const Program program = parse("items = []");
            const AssignStmt* stmt = only_assign(program);
            const auto* list = stmt ? as<ListExpr>(stmt->value.get()) : nullptr;
            if (!list || !list->elements.empty()) {
                fail_test("empty list", "expected empty ListExpr");
            } else {
                pass_test("empty list");
            }
        }

        {
            const Program program = parse("player = {\"name\": \"Emir\", \"score\": 100}");
            const AssignStmt* stmt = only_assign(program);
            const auto* dict = stmt ? as<DictExpr>(stmt->value.get()) : nullptr;
            const auto* k0 = dict && dict->entries.size() == 2 ? as<LiteralExpr>(dict->entries[0].key.get()) : nullptr;
            const auto* v0 = dict && dict->entries.size() == 2 ? as<LiteralExpr>(dict->entries[0].value.get()) : nullptr;
            if (!dict || !k0 || k0->lexeme != "\"name\"" || !v0 || v0->lexeme != "\"Emir\"") {
                fail_test("dict literal", "unexpected AST");
            } else {
                pass_test("dict literal");
            }
        }

        {
            const Program program = parse("empty = {}");
            const AssignStmt* stmt = only_assign(program);
            const auto* dict = stmt ? as<DictExpr>(stmt->value.get()) : nullptr;
            if (!dict || !dict->entries.empty()) {
                fail_test("empty dict", "expected empty DictExpr");
            } else {
                pass_test("empty dict");
            }
        }

        {
            const Program program = parse("t = ()");
            const AssignStmt* stmt = only_assign(program);
            const auto* tuple = stmt ? as<TupleExpr>(stmt->value.get()) : nullptr;
            if (!tuple || !tuple->elements.empty()) {
                fail_test("empty tuple", "expected empty TupleExpr");
            } else {
                pass_test("empty tuple");
            }
        }

        {
            const Program program = parse("t = (10,)");
            const AssignStmt* stmt = only_assign(program);
            const auto* tuple = stmt ? as<TupleExpr>(stmt->value.get()) : nullptr;
            const auto* value = tuple && tuple->elements.size() == 1 ? as<LiteralExpr>(tuple->elements[0].get()) : nullptr;
            if (!value || value->lexeme != "10") {
                fail_test("single-element tuple", "expected (10,)");
            } else {
                pass_test("single-element tuple");
            }
        }

        {
            const Program program = parse("t = (10, 20, 30)");
            const AssignStmt* stmt = only_assign(program);
            const auto* tuple = stmt ? as<TupleExpr>(stmt->value.get()) : nullptr;
            if (!tuple || tuple->elements.size() != 3) {
                fail_test("tuple literal", "expected three elements");
            } else {
                pass_test("tuple literal");
            }
        }

        {
            const Program program = parse("x = (10)");
            const AssignStmt* stmt = only_assign(program);
            const auto* value = stmt ? as<LiteralExpr>(stmt->value.get()) : nullptr;
            if (!value || value->lexeme != "10" || as<TupleExpr>(stmt->value.get())) {
                fail_test("parenthesized expression", "expected grouped integer, not tuple");
            } else {
                pass_test("parenthesized expression");
            }
        }

        {
            const Program program = parse("numbers = {1, 2, 3}");
            const AssignStmt* stmt = only_assign(program);
            const auto* set = stmt ? as<SetExpr>(stmt->value.get()) : nullptr;
            if (!set || set->elements.size() != 3) {
                fail_test("set literal", "expected SetExpr with 3 elements");
            } else {
                pass_test("set literal");
            }
        }

        {
            const Program program = parse("s = {1}");
            const AssignStmt* stmt = only_assign(program);
            const auto* set = stmt ? as<SetExpr>(stmt->value.get()) : nullptr;
            if (!set || set->elements.size() != 1) {
                fail_test("single-element set", "expected SetExpr");
            } else {
                pass_test("single-element set");
            }
        }

        {
            const Program program = parse("print(numbers[0])");
            const PrintStmt* stmt = only_print(program);
            const auto* index = stmt && stmt->expressions.size() == 1 ? as<IndexExpr>(stmt->expressions[0].get()) : nullptr;
            if (!index || !ident(index->object.get()) || ident(index->object.get())->name != "numbers" ||
                !as<LiteralExpr>(index->index.get()) || as<LiteralExpr>(index->index.get())->lexeme != "0") {
                fail_test("index read", "expected numbers[0]");
            } else {
                pass_test("index read");
            }
        }

        {
            const Program program = parse("numbers[1] = 50");
            const auto* stmt = program.statements.size() == 1 ? as_stmt<IndexAssignStmt>(program.statements[0].get()) : nullptr;
            if (!stmt || !ident(stmt->object.get()) || ident(stmt->object.get())->name != "numbers" ||
                !as<LiteralExpr>(stmt->index.get()) || as<LiteralExpr>(stmt->index.get())->lexeme != "1" ||
                !as<LiteralExpr>(stmt->value.get()) || as<LiteralExpr>(stmt->value.get())->lexeme != "50") {
                fail_test("index write", "expected numbers[1] = 50");
            } else {
                pass_test("index write");
            }
        }

        {
            const Program program = parse("if {}:\n    print(\"no\")");
            const IfStmt* stmt = only_if(program);
            if (!stmt || !as<DictExpr>(stmt->condition.get())) {
                fail_test("empty dict condition", "expected {} as dict condition");
            } else {
                pass_test("empty dict condition");
            }
        }

        {
            const Program program = parse("if true {\n    print(\"block\")\n}");
            const IfStmt* stmt = only_if(program);
            if (!stmt || stmt->body.size() != 1 || !print_string(stmt->body[0].get(), "\"block\"")) {
                fail_test("brace block still parses", "block syntax must not become a dict");
            } else {
                pass_test("brace block still parses");
            }
        }

        {
            const Program program = parse("grid = [[1, 2], [3]]");
            const AssignStmt* stmt = only_assign(program);
            const auto* outer = stmt ? as<ListExpr>(stmt->value.get()) : nullptr;
            const auto* inner = outer && outer->elements.size() == 2 ? as<ListExpr>(outer->elements[0].get()) : nullptr;
            if (!inner || inner->elements.size() != 2) {
                fail_test("nested list", "expected list of lists");
            } else {
                pass_test("nested list");
            }
        }

        {
            const Program program = parse("items.append(10)");
            const auto* stmt = program.statements.size() == 1 ? as_stmt<ExprStmt>(program.statements[0].get()) : nullptr;
            const auto* call = stmt ? as<MethodCallExpr>(stmt->expression.get()) : nullptr;
            if (!call || call->method != "append" || !ident(call->object.get()) ||
                ident(call->object.get())->name != "items" || call->arguments.size() != 1) {
                fail_test("method call statement", "expected items.append(10)");
            } else {
                pass_test("method call statement");
            }
        }

        {
            const Program program = parse("x = items.length()");
            const AssignStmt* stmt = only_assign(program);
            const auto* call = stmt ? as<MethodCallExpr>(stmt->value.get()) : nullptr;
            if (!call || call->method != "length" || !call->arguments.empty()) {
                fail_test("method call expression", "expected items.length()");
            } else {
                pass_test("method call expression");
            }
        }

        {
            const Program program = parse("if numbers.is_empty():\n    print(\"empty\")");
            const IfStmt* stmt = only_if(program);
            const auto* call = stmt ? as<MethodCallExpr>(stmt->condition.get()) : nullptr;
            if (!call || call->method != "is_empty") {
                fail_test("method call condition", "expected numbers.is_empty()");
            } else {
                pass_test("method call condition");
            }
        }

        {
            const Program program = parse("while x < 10:\n    print(x)");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<WhileStmt>(program.statements[0].get()) : nullptr;
            if (!stmt || !comparison(stmt->condition.get(), TokenKind::Less) || stmt->body.size() != 1) {
                fail_test("while colon", "expected while with comparison body");
            } else {
                pass_test("while colon");
            }
        }

        {
            const Program program = parse("while (x < 10) {\n    print(x)\n}");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<WhileStmt>(program.statements[0].get()) : nullptr;
            if (!stmt || !comparison(stmt->condition.get(), TokenKind::Less) || stmt->body.size() != 1) {
                fail_test("while paren brace", "expected parenthesized while");
            } else {
                pass_test("while paren brace");
            }
        }

        {
            const Program program = parse("for item in items:\n    print(item)");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<ForStmt>(program.statements[0].get()) : nullptr;
            if (!stmt || stmt->item != "item" || !ident(stmt->iterable.get()) ||
                ident(stmt->iterable.get())->name != "items" || stmt->body.size() != 1) {
                fail_test("for in colon", "expected for item in items");
            } else {
                pass_test("for in colon");
            }
        }

        {
            const Program program = parse("for (item in items) {\n    print(item)\n}");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<ForStmt>(program.statements[0].get()) : nullptr;
            if (!stmt || stmt->item != "item" || stmt->body.size() != 1) {
                fail_test("for paren brace", "expected parenthesized for");
            } else {
                pass_test("for paren brace");
            }
        }

        {
            const Program program = parse("x = range(2, 10, 2)");
            const AssignStmt* stmt = only_assign(program);
            const auto* call = stmt ? as<CallExpr>(stmt->value.get()) : nullptr;
            if (!call || call->name != "range" || call->arguments.size() != 3) {
                fail_test("range call", "expected range(2, 10, 2)");
            } else {
                pass_test("range call");
            }
        }

        {
            const Program program = parse("while x < 10:\n    break\n    continue");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<WhileStmt>(program.statements[0].get()) : nullptr;
            if (!stmt || stmt->body.size() != 2 || !as_stmt<BreakStmt>(stmt->body[0].get()) ||
                !as_stmt<ContinueStmt>(stmt->body[1].get())) {
                fail_test("break continue parse", "expected break then continue");
            } else {
                pass_test("break continue parse");
            }
        }

        {
            const Program program = parse("x = a and b or c");
            const AssignStmt* stmt = only_assign(program);
            const auto* or_expr = stmt ? comparison(stmt->value.get(), TokenKind::Or) : nullptr;
            const auto* and_expr = or_expr ? comparison(or_expr->left.get(), TokenKind::And) : nullptr;
            if (!or_expr || !and_expr) {
                fail_test("logical precedence parse", "expected (a and b) or c");
            } else {
                pass_test("logical precedence parse");
            }
        }

        {
            const Program program = parse("x = !active && score >= 50");
            const AssignStmt* stmt = only_assign(program);
            const auto* and_expr = stmt ? comparison(stmt->value.get(), TokenKind::AmpAmp) : nullptr;
            const auto* unary = and_expr ? as<UnaryExpr>(and_expr->left.get()) : nullptr;
            if (!and_expr || !unary || unary->op != TokenKind::Bang ||
                !comparison(and_expr->right.get(), TokenKind::GreaterEqual)) {
                fail_test("logical symbols parse", "expected !active && score >= 50");
            } else {
                pass_test("logical symbols parse");
            }
        }

        {
            const Program program = parse("x = not active");
            const AssignStmt* stmt = only_assign(program);
            const auto* unary = stmt ? as<UnaryExpr>(stmt->value.get()) : nullptr;
            if (!unary || unary->op != TokenKind::Not) {
                fail_test("unary not parse", "expected not active");
            } else {
                pass_test("unary not parse");
            }
        }

        {
            const Program program = parse("name = input(\"Name: \")");
            const AssignStmt* stmt = only_assign(program);
            const auto* call = stmt ? as<CallExpr>(stmt->value.get()) : nullptr;
            if (!call || call->name != "input" || call->arguments.size() != 1) {
                fail_test("input call parse", "expected input(\"Name: \")");
            } else {
                pass_test("input call parse");
            }
        }

        {
            const Program program = parse("age = int(input(\"Age: \"))");
            const AssignStmt* stmt = only_assign(program);
            const auto* conv = stmt ? as<CallExpr>(stmt->value.get()) : nullptr;
            const auto* inner = conv && conv->arguments.size() == 1 ? as<CallExpr>(conv->arguments[0].get()) : nullptr;
            if (!conv || conv->name != "int" || !inner || inner->name != "input") {
                fail_test("int input parse", "expected int(input(...))");
            } else {
                pass_test("int input parse");
            }
        }

        {
            const Program program = parse("match score {\n    100:\n        print(\"Perfect\")\n    90, 95:\n        print(\"Excellent\")\n    _:\n        print(\"Other\")\n}");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<MatchStmt>(program.statements[0].get()) : nullptr;
            if (!stmt || stmt->cases.size() != 3 || !stmt->cases[0].pattern ||
                stmt->cases[0].pattern->kind != MatchPatternKind::Literal || !stmt->cases[1].pattern ||
                stmt->cases[1].pattern->kind != MatchPatternKind::Literals ||
                stmt->cases[1].pattern->literals.size() != 2 || !stmt->cases[2].pattern ||
                stmt->cases[2].pattern->kind != MatchPatternKind::Wildcard) {
                fail_test("match brace parse", "expected literal, multi, wildcard cases");
            } else {
                pass_test("match brace parse");
            }
        }

        {
            const Program program = parse("match score:\n    value if value >= 90:\n        print(\"Excellent\")\n    _:\n        print(\"Other\")\n");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<MatchStmt>(program.statements[0].get()) : nullptr;
            if (!stmt || stmt->cases.size() != 2 || !stmt->cases[0].pattern ||
                stmt->cases[0].pattern->kind != MatchPatternKind::Binding ||
                !stmt->cases[0].pattern->binding || *stmt->cases[0].pattern->binding != "value" ||
                !stmt->cases[0].guard || !stmt->cases[1].pattern ||
                stmt->cases[1].pattern->kind != MatchPatternKind::Wildcard) {
                fail_test("match guard parse", "expected binding guard and wildcard");
            } else {
                pass_test("match guard parse");
            }
        }

        {
            const Program program = parse("x = a & 1 == 0");
            const auto* eq = as<BinaryExpr>(only_assign(program)->value.get());
            const auto* band = eq ? as<BinaryExpr>(eq->left.get()) : nullptr;
            if (!eq || eq->op != TokenKind::EqualEqual || !band || band->op != TokenKind::Amp) {
                fail_test("bitwise vs comparison parse", "expected (a & 1) == 0");
            } else {
                pass_test("bitwise vs comparison parse");
            }
        }

        {
            const Program program = parse("x = a | b ^ c");
            const auto* bor = as<BinaryExpr>(only_assign(program)->value.get());
            const auto* bxor = bor ? as<BinaryExpr>(bor->right.get()) : nullptr;
            if (!bor || bor->op != TokenKind::Pipe || !bxor || bxor->op != TokenKind::Caret) {
                fail_test("bitwise or xor parse", "expected a | (b ^ c)");
            } else {
                pass_test("bitwise or xor parse");
            }
        }

        {
            const Program program = parse("x = ~a << 2");
            const auto* shl = as<BinaryExpr>(only_assign(program)->value.get());
            const auto* bitnot = shl ? as<UnaryExpr>(shl->left.get()) : nullptr;
            if (!shl || shl->op != TokenKind::LessLess || !bitnot || bitnot->op != TokenKind::Tilde) {
                fail_test("shift unary parse", "expected (~a) << 2");
            } else {
                pass_test("shift unary parse");
            }
        }

        {
            const Program program = parse("x += 2");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<ExprStmt>(program.statements[0].get()) : nullptr;
            const auto* assign = stmt ? as<AssignExpr>(stmt->expression.get()) : nullptr;
            if (!assign || assign->op != TokenKind::PlusEqual || !ident(assign->target.get()) ||
                ident(assign->target.get())->name != "x") {
                fail_test("compound assignment parse", "expected x += 2");
            } else {
                pass_test("compound assignment parse");
            }
        }

        {
            const Program program = parse("x++");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<ExprStmt>(program.statements[0].get()) : nullptr;
            const auto* update = stmt ? as<UpdateExpr>(stmt->expression.get()) : nullptr;
            if (!update || update->prefix || update->op != TokenKind::PlusPlus) {
                fail_test("postfix increment parse", "expected x++");
            } else {
                pass_test("postfix increment parse");
            }
        }

        {
            const Program program = parse("++x");
            const auto* stmt =
                program.statements.size() == 1 ? as_stmt<ExprStmt>(program.statements[0].get()) : nullptr;
            const auto* update = stmt ? as<UpdateExpr>(stmt->expression.get()) : nullptr;
            if (!update || !update->prefix || update->op != TokenKind::PlusPlus) {
                fail_test("prefix increment parse", "expected ++x");
            } else {
                pass_test("prefix increment parse");
            }
        }

        {
            const Program program = parse("x = a ? b : c ? d : e");
            const auto* outer = as<TernaryExpr>(only_assign(program)->value.get());
            const auto* inner = outer ? as<TernaryExpr>(outer->else_expr.get()) : nullptr;
            if (!outer || as<TernaryExpr>(outer->then_expr.get()) || !inner) {
                fail_test("nested ternary parse", "expected right-associative ternary");
            } else {
                pass_test("nested ternary parse");
            }
        }

        {
            const Program program = parse("x = v in xs");
            const auto* mem = as<MembershipExpr>(only_assign(program)->value.get());
            if (!mem || mem->negated || !ident(mem->value.get()) || ident(mem->value.get())->name != "v") {
                fail_test("membership parse", "expected v in xs");
            } else {
                pass_test("membership parse");
            }
        }

        {
            const Program program = parse("x = v not in xs");
            const auto* mem = as<MembershipExpr>(only_assign(program)->value.get());
            if (!mem || !mem->negated) {
                fail_test("not in parse", "expected v not in xs");
            } else {
                pass_test("not in parse");
            }
        }

        {
            const Program program = parse("x = a > b and c > d");
            const auto* and_expr = comparison(only_assign(program)->value.get(), TokenKind::And);
            if (!and_expr || !comparison(and_expr->left.get(), TokenKind::Greater) ||
                !comparison(and_expr->right.get(), TokenKind::Greater)) {
                fail_test("compare and parse", "expected (a > b) and (c > d)");
            } else {
                pass_test("compare and parse");
            }
        }
    } catch (const std::exception& ex) {
        fail_test("parser tests", ex.what());
    }

    expect_error("unexpected token", "1 = 2", "invalid assignment", 1, 3);
    expect_error("invalid statement", "x + 1", "invalid statement", 1, 1);
    expect_error("invalid assignment", "x =", "invalid assignment", 1, 4);
    expect_error("invalid expression", "x = )", "invalid expression", 1, 5);
    expect_error("missing paren", "print(\"Hello\"", "missing expected token", 1, 14);
    expect_error("missing type name", "x:", "missing expected token", 1, 3);
    expect_error("keyword is not a statement", "for x = 1", "missing expected token", 1, 7);
    expect_error("while missing condition", "while:\n    print(1)", "missing condition", 1, 6);
    expect_error("for missing in", "for x:\n    print(1)", "missing expected token", 1, 6);
    expect_error("for missing variable", "for in items:\n    print(1)", "missing expected token", 1, 5);
    expect_error("for missing collection", "for x in:\n    print(1)", "missing collection", 1, 9);
    expect_error("while colon brace mix", "while x: {}", "cannot mix ':' and '{'", 1, 10);
    expect_error("trailing comma print", "print(\"Hello\",)", "invalid print arguments", 1, 14);
    expect_error("leading comma print", "print(, 10)", "invalid print arguments", 1, 7);
    expect_error("trailing comma bare print", "print \"Hello\",", "invalid print arguments", 1, 14);
    expect_error("null is not a type name", "x: null", "missing expected token", 1, 4);
    expect_error("else if rejected", "if x:\n    print(1)\nelse if y:\n    print(2)", "else if is not supported", 3, 6);
    expect_error("missing condition", "if:\n    print(1)", "missing condition", 1, 3);
    expect_error("missing block", "if x", "missing block", 1, 5);
    expect_error("unclosed brace", "if x {\n    print(1)", "missing expected token", 2, 13);
    expect_error("extra brace", "if x {\n    print(1)\n} }", "unexpected token", 3, 3);
    expect_error("invalid elif", "elif x:\n    print(1)", "invalid elif", 1, 1);
    expect_error("invalid else", "else:\n    print(1)", "invalid else", 1, 1);
    expect_error("colon missing indent", "if true:\nprint(\"Passed\")", "missing indent", 2, 1);
    expect_error("colon extra indent", "if true:\n    print(\"a\")\n        print(\"b\")", "unexpected indent", 3, 9);
    expect_error("wrong elif indent", "if true:\n    print(\"a\")\n  elif false:\n    print(\"b\")", "invalid indent", 3, 3);
    expect_error("brace missing indent", "if true {\nprint(\"Passed\")\n}", "missing indent", 2, 1);
    expect_error("same-line brace body", "if true { print(1) }", "missing indent", 1, 11);
    expect_error("colon brace mix", "if a > b: {}", "cannot mix ':' and '{'", 1, 11);
    expect_error("trailing comma list", "x = [1,]", "invalid list", 1, 7);
    expect_error("trailing comma tuple", "x = (1, 2,)", "invalid tuple", 1, 10);
    expect_error("dict key not string", "x = {1: 2}", "dictionary key must be a string", 1, 6);
    expect_error("trailing comma set", "x = {1,}", "invalid set", 1, 7);
    expect_error("trailing comma dict", "x = {\"a\": 1,}", "invalid dictionary", 1, 12);
    expect_error("trailing comma call", "x.append(1,)", "invalid call arguments", 1, 11);
    expect_error("match missing expression", "match:\n    _:\n        print(1)", "missing expression", 1, 6);
    expect_error("match uses else", "match x:\n    else:\n        print(1)", "match uses '_' as the fallback, not else", 2, 5);
    expect_error("empty match", "match x {}", "empty match", 1, 11);
    expect_error("invalid match pattern", "match x:\n    if true:\n        print(1)", "invalid pattern", 2, 5);
    expect_error("non lvalue increment", "5++", "invalid increment", 1, 2);
    expect_error("non lvalue prefix increment", "++5", "invalid increment", 1, 1);
    expect_error("non lvalue compound", "x + 1 += 2", "invalid assignment", 1, 7);
    expect_error("missing ternary colon", "x = a ? b", "missing expected token", 1, 10);
    expect_error("invalid function syntax", "func ():\n    return 1\n", "invalid function syntax", 1, 6);
    expect_error("duplicate parameter", "func test(x, x):\n    return x\n", "duplicate parameter", 1, 14);
    expect_error("return outside function", "return 1", "return outside function", 1, 1);
    expect_error("trailing comma params", "func test(x,):\n    return x\n", "invalid function syntax", 1, 12);
    expect_error("expression statement outside function", "1 + 2", "invalid statement", 1, 1);

    {
        const Program program = parse("func add(a, b):\n    return a + b\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements.size() == 1 ? program.statements[0].get() : nullptr);
        const auto* ret = fn && fn->body.size() == 1 ? as_stmt<ReturnStmt>(fn->body[0].get()) : nullptr;
        const auto* sum = ret && ret->value ? as<BinaryExpr>(ret->value.get()) : nullptr;
        if (!fn || fn->name != "add" || fn->parameters.size() != 2 || fn->parameters[0].name != "a" ||
            fn->parameters[1].name != "b" || !sum || sum->op != TokenKind::Plus) {
            fail_test("function colon declaration", "unexpected AST");
        } else {
            pass_test("function colon declaration");
        }
    }

    {
        const Program program = parse("func test() {\n    return\n}\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements.size() == 1 ? program.statements[0].get() : nullptr);
        const auto* ret = fn && fn->body.size() == 1 ? as_stmt<ReturnStmt>(fn->body[0].get()) : nullptr;
        if (!fn || fn->name != "test" || !fn->parameters.empty() || !ret || ret->value) {
            fail_test("function brace zero params", "unexpected AST");
        } else {
            pass_test("function brace zero params");
        }
    }

    {
        const Program program = parse("result = add(10, 20)\n");
        const auto* assign = only_assign(program);
        const auto* call = assign ? as<CallExpr>(assign->value.get()) : nullptr;
        if (!call || call->name != "add" || call->arguments.size() != 2) {
            fail_test("function call expression", "unexpected AST");
        } else {
            pass_test("function call expression");
        }
    }

    {
        const Program program = parse("func square(x):\n    x * x\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements.size() == 1 ? program.statements[0].get() : nullptr);
        const auto* expr_stmt = fn && fn->body.size() == 1 ? as_stmt<ExprStmt>(fn->body[0].get()) : nullptr;
        const auto* mul = expr_stmt && expr_stmt->expression ? as<BinaryExpr>(expr_stmt->expression.get()) : nullptr;
        if (!fn || !mul || mul->op != TokenKind::Star) {
            fail_test("function trailing expression", "unexpected AST");
        } else {
            pass_test("function trailing expression");
        }
    }

    {
        const Program program = parse("func outer():\n    func inner():\n        return 1\n    inner()\n");
        const auto* outer = as_stmt<FuncStmt>(program.statements.size() == 1 ? program.statements[0].get() : nullptr);
        const auto* inner = outer && outer->body.size() >= 1 ? as_stmt<FuncStmt>(outer->body[0].get()) : nullptr;
        if (!outer || !inner || inner->name != "inner") {
            fail_test("nested function declaration", "unexpected AST");
        } else {
            pass_test("nested function declaration");
        }
    }

    {
        const Program program = parse("struct Player:\n    name\n    number\n");
        const auto* stmt = as_stmt<StructStmt>(program.statements.size() == 1 ? program.statements[0].get() : nullptr);
        if (!stmt || stmt->name != "Player" || stmt->is_record || stmt->fields.size() != 2 ||
            stmt->fields[0].name != "name" || stmt->fields[1].name != "number") {
            fail_test("struct declaration", "unexpected AST");
        } else {
            pass_test("struct declaration");
        }
    }
    {
        const Program program = parse("record Player:\n    name\n");
        const auto* stmt = as_stmt<StructStmt>(program.statements.size() == 1 ? program.statements[0].get() : nullptr);
        if (!stmt || !stmt->is_record || stmt->name != "Player") {
            fail_test("record declaration", "unexpected AST");
        } else {
            pass_test("record declaration");
        }
    }
    {
        const Program program = parse("enum Color:\n    RED\n    GREEN\n");
        const auto* stmt = as_stmt<EnumStmt>(program.statements.size() == 1 ? program.statements[0].get() : nullptr);
        if (!stmt || stmt->name != "Color" || stmt->members.size() != 2 || stmt->members[0] != "RED") {
            fail_test("enum declaration", "unexpected AST");
        } else {
            pass_test("enum declaration");
        }
    }
    {
        const Program program = parse("x = Color.RED\n");
        const auto* stmt = program.statements.size() == 1 ? as_stmt<AssignStmt>(program.statements[0].get()) : nullptr;
        const auto* field = stmt && stmt->value ? as<FieldAccessExpr>(stmt->value.get()) : nullptr;
        const auto* object = field && field->object ? as<IdentifierExpr>(field->object.get()) : nullptr;
        if (!field || !object || object->name != "Color" || field->field != "RED") {
            fail_test("enum member access expr", "unexpected AST");
        } else {
            pass_test("enum member access expr");
        }
    }
    {
        const Program program = parse("p.name = \"Emir\"\n");
        const auto* stmt = program.statements.size() == 1 ? as_stmt<ExprStmt>(program.statements[0].get()) : nullptr;
        const auto* assign = stmt && stmt->expression ? as<AssignExpr>(stmt->expression.get()) : nullptr;
        const auto* field = assign && assign->target ? as<FieldAccessExpr>(assign->target.get()) : nullptr;
        if (!assign || !field || field->field != "name") {
            fail_test("field assignment expr", "unexpected AST");
        } else {
            pass_test("field assignment expr");
        }
    }

    expect_error("duplicate struct field", "struct Player:\n    name\n    name\n", "duplicate field", 3, 5);
    expect_error("duplicate enum member", "enum Color:\n    RED\n    RED\n", "duplicate enum member", 3, 5);

    {
        const Program program = parse("struct Player:\n    pub name\n    private secret\n    prot score\n");
        const auto* stmt = as_stmt<StructStmt>(program.statements.size() == 1 ? program.statements[0].get() : nullptr);
        if (!stmt || stmt->fields.size() != 3 || stmt->fields[0].access != MemberAccess::Public ||
            stmt->fields[1].access != MemberAccess::Private || stmt->fields[2].access != MemberAccess::Protected) {
            fail_test("struct access modifiers", "unexpected field access metadata");
        } else {
            pass_test("struct access modifiers");
        }
    }
    {
        const Program program = parse("struct Player:\n    rating\n");
        const auto* stmt = as_stmt<StructStmt>(program.statements[0].get());
        if (!stmt || stmt->fields.empty() || stmt->fields[0].access != MemberAccess::Public) {
            fail_test("default public field access", "expected public default");
        } else {
            pass_test("default public field access");
        }
    }
    {
        const Program program = parse("pass\n");
        if (!dynamic_cast<const PassStmt*>(program.statements[0].get())) {
            fail_test("pass statement", "expected PassStmt");
        } else {
            pass_test("pass statement");
        }
    }
    {
        const Program program = parse("if true:\n    pass\n");
        const auto* if_stmt = as_stmt<IfStmt>(program.statements[0].get());
        const auto* pass_stmt = if_stmt && if_stmt->body.size() == 1 ? as_stmt<PassStmt>(if_stmt->body[0].get()) : nullptr;
        if (!pass_stmt) {
            fail_test("pass in if block", "expected PassStmt in if body");
        } else {
            pass_test("pass in if block");
        }
    }
    {
        const Program program = parse("func noop():\n    pass\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements[0].get());
        const auto* pass_stmt = fn && fn->body.size() == 1 ? as_stmt<PassStmt>(fn->body[0].get()) : nullptr;
        if (!pass_stmt) {
            fail_test("pass in function", "expected PassStmt in function body");
        } else {
            pass_test("pass in function");
        }
    }

    expect_error("pass as expression", "x = pass\n", "invalid expression", 1, 5);

    {
        const Program program = parse("class Player:\n    string name\n    int score\n");
        const auto* class_stmt = as_stmt<ClassStmt>(program.statements[0].get());
        if (!class_stmt || class_stmt->name != "Player" || class_stmt->fields.size() != 2) {
            fail_test("class declaration", "unexpected AST");
        } else {
            pass_test("class declaration");
        }
    }
    {
        const Program program = parse("class Player:\n    constructor(name):\n        pass\n    func get():\n        pass\n");
        const auto* class_stmt = as_stmt<ClassStmt>(program.statements[0].get());
        if (!class_stmt || !class_stmt->constructor || class_stmt->methods.size() != 1 ||
            class_stmt->constructor->parameters.size() != 1 || class_stmt->methods[0]->name != "get") {
            fail_test("class constructor and method", "unexpected AST");
        } else {
            pass_test("class constructor and method");
        }
    }
    {
        const Program program = parse("class FootballPlayer : Player:\n    int number\n");
        const auto* class_stmt = as_stmt<ClassStmt>(program.statements[0].get());
        if (!class_stmt || class_stmt->name != "FootballPlayer" || !class_stmt->parent_name ||
            *class_stmt->parent_name != "Player" || class_stmt->fields.size() != 1) {
            fail_test("class inheritance", "unexpected AST");
        } else {
            pass_test("class inheritance");
        }
    }
    {
        const Program program = parse("class Child : Base {\n    int value\n}\n");
        const auto* class_stmt = as_stmt<ClassStmt>(program.statements[0].get());
        if (!class_stmt || !class_stmt->parent_name || *class_stmt->parent_name != "Base") {
            fail_test("class inheritance brace", "unexpected AST");
        } else {
            pass_test("class inheritance brace");
        }
    }
    {
        const Program program = parse("class Child : Entity, Printable:\n    pass\n");
        const auto* class_stmt = as_stmt<ClassStmt>(program.statements[0].get());
        if (!class_stmt || class_stmt->base_type_specs.size() != 2 || class_stmt->base_type_specs[0] != "Entity" ||
            class_stmt->base_type_specs[1] != "Printable") {
            fail_test("class multiple bases parse", "unexpected AST");
        } else {
            pass_test("class multiple bases parse");
        }
    }
    expect_error("invalid dual colon inheritance", "class Child : A : B:\n    pass\n", "missing indent", 1, 19);
    {
        const Program program = parse("[a, b] = [1, 2]\n");
        const auto* stmt = as_stmt<DestructureAssignStmt>(program.statements[0].get());
        if (!stmt || !stmt->pattern || stmt->pattern->kind != DestructPatternKind::List ||
            stmt->pattern->elements.size() != 2) {
            fail_test("destructure list parse", "unexpected AST");
        } else {
            pass_test("destructure list parse");
        }
    }
    {
        const Program program = parse("(a, b) = (1, 2)\n");
        const auto* stmt = as_stmt<DestructureAssignStmt>(program.statements[0].get());
        if (!stmt || !stmt->pattern || stmt->pattern->kind != DestructPatternKind::Tuple ||
            stmt->pattern->elements.size() != 2) {
            fail_test("destructure tuple parse", "unexpected AST");
        } else {
            pass_test("destructure tuple parse");
        }
    }
    {
        const Program program = parse("[a, (b, c)] = [1, (2, 3)]\n");
        const auto* stmt = as_stmt<DestructureAssignStmt>(program.statements[0].get());
        const auto* nested =
            stmt && stmt->pattern && stmt->pattern->elements.size() == 2 && stmt->pattern->elements[1]
                ? stmt->pattern->elements[1].get()
                : nullptr;
        if (!stmt || !stmt->pattern || stmt->pattern->kind != DestructPatternKind::List || !nested ||
            nested->kind != DestructPatternKind::Tuple || nested->elements.size() != 2) {
            fail_test("destructure nested parse", "unexpected AST");
        } else {
            pass_test("destructure nested parse");
        }
    }

    {
        const Program program = parse("[_, b, _] = [1, 2, 3]\n");
        const auto* stmt = as_stmt<DestructureAssignStmt>(program.statements[0].get());
        const bool ok = stmt && stmt->pattern && stmt->pattern->kind == DestructPatternKind::List &&
                        stmt->pattern->elements.size() == 3 && stmt->pattern->elements[0] &&
                        stmt->pattern->elements[0]->kind == DestructPatternKind::Wildcard &&
                        stmt->pattern->elements[1] &&
                        stmt->pattern->elements[1]->kind == DestructPatternKind::Identifier &&
                        stmt->pattern->elements[1]->name == "b" && stmt->pattern->elements[2] &&
                        stmt->pattern->elements[2]->kind == DestructPatternKind::Wildcard;
        if (!ok) {
            fail_test("destructure wildcard parse", "unexpected AST");
        } else {
            pass_test("destructure wildcard parse");
        }
    }
    {
        const Program program = parse("[a, *rest] = [1, 2, 3]\n");
        const auto* stmt = as_stmt<DestructureAssignStmt>(program.statements[0].get());
        const bool ok = stmt && stmt->pattern && stmt->pattern->elements.size() == 2 &&
                        stmt->pattern->elements[1] &&
                        stmt->pattern->elements[1]->kind == DestructPatternKind::Rest &&
                        stmt->pattern->elements[1]->name == "rest";
        if (!ok) {
            fail_test("destructure rest parse", "unexpected AST");
        } else {
            pass_test("destructure rest parse");
        }
    }
    {
        const Program program = parse("[a, (_, c)] = [10, (20, 30)]\n");
        const auto* stmt = as_stmt<DestructureAssignStmt>(program.statements[0].get());
        const auto* nested =
            stmt && stmt->pattern && stmt->pattern->elements.size() == 2 && stmt->pattern->elements[1]
                ? stmt->pattern->elements[1].get()
                : nullptr;
        const bool ok = nested && nested->kind == DestructPatternKind::Tuple && nested->elements.size() == 2 &&
                        nested->elements[0] && nested->elements[0]->kind == DestructPatternKind::Wildcard &&
                        nested->elements[1] && nested->elements[1]->kind == DestructPatternKind::Identifier &&
                        nested->elements[1]->name == "c";
        if (!ok) {
            fail_test("destructure nested wildcard parse", "unexpected AST");
        } else {
            pass_test("destructure nested wildcard parse");
        }
    }
    expect_error("destructure rest first", "[*a, b] = [1, 2]", "rest pattern must be last", 1, 4);
    expect_error("destructure multiple rest", "[a, *r, *s] = [1, 2, 3]", "rest pattern must be last", 1, 7);
    expect_error("destructure rest wildcard", "[*_] = [1]", "invalid destructuring pattern", 1, 3);
    {
        const Program program = parse("{\"name\": n, \"age\": a} = d\n");
        const auto* stmt = as_stmt<DestructureAssignStmt>(program.statements[0].get());
        const bool ok = stmt && stmt->pattern && stmt->pattern->kind == DestructPatternKind::Dict &&
                        stmt->pattern->dict_entries.size() == 2;
        if (!ok) {
            fail_test("destructure dict parse", "unexpected AST");
        } else {
            pass_test("destructure dict parse");
        }
    }
    {
        const Program program = parse("Player{name, age} = p\n");
        const auto* stmt = as_stmt<DestructureAssignStmt>(program.statements[0].get());
        const bool ok = stmt && stmt->pattern && stmt->pattern->kind == DestructPatternKind::Struct &&
                        stmt->pattern->struct_type_name == "Player" && stmt->pattern->elements.size() == 2;
        if (!ok) {
            fail_test("destructure struct parse", "unexpected AST");
        } else {
            pass_test("destructure struct parse");
        }
    }
    {
        const Program program = parse("func add((a, b)):\n    return a + b\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements[0].get());
        const bool ok = fn && fn->parameters.size() == 1 && fn->parameters[0].pattern &&
                        fn->parameters[0].pattern->kind == DestructPatternKind::Tuple;
        if (!ok) {
            fail_test("destructure param parse", "unexpected AST");
        } else {
            pass_test("destructure param parse");
        }
    }

    expect_error("duplicate class constructor", "class Player:\n    constructor():\n        pass\n    constructor():\n        pass\n",
                 "duplicate constructor", 4, 5);

    {
        const Program program = parse("list<int> values = []");
        const AssignStmt* stmt = only_assign(program);
        if (!stmt || stmt->annotation != TypeAnnotationKind::Prefix || !stmt->type_name ||
            *stmt->type_name != "list<int>" || stmt->name != "values") {
            fail_test("generic list type parse", "unexpected AST");
        } else {
            pass_test("generic list type parse");
        }
    }
    {
        const Program program = parse("map<string, int> scores = {}");
        const AssignStmt* stmt = only_assign(program);
        if (!stmt || !stmt->type_name || *stmt->type_name != "map<string,int>") {
            fail_test("generic map type parse", "unexpected AST");
        } else {
            pass_test("generic map type parse");
        }
    }
    {
        const Program program = parse("list<list<int>> matrix");
        const AssignStmt* stmt = only_assign(program);
        if (!stmt || !stmt->type_name || *stmt->type_name != "list<list<int>>") {
            fail_test("nested generic type parse", "unexpected AST");
        } else {
            pass_test("nested generic type parse");
        }
    }
    {
        const Program program = parse("func sum(list<int> values):\n    pass\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements[0].get());
        if (!fn || fn->parameters.size() != 1 || !fn->parameters[0].type_name ||
            *fn->parameters[0].type_name != "list<int>") {
            fail_test("generic function param parse", "unexpected AST");
        } else {
            pass_test("generic function param parse");
        }
    }
    {
        const Program program = parse("print(a < b)");
        const auto* print = as_stmt<PrintStmt>(program.statements[0].get());
        const auto* binary = print && print->expressions.size() == 1
                                 ? as<BinaryExpr>(print->expressions[0].get())
                                 : nullptr;
        if (!binary || binary->op != TokenKind::Less) {
            fail_test("comparison not generic parse", "expected Less operator");
        } else {
            pass_test("comparison not generic parse");
        }
    }

    {
        const Program program = parse("try:\n    throw \"error\"\ncatch error:\n    print(error)\n");
        const auto* try_stmt = as_stmt<TryStmt>(program.statements[0].get());
        if (!try_stmt || try_stmt->try_body.size() != 1 || try_stmt->catches.size() != 1 ||
            try_stmt->catches[0].binding != "error" || try_stmt->catches[0].type_name.has_value() ||
            try_stmt->catches[0].body.size() != 1 || try_stmt->finally_body) {
            fail_test("try catch parse", "unexpected AST");
        } else {
            pass_test("try catch parse");
        }
    }
    {
        const Program program =
            parse("try:\n    throw MyError()\ncatch MyError error:\n    print(error)\ncatch Exception error:\n"
                  "    print(error)\n");
        const auto* try_stmt = as_stmt<TryStmt>(program.statements[0].get());
        if (!try_stmt || try_stmt->catches.size() != 2 || try_stmt->catches[0].type_name != "MyError" ||
            try_stmt->catches[0].binding != "error" || try_stmt->catches[1].type_name != "Exception" ||
            try_stmt->catches[1].binding != "error") {
            fail_test("typed catch parse", "unexpected AST");
        } else {
            pass_test("typed catch parse");
        }
    }
    {
        const Program program = parse("try:\n    print(1)\nfinally:\n    print(2)\n");
        const auto* try_stmt = as_stmt<TryStmt>(program.statements[0].get());
        if (!try_stmt || !try_stmt->catches.empty() || !try_stmt->finally_body ||
            try_stmt->finally_body->size() != 1) {
            fail_test("try finally parse", "unexpected AST");
        } else {
            pass_test("try finally parse");
        }
    }
    expect_error("try without catch or finally", "try:\n    print(1)\n", "try requires catch or finally", 1, 1);
    expect_error("catch without variable", "try:\n    print(1)\ncatch:\n    print(2)\n", "expected catch variable name",
                 3, 1);
    expect_error("throw missing expression", "throw\n", "expected expression after throw", 1, 1);

    {
        const Program program = parse("import math\n");
        const auto* import_stmt = as_stmt<ImportStmt>(program.statements[0].get());
        if (!import_stmt || import_stmt->from_import || import_stmt->module != "math" || import_stmt->module_alias ||
            !import_stmt->bindings.empty()) {
            fail_test("import math parse", "unexpected AST");
        } else {
            pass_test("import math parse");
        }
    }
    {
        const Program program = parse("import math as m\n");
        const auto* import_stmt = as_stmt<ImportStmt>(program.statements[0].get());
        if (!import_stmt || import_stmt->from_import || import_stmt->module != "math" || !import_stmt->module_alias ||
            *import_stmt->module_alias != "m") {
            fail_test("import math as m parse", "unexpected AST");
        } else {
            pass_test("import math as m parse");
        }
    }
    {
        const Program program = parse("from math import add\n");
        const auto* import_stmt = as_stmt<ImportStmt>(program.statements[0].get());
        if (!import_stmt || !import_stmt->from_import || import_stmt->module != "math" || import_stmt->bindings.size() != 1 ||
            import_stmt->bindings[0].name != "add" || import_stmt->bindings[0].alias) {
            fail_test("from math import add parse", "unexpected AST");
        } else {
            pass_test("from math import add parse");
        }
    }
    {
        const Program program = parse("from math import add as plus, subtract\n");
        const auto* import_stmt = as_stmt<ImportStmt>(program.statements[0].get());
        if (!import_stmt || !import_stmt->from_import || import_stmt->bindings.size() != 2 ||
            import_stmt->bindings[0].name != "add" || !import_stmt->bindings[0].alias ||
            *import_stmt->bindings[0].alias != "plus" || import_stmt->bindings[1].name != "subtract" ||
            import_stmt->bindings[1].alias) {
            fail_test("from math import multiple parse", "unexpected AST");
        } else {
            pass_test("from math import multiple parse");
        }
    }
    {
        const Program program = parse("import utils.math\n");
        const auto* import_stmt = as_stmt<ImportStmt>(program.statements[0].get());
        if (!import_stmt || import_stmt->module != "utils.math") {
            fail_test("import dotted module parse", "unexpected AST");
        } else {
            pass_test("import dotted module parse");
        }
    }
    expect_error("from import missing names", "from math import\n", "expected import name", 2, 1);
    expect_error("import missing module name", "import\n", "missing expected token 'module name'", 2, 1);

    {
        const Program program = parse("async func foo():\n    return 1\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements[0].get());
        if (!fn || !fn->is_async || fn->name != "foo") {
            fail_test("async function parse", "unexpected AST");
        } else {
            pass_test("async function parse");
        }
    }
    {
        const Program program = parse("async func foo():\n    x = await bar()\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements[0].get());
        const auto* assign = fn && !fn->body.empty() ? as_stmt<AssignStmt>(fn->body[0].get()) : nullptr;
        const auto* await_expr = assign && assign->value ? as<AwaitExpr>(assign->value.get()) : nullptr;
        if (!await_expr || !await_expr->operand || assign->name != "x") {
            fail_test("await expression parse", "unexpected AST");
        } else {
            pass_test("await expression parse");
        }
    }
    expect_error("await outside async function", "func main():\n    await foo()\n", "await outside async function", 2, 5);
    {
        const Program program = parse("async func foo():\n    return 1\nawait foo()\n");
        if (program.statements.size() < 2) {
            fail_test("top-level await parse", "unexpected AST");
        } else {
            pass_test("top-level await parse");
        }
    }
    expect_error("async constructor rejection", "class Foo:\n    async constructor()\n        pass\n",
                  "async constructor is not supported", 2, 5);
    {
        const Program program = parse("class Worker:\n    async func run():\n        return 1\n");
        const auto* cls = as_stmt<ClassStmt>(program.statements.empty() ? nullptr : program.statements[0].get());
        const FuncStmt* method = cls && !cls->methods.empty() ? cls->methods[0].get() : nullptr;
        if (!method || !method->is_async || method->name != "run") {
            fail_test("async class method parse", "unexpected AST");
        } else {
            pass_test("async class method parse");
        }
    }
    {
        const Program program = parse("@deprecated\nfunc old():\n    pass\n");
        const auto* fn = as_stmt<FuncStmt>(program.statements[0].get());
        if (!fn || fn->name != "old" || fn->annotations.size() != 1 || fn->annotations[0].name != "deprecated") {
            fail_test("top-level annotated function parse", "unexpected AST");
        } else {
            pass_test("top-level annotated function parse");
        }
    }

    {
        try {
            const Program program = parse("pub func greet():\n    return 1\n");
            const FuncStmt* fn = nullptr;
            for (const auto& stmt : program.statements) {
                fn = as_stmt<FuncStmt>(stmt.get());
                if (fn != nullptr) {
                    break;
                }
            }
            if (!fn || fn->name != "greet" || fn->access != MemberAccess::Public) {
                fail_test("pub func module declaration", "unexpected AST");
            } else {
                pass_test("pub func module declaration");
            }
        } catch (const ParserError& ex) {
            fail_test("pub func module declaration", ex.message());
        }
    }

    {
        try {
            const Program program = parse("public func greet():\n    return 1\n");
            const FuncStmt* fn = as_stmt<FuncStmt>(program.statements.front().get());
            if (!fn || fn->access != MemberAccess::Public) {
                fail_test("public func module declaration", "unexpected AST");
            } else {
                pass_test("public func module declaration");
            }
        } catch (const ParserError& ex) {
            fail_test("public func module declaration", ex.message());
        }
    }

    {
        try {
            (void)parse("func greet():\n    return 1\n");
            pass_test("plain func module declaration");
        } catch (const ParserError& ex) {
            fail_test("plain func module declaration", ex.message());
        }
    }

    {
        bool threw = false;
        try {
            (void)parse("pub 123 = 1\n");
        } catch (const ParserError&) {
            threw = true;
        }
        if (!threw) {
            fail_test("pub invalid declaration rejected", "expected parse failure");
        } else {
            pass_test("pub invalid declaration rejected");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All parser tests passed\n";
    return 0;
}
