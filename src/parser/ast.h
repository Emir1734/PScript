#pragma once

#include "lexer/token.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pscript {

struct Location {
    int line = 1;
    int column = 1;
};

inline Location location_from(const Token& token) {
    return Location{token.line, token.column};
}

struct Node {
    virtual ~Node() = default;
    Location loc;
};

struct Expr : Node {};
struct Stmt : Node {};

struct LiteralExpr final : Expr {
    enum class Kind { Integer, Float, Duration, String, True, False, Null };

    Kind kind = Kind::Null;
    std::string lexeme;
};

struct IdentifierExpr final : Expr {
    std::string name;
};

struct UnaryExpr final : Expr {
    TokenKind op = TokenKind::Minus;
    std::unique_ptr<Expr> operand;
};

struct BinaryExpr final : Expr {
    std::unique_ptr<Expr> left;
    TokenKind op = TokenKind::Plus;
    std::unique_ptr<Expr> right;
};

struct ListExpr final : Expr {
    std::vector<std::unique_ptr<Expr>> elements;
};

struct DictEntry {
    std::unique_ptr<Expr> key;
    std::unique_ptr<Expr> value;
};

struct DictExpr final : Expr {
    std::vector<DictEntry> entries;
};

struct TupleExpr final : Expr {
    std::vector<std::unique_ptr<Expr>> elements;
};

struct SetExpr final : Expr {
    std::vector<std::unique_ptr<Expr>> elements;
};

struct IndexExpr final : Expr {
    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
};

