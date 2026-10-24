#pragma once

#include "lexer/token.h"
#include "parser/ast.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pscript {

struct ModuleAccessPrefix {
    MemberAccess access = MemberAccess::Public;
    int column = 0;
};

class ParserError : public std::runtime_error {
public:
    ParserError(std::string message, int line, int column);

    int line() const { return line_; }
    int column() const { return column_; }
    const std::string& message() const { return message_; }

private:
    std::string message_;
    int line_;
    int column_;
};

class Parser {
public:
    explicit Parser(std::vector<Token> tokens);

    Program parse();

    static std::unique_ptr<Expr> parse_standalone_expression(const std::string& source, std::string& error);

private:
    const Token& peek(std::size_t ahead = 0) const;
    bool check(TokenKind kind, std::size_t ahead = 0) const;
    bool is_at_end() const;
    Token advance();
    Token expect(TokenKind kind, std::string_view what);

    std::unique_ptr<Stmt> parse_statement();
    std::unique_ptr<Stmt> parse_if_statement();
    std::unique_ptr<Stmt> parse_while_statement();
    std::unique_ptr<Stmt> parse_for_statement();
    std::unique_ptr<Stmt> parse_async_for_statement();
    std::unique_ptr<Stmt> parse_async_with_statement();
    std::unique_ptr<Stmt> parse_break_statement();
    std::unique_ptr<Stmt> parse_continue_statement();
    std::unique_ptr<Stmt> parse_match_statement();
    std::unique_ptr<Stmt> parse_function_statement(MemberAccess access = MemberAccess::Public,
                                                   std::vector<Annotation> leading_annotations = {},
                                                   int access_column = -1);
    std::unique_ptr<Stmt> parse_struct_statement(MemberAccess access = MemberAccess::Public,
                                                 std::vector<Annotation> leading_annotations = {},
                                                 int access_column = -1);
    std::unique_ptr<Stmt> parse_class_statement(MemberAccess access = MemberAccess::Public,
                                                std::vector<Annotation> leading_annotations = {},
                                                int access_column = -1);
    std::unique_ptr<Stmt> parse_enum_statement(MemberAccess access = MemberAccess::Public,
                                               std::vector<Annotation> leading_annotations = {},
                                               int access_column = -1);
    std::unique_ptr<Stmt> parse_typealias_statement(MemberAccess access = MemberAccess::Public,
                                                    std::vector<Annotation> leading_annotations = {},
                                                    int access_column = -1);
    std::unique_ptr<Stmt> parse_return_statement();
    std::unique_ptr<Stmt> parse_try_statement();
    std::unique_ptr<Stmt> parse_throw_statement();
    std::unique_ptr<Stmt> parse_import_statement();
    std::string parse_module_path();
    bool try_handler_belongs(const Token& try_tok) const;
    std::vector<StructField> parse_struct_body(int header_column);
    StructField parse_struct_field(std::unordered_set<std::string>& seen);
    void parse_class_body(ClassStmt& stmt, int header_column);
    StructField parse_class_field(std::unordered_set<std::string>& seen);
    ClassProperty parse_class_property(std::unordered_set<std::string>& seen_properties);
    std::unique_ptr<FuncStmt> parse_class_constructor(int header_column);
    std::unique_ptr<FuncStmt> parse_class_method(int header_column);
    std::optional<MemberAccess> parse_member_access_modifier();
    std::optional<ModuleAccessPrefix> try_parse_module_access_modifier();
    bool is_access_modifier(TokenKind kind) const;
    bool looks_like_property_member() const;
    bool is_module_declaration_after_access(std::size_t ahead) const;
    void parse_enum_body(int header_column, std::vector<std::string>& members,
                         std::vector<std::vector<std::string>>& member_payloads,
                         std::vector<std::vector<Annotation>>& member_annotations);
    std::vector<std::string> parse_enum_member_payload();
    std::unique_ptr<Stmt> parse_defer_statement();
    std::unique_ptr<Stmt> parse_print_statement();
    std::vector<Parameter> parse_parameters();
    std::unique_ptr<Stmt> parse_assign_statement();
    bool looks_like_type_prefix() const;
    bool looks_like_generic_instantiation_call() const;
    std::string parse_type_annotation_string();
    std::string parse_type_annotation_primary();
    std::vector<Annotation> parse_annotations();
    std::vector<GenericTypeParam> parse_generic_type_params();
    void consume_generic_close();
    bool pattern_type_followed_by(std::size_t name_ahead, TokenKind open) const;
    std::string parse_type_name_for_pattern();
    std::unique_ptr<Stmt> parse_interface_statement(MemberAccess access = MemberAccess::Public,
                                                    std::vector<Annotation> leading_annotations = {},
                                                    int access_column = -1);
    std::unique_ptr<Stmt> parse_expression_statement();
    bool looks_like_destructure_dict_pattern() const;
    bool looks_like_destructure_struct_pattern() const;
    bool looks_like_destructure_list_pattern() const;
    bool looks_like_destructure_tuple_pattern() const;
    bool skip_destructure_pattern_token(std::size_t& index) const;
    bool skip_destructure_dict_pattern_token(std::size_t& index) const;
    bool skip_destructure_struct_body_token(std::size_t& index) const;
    void validate_destructure_pattern(const DestructPattern& pattern);
    std::unique_ptr<DestructPattern> parse_destructure_pattern_element();
    std::unique_ptr<DestructPattern> parse_destructure_dict_pattern();
    std::unique_ptr<DestructPattern> parse_destructure_struct_pattern(const std::string& type_name,
                                                                        const Location& loc);
    std::unique_ptr<DestructPattern> parse_destructure_struct_pattern_element();
    std::unique_ptr<DestructPattern> parse_destructure_list_pattern();
    std::unique_ptr<DestructPattern> parse_destructure_tuple_pattern();
    std::unique_ptr<DestructPattern> parse_parameter_destructure_pattern();
    std::vector<std::unique_ptr<Expr>> parse_print_arguments(bool parenthesized);
    std::vector<std::unique_ptr<Expr>> parse_call_arguments();
    bool is_immediate_call_paren(const Token& callee) const;
    bool can_start_expression() const;
    std::unique_ptr<Expr> parse_if_condition();
    bool looks_like_dict() const;
    bool looks_like_ternary_question() const;
    bool can_start_expression_kind(TokenKind kind) const;
    bool can_start_whitespace_call_argument() const;
    bool is_whitespace_call_callee(std::string_view name) const;
    std::vector<std::unique_ptr<Stmt>> parse_block(int header_column);
    std::vector<std::unique_ptr<Stmt>> parse_indented_suite(int header_column, int opener_line);
    std::vector<std::unique_ptr<Stmt>> parse_brace_block(int header_column, int opener_line);