struct MethodCallExpr final : Expr {
    std::unique_ptr<Expr> object;
    std::string method;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct FieldAccessExpr final : Expr {
    std::unique_ptr<Expr> object;
    std::string field;
};

struct CallExpr final : Expr {
    std::string name;
    std::vector<std::string> type_arguments;
    std::vector<std::unique_ptr<Expr>> arguments;
};

struct MembershipExpr final : Expr {
    std::unique_ptr<Expr> value;
    std::unique_ptr<Expr> container;
    bool negated = false;
};

struct TernaryExpr final : Expr {
    std::unique_ptr<Expr> condition;
    std::unique_ptr<Expr> then_expr;
    std::unique_ptr<Expr> else_expr;
};

struct AwaitExpr final : Expr {
    std::unique_ptr<Expr> operand;
};

struct PropagateExpr final : Expr {
    std::unique_ptr<Expr> operand;
};

struct AssignExpr final : Expr {
    std::unique_ptr<Expr> target;
    TokenKind op = TokenKind::Equal;
    std::unique_ptr<Expr> value;
};

struct UpdateExpr final : Expr {
    std::unique_ptr<Expr> target;
    TokenKind op = TokenKind::PlusPlus;
    bool prefix = false;
};

enum class TypeAnnotationKind { None, Prefix, Colon };

enum class MemberAccess { Public, Protected, Private };

struct AssignStmt final : Stmt {
    bool is_const = false;
    TypeAnnotationKind annotation = TypeAnnotationKind::None;
    std::optional<std::string> type_name;
    std::string name;
    std::unique_ptr<Expr> value;
    MemberAccess access = MemberAccess::Public;
};

enum class DestructPatternKind { Identifier, Wildcard, Rest, List, Tuple, Dict, Struct, Positional, Literal };

struct DestructPattern;

struct DestructDictEntry {
    std::string key;
    Location loc;
    std::unique_ptr<DestructPattern> pattern;
};

struct DestructPattern {
    DestructPatternKind kind = DestructPatternKind::Identifier;
    Location loc;
    std::string name;
    std::string struct_field_name;
    std::string struct_type_name;
    std::unique_ptr<Expr> literal_expr;
    std::vector<std::unique_ptr<DestructPattern>> elements;
    std::vector<DestructDictEntry> dict_entries;
    std::unique_ptr<DestructPattern> dict_rest;
};

struct AnnotationArg {
    std::optional<std::string> key;
    std::unique_ptr<Expr> value;
    AnnotationArg() = default;
    AnnotationArg(AnnotationArg&&) = default;
    AnnotationArg& operator=(AnnotationArg&&) = default;
    AnnotationArg(const AnnotationArg&) = delete;
    AnnotationArg& operator=(const AnnotationArg&) = delete;
};

struct Annotation {
    std::string name;
    std::vector<AnnotationArg> args;
    Location loc;
    Annotation() = default;
    Annotation(Annotation&&) = default;
    Annotation& operator=(Annotation&&) = default;
    Annotation(const Annotation&) = delete;
    Annotation& operator=(const Annotation&) = delete;
};

struct GenericTypeParam {
    std::string name;
    std::optional<std::string> constraint_type_name;
    std::vector<std::string> constraint_type_names;
    Location loc;
};

struct Parameter {
    std::optional<std::string> type_name;
    std::string name;
    Location loc;
    std::unique_ptr<DestructPattern> pattern;
    std::unique_ptr<Expr> default_value;
    bool is_variadic = false;
    std::vector<Annotation> annotations;
};

struct DestructureAssignStmt final : Stmt {
    std::unique_ptr<DestructPattern> pattern;
    std::unique_ptr<Expr> value;
};

struct PrintStmt final : Stmt {
    bool parenthesized = false;
    std::vector<std::unique_ptr<Expr>> expressions;
};

struct IndexAssignStmt final : Stmt {
    std::unique_ptr<Expr> object;
    std::unique_ptr<Expr> index;
    std::unique_ptr<Expr> value;
};

struct ExprStmt final : Stmt {
    std::unique_ptr<Expr> expression;
};

struct ElifBranch {
    Location loc;
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct IfStmt final : Stmt {
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> body;
    std::vector<ElifBranch> elif_branches;
    std::optional<std::vector<std::unique_ptr<Stmt>>> else_body;
};

struct WhileStmt final : Stmt {
    std::unique_ptr<Expr> condition;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct ForStmt final : Stmt {
    std::string item;
    std::unique_ptr<Expr> iterable;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct AsyncForStmt final : Stmt {
    std::string item;
    std::unique_ptr<Expr> iterable;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct AsyncWithStmt final : Stmt {
    std::unique_ptr<Expr> manager;
    std::optional<std::string> binding;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct BreakStmt final : Stmt {};
struct ContinueStmt final : Stmt {};

struct CatchClause {
    Location loc;
    std::optional<std::string> type_name;
    std::string binding;
    std::vector<std::unique_ptr<Stmt>> body;
};

struct TryStmt final : Stmt {
    std::vector<std::unique_ptr<Stmt>> try_body;
    std::vector<CatchClause> catches;
    std::optional<std::vector<std::unique_ptr<Stmt>>> finally_body;
};

struct ThrowStmt final : Stmt {
    std::unique_ptr<Expr> value;
};

enum class MatchPatternKind {
    Wildcard,
    Literal,
    Literals,
    Binding,
    TypeBind,
    Destruct,
    Or,
    And,
    EnumVariant,
};

struct MatchPattern {
    MatchPatternKind kind = MatchPatternKind::Wildcard;
    Location loc;
    std::unique_ptr<Expr> literal;
    std::vector<std::unique_ptr<Expr>> literals;
    std::optional<std::string> binding;
    std::optional<std::string> type_name;
    std::unique_ptr<DestructPattern> destruct;
    std::vector<std::unique_ptr<MatchPattern>> parts;
    std::optional<std::string> enum_type;
    std::optional<std::string> enum_variant;
    std::vector<std::string> enum_bindings;
};

struct DeferStmt final : Stmt {
    std::unique_ptr<Expr> expression;
};

struct MatchCase {
    Location loc;
    std::unique_ptr<MatchPattern> pattern;
    std::unique_ptr<Expr> guard;
    std::vector<std::unique_ptr<Stmt>> body;
    std::unique_ptr<Expr> result_expr;
};

struct MatchStmt final : Stmt {
    std::unique_ptr<Expr> value;
    std::vector<MatchCase> cases;
};

struct MatchExpr final : Expr {
    std::unique_ptr<Expr> value;
    std::vector<MatchCase> cases;
};

struct StructField {
    MemberAccess access = MemberAccess::Public;
    std::optional<std::string> type_name;
    std::string name;
    Location loc;
};

struct PassStmt final : Stmt {};

struct StructStmt final : Stmt {
    std::string name;
    std::vector<GenericTypeParam> type_params;
    std::vector<StructField> fields;
    bool is_record = false;
    MemberAccess access = MemberAccess::Public;
    std::vector<Annotation> annotations;
};

struct EnumStmt final : Stmt {
    std::string name;
    std::vector<std::string> members;
    std::vector<std::vector<std::string>> member_payloads;
    std::vector<std::vector<Annotation>> member_annotations;
    MemberAccess access = MemberAccess::Public;
    std::vector<Annotation> annotations;
};

struct TypeAliasStmt final : Stmt {
    std::string name;
    std::vector<GenericTypeParam> type_params;
    std::string target_type;
    MemberAccess access = MemberAccess::Public;
    std::vector<Annotation> annotations;
};

struct FuncStmt final : Stmt {
    std::string name;
    std::vector<GenericTypeParam> type_params;
    std::vector<Parameter> parameters;
    std::optional<std::string> return_type_name;
    std::vector<std::unique_ptr<Stmt>> body;
    MemberAccess access = MemberAccess::Public;
    bool is_async = false;
    std::vector<Annotation> annotations;
};

struct ClassProperty {
    std::string name;
    std::optional<std::string> type_name;
    MemberAccess getter_access = MemberAccess::Private;
    MemberAccess setter_access = MemberAccess::Private;
    std::unique_ptr<FuncStmt> getter;
    std::unique_ptr<FuncStmt> setter;
};

struct InterfaceMethodSig {
    std::string name;
    std::vector<Parameter> parameters;
    std::optional<std::string> return_type_name;
    bool is_async = false;
    Location loc;
};

struct InterfacePropertySig {
    std::string name;
    std::optional<std::string> type_name;
    bool has_getter = false;
    bool has_setter = false;
    Location loc;
};

struct InterfaceStmt final : Stmt {
    std::string name;
    std::vector<GenericTypeParam> type_params;
    std::vector<std::string> extends;
    std::vector<InterfaceMethodSig> methods;
    std::vector<InterfacePropertySig> properties;
    MemberAccess access = MemberAccess::Public;
    std::vector<Annotation> annotations;
};

struct ClassStmt final : Stmt {
    std::string name;
    std::vector<GenericTypeParam> type_params;
    std::optional<std::string> parent_name;
    std::vector<std::string> implements;
    std::vector<std::string> base_type_specs;
    std::vector<StructField> fields;
    std::vector<ClassProperty> properties;
    std::unique_ptr<FuncStmt> constructor;
    std::vector<std::unique_ptr<FuncStmt>> methods;
    MemberAccess access = MemberAccess::Public;
    std::vector<Annotation> annotations;
};

struct ReturnStmt final : Stmt {
    std::unique_ptr<Expr> value;
};

struct ImportBinding {
    std::string name;
    std::optional<std::string> alias;
    Location loc;
};

struct ImportStmt final : Stmt {
    std::string module;
    std::optional<std::string> module_alias;
    std::vector<ImportBinding> bindings;
    bool from_import = false;
};

struct Program final : Node {
    std::vector<std::unique_ptr<Stmt>> statements;
};

}  // namespace pscript