    std::vector<MatchCase> parse_match_cases_indented(int header_column, int opener_line, bool expression_form);
    std::vector<MatchCase> parse_match_cases_brace(int header_column, int opener_line, bool expression_form);
    MatchCase parse_match_case(int case_column, bool expression_form);
    std::unique_ptr<MatchPattern> parse_match_pattern_or();
    std::unique_ptr<MatchPattern> parse_match_pattern_and();
    std::unique_ptr<MatchPattern> parse_match_pattern_primary();
    std::unique_ptr<DestructPattern> parse_destructure_positional_pattern(const std::string& type_name,
                                                                          const Location& loc);
    std::unique_ptr<Expr> parse_match_expression();
    std::unique_ptr<Expr> parse_match_literal();

    bool is_elif() const;
    bool can_continue_branch(int header_column) const;

    std::unique_ptr<Expr> parse_expression();
    std::unique_ptr<Expr> parse_assignment();
    std::unique_ptr<Expr> parse_pipeline();
    std::unique_ptr<Expr> parse_ternary();
    std::unique_ptr<Expr> parse_null_coalesce();
    std::unique_ptr<Expr> parse_or();
    std::unique_ptr<Expr> parse_and();
    std::unique_ptr<Expr> parse_membership();
    std::unique_ptr<Expr> parse_comparison();
    std::unique_ptr<Expr> parse_bitwise_or();
    std::unique_ptr<Expr> parse_bitwise_xor();
    std::unique_ptr<Expr> parse_bitwise_and();
    std::unique_ptr<Expr> parse_shift();
    std::unique_ptr<Expr> parse_additive();
    std::unique_ptr<Expr> parse_multiplicative();
    std::unique_ptr<Expr> parse_unary();
    std::unique_ptr<Expr> parse_postfix();
    std::unique_ptr<Expr> parse_primary();
    std::unique_ptr<Expr> parse_paren_expr();
    std::unique_ptr<Expr> parse_list_literal();
    std::unique_ptr<Expr> parse_dict_literal();

    static bool is_comparison(TokenKind kind);
    static bool is_and(TokenKind kind);
    static bool is_or(TokenKind kind);
    static bool is_additive(TokenKind kind);
    static bool is_multiplicative(TokenKind kind);
    static bool is_assign_op(TokenKind kind);
    static bool is_lvalue(const Expr& expr);
    static std::unique_ptr<LiteralExpr> make_literal(LiteralExpr::Kind kind, const Token& token);

    [[noreturn]] void fail(std::string message) const;
    [[noreturn]] void fail(std::string message, const Token& token) const;
    [[noreturn]] void fail(std::string message, const Location& loc) const;

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    int function_depth_ = 0;
    int async_function_depth_ = 0;
};

Program parse(std::string source);

}  // namespace pscript
