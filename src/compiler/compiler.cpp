#include "compiler/compiler.h"
#include "compiler/embed_codegen.h"

#include "stdlib/stdlib.h"

#include "compiler/module_loader.h"
#include "lexer/duration_util.h"
#include "parser/parser.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pscript {
namespace {

std::string constant_key(const Constant& constant) {
    return std::to_string(static_cast<int>(constant.kind)) + "\n" + constant.lexeme;
}

bool is_number_type(StaticType type) {
    return type == StaticType::Integer || type == StaticType::Float;
}

bool is_logical_and(TokenKind op) {
    return op == TokenKind::And || op == TokenKind::AmpAmp;
}

bool is_logical_or(TokenKind op) {
    return op == TokenKind::Or || op == TokenKind::PipePipe;
}

bool is_null_coalesce(TokenKind op) {
    return op == TokenKind::NullCoalesce;
}

bool annotation_exports_symbol(const std::vector<Annotation>& annotations) {
    for (const Annotation& annotation : annotations) {
        if (annotation.name == "export") {
            return true;
        }
    }
    return false;
}

MemberAccess access_with_annotations(MemberAccess access, const std::vector<Annotation>& annotations) {
    if (annotation_exports_symbol(annotations)) {
        return MemberAccess::Public;
    }
    return access;
}

bool is_unary_not(TokenKind op) {
    return op == TokenKind::Not || op == TokenKind::Bang;
}

TokenKind compound_binary(TokenKind op) {
    switch (op) {
        case TokenKind::PlusEqual:
            return TokenKind::Plus;
        case TokenKind::MinusEqual:
            return TokenKind::Minus;
        case TokenKind::StarEqual:
            return TokenKind::Star;
        case TokenKind::SlashEqual:
            return TokenKind::Slash;
        case TokenKind::PercentEqual:
            return TokenKind::Percent;
        default:
            return op;
    }
}

StaticType combine_binary(StaticType left, StaticType right, TokenKind op) {
    switch (op) {
        case TokenKind::EqualEqual:
        case TokenKind::BangEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
            return StaticType::Bool;
        case TokenKind::And:
        case TokenKind::AmpAmp:
        case TokenKind::Or:
        case TokenKind::PipePipe:
            return StaticType::Unknown;
        case TokenKind::Percent:
            if (left == StaticType::Integer && right == StaticType::Integer) {
                return StaticType::Integer;
            }
            return StaticType::Unknown;
        case TokenKind::Amp:
        case TokenKind::Pipe:
        case TokenKind::Caret:
        case TokenKind::LessLess:
        case TokenKind::GreaterGreater:
            if (left == StaticType::Integer && right == StaticType::Integer) {
                return StaticType::Integer;
            }
            return StaticType::Unknown;
        case TokenKind::Plus:
            if (left == StaticType::String && right == StaticType::String) {
                return StaticType::String;
            }
            if (left == StaticType::Integer && right == StaticType::Integer) {
                return StaticType::Integer;
            }
            if (is_number_type(left) && is_number_type(right)) {
                return StaticType::Float;
            }
            return StaticType::Unknown;
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
            if (left == StaticType::Integer && right == StaticType::Integer) {
                return StaticType::Integer;
            }
            if (is_number_type(left) && is_number_type(right)) {
                return StaticType::Float;
            }
            return StaticType::Unknown;
        default:
            return StaticType::Unknown;
    }
}

std::string encode_string_lexeme(const std::string& value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out.push_back(c);
                break;
        }
    }
    out += '"';
    return out;
}

bool is_known_builtin(const std::string& name) {
    return name == "range" || name == "print" || name == "input" || name == "async_input" || name == "int" ||
           name == "float" ||
           name == "str" || name == "bool" || name == "type" || name == "type_of" || name == "annotations" ||
           name == "members" || name == "delay" || name == "race" || name == "timeout" || name == "wait_all" ||
           name == "wait_any" || name == "abs" ||
           name == "min" || name == "max" || name == "round" || name == "clamp" || name == "filter" ||
           name == "map" || name == "sort_by" || name == "group_by" || name == "take" || name == "all" ||
           name == "parallel_map";
}

bool builtin_returns_value(const std::string& name) {
    return name != "print";
}

const Stmt* last_executable_stmt(const std::vector<std::unique_ptr<Stmt>>& statements) {
    const Stmt* last = nullptr;
    for (const auto& stmt : statements) {
        if (!stmt || dynamic_cast<const FuncStmt*>(stmt.get())) {
            continue;
        }
        last = stmt.get();
    }
    return last;
}

bool expr_returns_value(const Expr& expr, const std::function<bool(const std::string&)>& function_returns);

bool expr_returns_value(const Expr& expr, const std::function<bool(const std::string&)>& function_returns) {
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (is_known_builtin(call->name)) {
            return builtin_returns_value(call->name);
        }
        return function_returns(call->name);
    }
    if (dynamic_cast<const MethodCallExpr*>(&expr)) {
        return true;
    }
    return true;
}

bool function_body_returns_value(const FuncStmt& fn,
                                 const std::function<bool(const std::string&)>& function_returns) {
    const Stmt* last = last_executable_stmt(fn.body);
    if (!last) {
        return false;
    }
    if (const auto* ret = dynamic_cast<const ReturnStmt*>(last)) {
        return ret->value != nullptr;
    }
    if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(last)) {
        if (!expr_stmt->expression) {
            return false;
        }
        return expr_returns_value(*expr_stmt->expression, function_returns);
    }
    return false;
}

void collect_pattern_names(const DestructPattern& pattern, std::vector<std::string>& names) {
    if (pattern.kind == DestructPatternKind::Wildcard) {
        return;
    }
    if (pattern.kind == DestructPatternKind::Identifier || pattern.kind == DestructPatternKind::Rest) {
        names.push_back(pattern.name);
        return;
    }
    if (pattern.kind == DestructPatternKind::Dict) {
        for (const DestructDictEntry& entry : pattern.dict_entries) {
            if (entry.pattern) {
                collect_pattern_names(*entry.pattern, names);
            }
        }
        if (pattern.dict_rest) {
            collect_pattern_names(*pattern.dict_rest, names);
        }
        return;
    }
    for (const auto& element : pattern.elements) {
        if (element) {
            collect_pattern_names(*element, names);
        }
    }
}

std::string struct_pattern_field_name(const DestructPattern& pattern) {
    if (!pattern.struct_field_name.empty()) {
        return pattern.struct_field_name;
    }
    return pattern.name;
}

struct DestructCollectionShape {
    bool has_rest = false;
    std::size_t fixed_slots = 0;
};

DestructCollectionShape analyze_collection_pattern(const DestructPattern& pattern) {
    DestructCollectionShape shape;
    if (pattern.kind != DestructPatternKind::List && pattern.kind != DestructPatternKind::Tuple) {
        return shape;
    }
    for (const auto& element : pattern.elements) {
        if (!element) {
            continue;
        }
        if (element->kind == DestructPatternKind::Rest) {
            shape.has_rest = true;
            break;
        }
        ++shape.fixed_slots;
    }
    return shape;
}

bool expr_contains_await(const Expr& expr) {
    if (dynamic_cast<const AwaitExpr*>(&expr) != nullptr) {
        return true;
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        return (binary->left && expr_contains_await(*binary->left)) ||
               (binary->right && expr_contains_await(*binary->right));
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return unary->operand && expr_contains_await(*unary->operand);
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        for (const auto& arg : call->arguments) {
            if (arg && expr_contains_await(*arg)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* method = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (method->object && expr_contains_await(*method->object)) {
            return true;
        }
        for (const auto& arg : method->arguments) {
            if (arg && expr_contains_await(*arg)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* assign = dynamic_cast<const AssignExpr*>(&expr)) {
        return (assign->target && expr_contains_await(*assign->target)) ||
               (assign->value && expr_contains_await(*assign->value));
    }
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expr)) {
        return field->object && expr_contains_await(*field->object);
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr)) {
        return (index->object && expr_contains_await(*index->object)) ||
               (index->index && expr_contains_await(*index->index));
    }
    if (const auto* update = dynamic_cast<const UpdateExpr*>(&expr)) {
        return update->target && expr_contains_await(*update->target);
    }
    if (const auto* match = dynamic_cast<const MatchExpr*>(&expr)) {
        if (match->value && expr_contains_await(*match->value)) {
            return true;
        }
        for (const MatchCase& arm : match->cases) {
            if (arm.guard && expr_contains_await(*arm.guard)) {
                return true;
            }
            if (arm.result_expr && expr_contains_await(*arm.result_expr)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* list = dynamic_cast<const ListExpr*>(&expr)) {
        for (const auto& element : list->elements) {
            if (element && expr_contains_await(*element)) {
                return true;
            }
        }
    }
    if (const auto* dict = dynamic_cast<const DictExpr*>(&expr)) {
        for (const auto& entry : dict->entries) {
            if (entry.key && expr_contains_await(*entry.key)) {
                return true;
            }
            if (entry.value && expr_contains_await(*entry.value)) {
                return true;
            }
        }
    }
    return false;
}

bool stmt_contains_await(const Stmt& stmt) {
    if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(&stmt)) {
        return expr_stmt->expression && expr_contains_await(*expr_stmt->expression);
    }
    if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
        return assign->value && expr_contains_await(*assign->value);
    }
    if (const auto* print = dynamic_cast<const PrintStmt*>(&stmt)) {
        for (const auto& arg : print->expressions) {
            if (arg && expr_contains_await(*arg)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* ret = dynamic_cast<const ReturnStmt*>(&stmt)) {
        return ret->value && expr_contains_await(*ret->value);
    }
    if (const auto* if_stmt = dynamic_cast<const IfStmt*>(&stmt)) {
        auto scan_block = [](const std::vector<std::unique_ptr<Stmt>>& block) {
            for (const auto& inner : block) {
                if (inner && stmt_contains_await(*inner)) {
                    return true;
                }
            }
            return false;
        };
        if (if_stmt->condition && expr_contains_await(*if_stmt->condition)) {
            return true;
        }
        if (scan_block(if_stmt->body)) {
            return true;
        }
        for (const auto& branch : if_stmt->elif_branches) {
            if (branch.condition && expr_contains_await(*branch.condition)) {
                return true;
            }
            if (scan_block(branch.body)) {
                return true;
            }
        }
        if (if_stmt->else_body.has_value()) {
            return scan_block(*if_stmt->else_body);
        }
        return false;
    }
    if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(&stmt)) {
        if (while_stmt->condition && expr_contains_await(*while_stmt->condition)) {
            return true;
        }
        for (const auto& inner : while_stmt->body) {
            if (inner && stmt_contains_await(*inner)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* for_stmt = dynamic_cast<const ForStmt*>(&stmt)) {
        if (for_stmt->iterable && expr_contains_await(*for_stmt->iterable)) {
            return true;
        }
        for (const auto& inner : for_stmt->body) {
            if (inner && stmt_contains_await(*inner)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* async_for = dynamic_cast<const AsyncForStmt*>(&stmt)) {
        if (async_for->iterable && expr_contains_await(*async_for->iterable)) {
            return true;
        }
        for (const auto& inner : async_for->body) {
            if (inner && stmt_contains_await(*inner)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* async_with = dynamic_cast<const AsyncWithStmt*>(&stmt)) {
        if (async_with->manager && expr_contains_await(*async_with->manager)) {
            return true;
        }
        for (const auto& inner : async_with->body) {
            if (inner && stmt_contains_await(*inner)) {
                return true;
            }
        }
        return false;
    }
    if (const auto* try_stmt = dynamic_cast<const TryStmt*>(&stmt)) {
        auto scan_block = [](const std::vector<std::unique_ptr<Stmt>>& block) {
            for (const auto& inner : block) {
                if (inner && stmt_contains_await(*inner)) {
                    return true;
                }
            }
            return false;
        };
        if (scan_block(try_stmt->try_body)) {
            return true;
        }
        for (const auto& clause : try_stmt->catches) {
            if (scan_block(clause.body)) {
                return true;
            }
        }
        if (try_stmt->finally_body.has_value()) {
            return scan_block(*try_stmt->finally_body);
        }
        return false;
    }
    if (const auto* throw_stmt = dynamic_cast<const ThrowStmt*>(&stmt)) {
        return throw_stmt->value && expr_contains_await(*throw_stmt->value);
    }
    if (const auto* match_stmt = dynamic_cast<const MatchStmt*>(&stmt)) {
        if (match_stmt->value && expr_contains_await(*match_stmt->value)) {
            return true;
        }
        for (const MatchCase& arm : match_stmt->cases) {
            if (arm.guard && expr_contains_await(*arm.guard)) {
                return true;
            }
            if (arm.result_expr && expr_contains_await(*arm.result_expr)) {
                return true;
            }
            for (const auto& inner : arm.body) {
                if (inner && stmt_contains_await(*inner)) {
                    return true;
                }
            }
        }
        return false;
    }
    if (const auto* destructure = dynamic_cast<const DestructureAssignStmt*>(&stmt)) {
        return destructure->value && expr_contains_await(*destructure->value);
    }
    if (const auto* index_assign = dynamic_cast<const IndexAssignStmt*>(&stmt)) {
        return (index_assign->object && expr_contains_await(*index_assign->object)) ||
               (index_assign->index && expr_contains_await(*index_assign->index)) ||
               (index_assign->value && expr_contains_await(*index_assign->value));
    }
    return false;
}

bool is_module_level_declaration(const Stmt& stmt) {
    return dynamic_cast<const FuncStmt*>(&stmt) != nullptr || dynamic_cast<const StructStmt*>(&stmt) != nullptr ||
           dynamic_cast<const ClassStmt*>(&stmt) != nullptr || dynamic_cast<const EnumStmt*>(&stmt) != nullptr ||
           dynamic_cast<const InterfaceStmt*>(&stmt) != nullptr;
}

}  // namespace

Bytecode Compiler::compile(const Program& program) {
    const std::string saved_source_file = bytecode_.source_file;
    const int saved_source_line_base = source_line_base_;
    bytecode_ = {};
    bytecode_.source_file = saved_source_file;
    bytecode_.source_line_base = saved_source_line_base;
    source_line_base_ = saved_source_line_base;
    next_register_ = 0;
    next_local_ = 0;
    in_function_ = false;
    emitting_body_ = false;
    function_stack_.clear();
    constant_keys_.clear();
    variable_index_.clear();
    variable_types_.clear();
    variable_local_.clear();
    loop_stack_.clear();
    try_stack_.clear();
    scope_stack_.clear();
    class_context_.reset();
    class_call_keys_.clear();
    pending_class_parents_.clear();
    push_scope();
    append_root_async_classes(bytecode_);
    for (std::uint32_t ci = 0; ci < bytecode_.classes.size(); ++ci) {
        const ClassInfo& cls = bytecode_.classes[ci];
        if (cls.native_class_kind == 0) {
            continue;
        }
        if (cls.name != "TaskGroup" && cls.name != "AsyncStream") {
            continue;
        }
        if (lookup_symbol(cls.name)) {
            continue;
        }
        Symbol symbol;
        symbol.kind = SymbolKind::Class;
        symbol.class_index = ci;
        scope_stack_.back().symbols[cls.name] = symbol;
    }
    if (!root_bytecode_) {
        root_bytecode_ = &bytecode_;
    }
    if (module_cache_) {
        module_cache_->set_root(root_bytecode_);
    }

    std::vector<const FuncStmt*> functions;
    std::vector<const ClassStmt*> classes;
    for (const auto& stmt : program.statements) {
        if (!stmt) {
            fail("invalid statement", program.loc);
        }
        if (const auto* import_stmt = dynamic_cast<const ImportStmt*>(stmt.get())) {
            declare_import(*import_stmt);
            continue;
        }
        if (const auto* fn = dynamic_cast<const FuncStmt*>(stmt.get())) {
            declare_function(*fn);
            functions.push_back(fn);
        }
        if (const auto* struct_stmt = dynamic_cast<const StructStmt*>(stmt.get())) {
            declare_struct(*struct_stmt);
        }
        if (const auto* class_stmt = dynamic_cast<const ClassStmt*>(stmt.get())) {
            predeclare_class(*class_stmt);
            classes.push_back(class_stmt);
        }
        if (const auto* enum_stmt = dynamic_cast<const EnumStmt*>(stmt.get())) {
            declare_enum(*enum_stmt);
        }
        if (const auto* alias_stmt = dynamic_cast<const TypeAliasStmt*>(stmt.get())) {
            declare_type_alias(*alias_stmt);
        }
        if (const auto* interface_stmt = dynamic_cast<const InterfaceStmt*>(stmt.get())) {
            declare_interface(*interface_stmt);
        }
    }
    for (const ClassStmt* class_stmt : classes) {
        finalize_class(*class_stmt);
    }
    if (is_module_ && module_cache_ && compiling_module_index_ != 0xFFFFFFFFu) {
        build_module_exports();
        module_cache_->refresh_module_exports(compiling_module_index_, module_exports_);
    }
    validate_class_cycles();
    rebuild_all_class_layouts();
    validate_class_constructors(classes);

    std::unordered_map<std::string, bool> function_returns_value;
    for (const FuncStmt* fn : functions) {
        function_returns_value[fn->name] =
            function_body_returns_value(*fn, [&](const std::string& name) -> bool {
                const auto found = function_returns_value.find(name);
                if (found != function_returns_value.end()) {
                    return found->second;
                }
                return true;
            });
    }
    for (const FuncStmt* fn : functions) {
        const Symbol* symbol = lookup_symbol(fn->name);
        if (symbol && symbol->kind == SymbolKind::Function) {
            bytecode_.functions[symbol->function_index].returns_value = function_returns_value[fn->name];
            if (fn->is_async) {
                bytecode_.functions[symbol->function_index].returns_value = true;
            }
        }
        if (fn->name == "main" && fn->is_async && symbol && symbol->kind == SymbolKind::Function) {
            bytecode_.async_main_function_index = symbol->function_index;
        }
    }

    const bool user_async_main = bytecode_.async_main_function_index != 0xFFFFFFFFu;
    std::vector<const Stmt*> module_executable;
    if (!is_module_ && !user_async_main) {
        for (const auto& stmt : program.statements) {
            if (!stmt || is_module_level_declaration(*stmt)) {
                continue;
            }
            module_executable.push_back(stmt.get());
        }
    }
    const bool use_module_async_entry =
        !is_module_ && !user_async_main && !module_executable.empty() &&
        std::any_of(module_executable.begin(), module_executable.end(),
                    [](const Stmt* stmt) { return stmt_contains_await(*stmt); });
    std::uint32_t module_async_entry_index = 0xFFFFFFFFu;
    if (use_module_async_entry) {
        FunctionInfo entry;
        entry.name = "__pscript_module_entry__";
        entry.is_async = true;
        entry.returns_value = false;
        module_async_entry_index = static_cast<std::uint32_t>(bytecode_.functions.size());
        bytecode_.functions.push_back(std::move(entry));
        bytecode_.async_main_function_index = module_async_entry_index;
    }

    for (const auto& stmt : program.statements) {
        if (!stmt || is_module_level_declaration(*stmt)) {
            continue;
        }
        if (use_module_async_entry) {
            continue;
        }
        compile_stmt(*stmt);
    }
    emit(Instruction{Opcode::Halt, 0, 0, 0});
    note_registers();

    for (const FuncStmt* fn : functions) {
        compile_function(*fn);
    }
    if (module_async_entry_index != 0xFFFFFFFFu) {
        compile_module_async_entry(module_async_entry_index, module_executable);
    }
    for (const ClassStmt* class_stmt : classes) {
        compile_class(*class_stmt);
    }
    note_registers();

    if (is_module_) {
        build_module_exports();
        bytecode_.modules.clear();
        bytecode_.module_bytecodes.clear();
    }

    if (const auto error = bytecode_.validate()) {
        fail(*error, program.loc);
    }
    bytecode_.warnings = std::move(warnings_);
    return bytecode_;
}

void Compiler::warn_deprecated_use(const std::vector<AnnotationInfo>& annotations, const Location& loc) {
    for (const AnnotationInfo& annotation : annotations) {
        if (annotation.name != "deprecated") {
            continue;
        }
        std::string message = "use of deprecated symbol";
        if (!annotation.args.empty() && !annotation.args.front().second.empty()) {
            message += ": " + annotation.args.front().second;
        }
        CompileWarning warning;
        warning.message = std::move(message);
        warning.loc.line = loc.line;
        warning.loc.column = loc.column;
        warnings_.push_back(std::move(warning));
        return;
    }
}

void Compiler::compile_stmt(const Stmt& stmt) {
    current_emit_loc_ = stmt.loc;
    if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
        compile_assign(*assign);
        return;
    }
    if (const auto* destructure = dynamic_cast<const DestructureAssignStmt*>(&stmt)) {
        compile_destructure_assign(*destructure);
        return;
    }
    if (const auto* print = dynamic_cast<const PrintStmt*>(&stmt)) {
        compile_print(*print);
        return;
    }
    if (const auto* if_stmt = dynamic_cast<const IfStmt*>(&stmt)) {
        compile_if(*if_stmt);
        return;
    }
    if (const auto* index_assign = dynamic_cast<const IndexAssignStmt*>(&stmt)) {
        compile_index_assign(*index_assign);
        return;
    }
    if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(&stmt)) {
        compile_expr_stmt(*expr_stmt);
        return;
    }
    if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(&stmt)) {
        compile_while(*while_stmt);
        return;
    }
    if (const auto* for_stmt = dynamic_cast<const ForStmt*>(&stmt)) {
        compile_for(*for_stmt);
        return;
    }
    if (const auto* async_for = dynamic_cast<const AsyncForStmt*>(&stmt)) {
        compile_async_for(*async_for);
        return;
    }
    if (const auto* async_with = dynamic_cast<const AsyncWithStmt*>(&stmt)) {
        compile_async_with(*async_with);
        return;
    }
    if (const auto* break_stmt = dynamic_cast<const BreakStmt*>(&stmt)) {
        compile_break(*break_stmt);
        return;
    }
    if (const auto* continue_stmt = dynamic_cast<const ContinueStmt*>(&stmt)) {
        compile_continue(*continue_stmt);
        return;
    }
    if (const auto* match_stmt = dynamic_cast<const MatchStmt*>(&stmt)) {
        compile_match(*match_stmt);
        return;
    }
    if (const auto* func_stmt = dynamic_cast<const FuncStmt*>(&stmt)) {
        (void)func_stmt;
        return;
    }
    if (const auto* struct_stmt = dynamic_cast<const StructStmt*>(&stmt)) {
        (void)struct_stmt;
        return;
    }
    if (const auto* class_stmt = dynamic_cast<const ClassStmt*>(&stmt)) {
        (void)class_stmt;
        return;
    }
    if (const auto* enum_stmt = dynamic_cast<const EnumStmt*>(&stmt)) {
        (void)enum_stmt;
        return;
    }
    if (dynamic_cast<const TypeAliasStmt*>(&stmt)) {
        return;
    }
    if (const auto* interface_stmt = dynamic_cast<const InterfaceStmt*>(&stmt)) {
        (void)interface_stmt;
        return;
    }
    if (const auto* return_stmt = dynamic_cast<const ReturnStmt*>(&stmt)) {
        compile_return(*return_stmt);
        return;
    }
    if (const auto* try_stmt = dynamic_cast<const TryStmt*>(&stmt)) {
        compile_try(*try_stmt);
        return;
    }
    if (const auto* throw_stmt = dynamic_cast<const ThrowStmt*>(&stmt)) {
        compile_throw(*throw_stmt);
        return;
    }
    if (const auto* defer_stmt = dynamic_cast<const DeferStmt*>(&stmt)) {
        compile_defer(*defer_stmt);
        return;
    }
    if (const auto* import_stmt = dynamic_cast<const ImportStmt*>(&stmt)) {
        compile_import(*import_stmt);
        return;
    }
    if (dynamic_cast<const PassStmt*>(&stmt)) {
        return;
    }
    fail("unsupported statement", stmt.loc);
}

Register Compiler::compile_expr(const Expr& expr) {
    current_emit_loc_ = expr.loc;
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (!unary->operand) {
            fail("invalid expression", unary->loc);
        }
        const Register src = compile_expr(*unary->operand);
        const Register dest = alloc_register();
        if (unary->op == TokenKind::Plus) {
            emit(Instruction{Opcode::UnaryPlus, dest, src, 0});
            return dest;
        }
        if (unary->op == TokenKind::Minus) {
            emit(Instruction{Opcode::UnaryMinus, dest, src, 0});
            return dest;
        }
        if (is_unary_not(unary->op)) {
            emit(Instruction{Opcode::UnaryNot, dest, src, 0});
            return dest;
        }
        if (unary->op == TokenKind::Tilde) {
            emit(Instruction{Opcode::UnaryBitNot, dest, src, 0});
            return dest;
        }
        fail("unsupported unary operator", unary->loc);
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (!binary->left || !binary->right) {
            fail("invalid expression", binary->loc);
        }
        if (is_logical_and(binary->op) || is_logical_or(binary->op)) {
            const Register dest = alloc_register();
            compile_logical_into(*binary, dest);
            return dest;
        }
        if (is_null_coalesce(binary->op)) {
            const Register dest = alloc_register();
            compile_null_coalesce_into(*binary, dest);
            return dest;
        }
        const Register lhs = compile_expr(*binary->left);
        const Register rhs = compile_expr(*binary->right);
        const Register dest = alloc_register();
        emit(Instruction{binary_opcode(binary->op, binary->loc), dest, lhs, rhs});
        return dest;
    }
    const Register dest = alloc_register();
    compile_expr_into(expr, dest);
    return dest;
}

void Compiler::compile_expr_into(const Expr& expr, Register dest) {
    current_emit_loc_ = expr.loc;
    if (const auto* match_expr = dynamic_cast<const MatchExpr*>(&expr)) {
        compile_match_expr_into(*match_expr, dest);
        return;
    }
    if (const auto* propagate_expr = dynamic_cast<const PropagateExpr*>(&expr)) {
        compile_propagate_expr_into(*propagate_expr, dest);
        return;
    }
    if (const auto* await_expr = dynamic_cast<const AwaitExpr*>(&expr)) {
        if (async_function_depth_ == 0) {
            fail("await outside async function", await_expr->loc);
        }
        if (!await_expr->operand) {
            fail("invalid expression", await_expr->loc);
        }
        const TypeDescriptor task_type = infer_type_descriptor(*await_expr->operand);
        if (task_type.kind != StaticType::Task && task_type.kind != StaticType::Any &&
            task_type.kind != StaticType::Unknown) {
            fail("await expects a Task", await_expr->loc);
        }
        const Register task_reg = compile_expr(*await_expr->operand);
        emit(Instruction{Opcode::Await, dest, task_reg, 0});
        return;
    }
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expr)) {
        Constant constant;
        constant.lexeme = literal->lexeme;
        switch (literal->kind) {
            case LiteralExpr::Kind::Integer:
                constant.kind = ConstantKind::Integer;
                break;
            case LiteralExpr::Kind::Float:
                constant.kind = ConstantKind::Float;
                break;
            case LiteralExpr::Kind::String:
                constant.kind = ConstantKind::String;
                break;
            case LiteralExpr::Kind::True:
                constant.kind = ConstantKind::Bool;
                constant.bool_value = true;
                break;
            case LiteralExpr::Kind::False:
                constant.kind = ConstantKind::Bool;
                constant.bool_value = false;
                break;
            case LiteralExpr::Kind::Null:
                constant.kind = ConstantKind::Null;
                break;
            case LiteralExpr::Kind::Duration:
                fail("duration literal not allowed here", literal->loc);
        }
        emit(Instruction{Opcode::LoadConst, dest, intern_constant(std::move(constant)), 0});
        return;
    }
    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(&expr)) {
        if (ident->name == "this") {
            if (!class_context_) {
                fail("invalid use of 'this'", ident->loc);
            }
            emit(Instruction{Opcode::LoadLocal, dest, 0, 0});
            return;
        }
        if (ident->name == "super" || ident->name == "parent") {
            fail("invalid use of '" + ident->name + "'", ident->loc);
        }
        if (const Symbol* symbol = lookup_symbol(ident->name)) {
            if (symbol->kind == SymbolKind::Variable || symbol->kind == SymbolKind::Function) {
                emit_load_name(dest, ident->name, ident->loc);
                return;
            }
        }
        if (class_context_) {
            if (const std::optional<ClassFieldLookup> field = find_class_field(class_context_->class_index, ident->name)) {
                check_class_field_access(*field, ident->loc);
                const Register object = alloc_register();
                emit(Instruction{Opcode::LoadLocal, object, 0, 0});
                emit(Instruction{Opcode::GetField, dest, object, field->field_index});
                return;
            }
        }
        emit_load_name(dest, ident->name, ident->loc);
        return;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        if (!unary->operand) {
            fail("invalid expression", unary->loc);
        }
        const Register src = compile_expr(*unary->operand);
        if (unary->op == TokenKind::Plus) {
            emit(Instruction{Opcode::UnaryPlus, dest, src, 0});
            return;
        }
        if (unary->op == TokenKind::Minus) {
            emit(Instruction{Opcode::UnaryMinus, dest, src, 0});
            return;
        }
        if (is_unary_not(unary->op)) {
            emit(Instruction{Opcode::UnaryNot, dest, src, 0});
            return;
        }
        if (unary->op == TokenKind::Tilde) {
            emit(Instruction{Opcode::UnaryBitNot, dest, src, 0});
            return;
        }
        fail("unsupported unary operator", unary->loc);
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        if (!binary->left || !binary->right) {
            fail("invalid expression", binary->loc);
        }
        if (is_logical_and(binary->op) || is_logical_or(binary->op)) {
            compile_logical_into(*binary, dest);
            return;
        }
        if (is_null_coalesce(binary->op)) {
            compile_null_coalesce_into(*binary, dest);
            return;
        }
        const Register lhs = compile_expr(*binary->left);
        const Register rhs = compile_expr(*binary->right);
        emit(Instruction{binary_opcode(binary->op, binary->loc), dest, lhs, rhs});
        return;
    }
    if (const auto* membership = dynamic_cast<const MembershipExpr*>(&expr)) {
        compile_membership_into(*membership, dest);
        return;
    }
    if (const auto* ternary = dynamic_cast<const TernaryExpr*>(&expr)) {
        compile_ternary_into(*ternary, dest);
        return;
    }
    if (const auto* assign = dynamic_cast<const AssignExpr*>(&expr)) {
        compile_assign_expr_into(*assign, dest);
        return;
    }
    if (const auto* update = dynamic_cast<const UpdateExpr*>(&expr)) {
        compile_update_into(*update, dest);
        return;
    }
    if (const auto* list = dynamic_cast<const ListExpr*>(&expr)) {
        if (list->elements.empty()) {
            emit(Instruction{Opcode::MakeList, dest, 0, 0});
            return;
        }
        const Register first = next_register_;
        for (std::size_t i = 0; i < list->elements.size(); ++i) {
            alloc_register();
        }
        for (std::size_t i = 0; i < list->elements.size(); ++i) {
            if (!list->elements[i]) {
                fail("invalid expression", list->loc);
            }
            compile_expr_into(*list->elements[i], first + static_cast<Register>(i));
        }
        emit(Instruction{Opcode::MakeList, dest, first, static_cast<std::uint32_t>(list->elements.size())});
        return;
    }
    if (const auto* dict = dynamic_cast<const DictExpr*>(&expr)) {
        if (dict->entries.empty()) {
            emit(Instruction{Opcode::MakeDict, dest, 0, 0});
            return;
        }
        const Register first = next_register_;
        for (std::size_t i = 0; i < dict->entries.size() * 2; ++i) {
            alloc_register();
        }
        for (std::size_t i = 0; i < dict->entries.size(); ++i) {
            const DictEntry& entry = dict->entries[i];
            if (!entry.key || !entry.value) {
                fail("invalid expression", dict->loc);
            }
            compile_expr_into(*entry.key, first + static_cast<Register>(i * 2));
            compile_expr_into(*entry.value, first + static_cast<Register>(i * 2 + 1));
        }
        emit(Instruction{Opcode::MakeDict, dest, first, static_cast<std::uint32_t>(dict->entries.size())});
        return;
    }
    if (const auto* tuple = dynamic_cast<const TupleExpr*>(&expr)) {
        if (tuple->elements.empty()) {
            emit(Instruction{Opcode::MakeTuple, dest, 0, 0});
            return;
        }
        const Register first = next_register_;
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            alloc_register();
        }
        for (std::size_t i = 0; i < tuple->elements.size(); ++i) {
            if (!tuple->elements[i]) {
                fail("invalid expression", tuple->loc);
            }
            compile_expr_into(*tuple->elements[i], first + static_cast<Register>(i));
        }
        emit(Instruction{Opcode::MakeTuple, dest, first, static_cast<std::uint32_t>(tuple->elements.size())});
        return;
    }
    if (const auto* set = dynamic_cast<const SetExpr*>(&expr)) {
        if (set->elements.empty()) {
            emit(Instruction{Opcode::MakeSet, dest, 0, 0});
            return;
        }
        const Register first = next_register_;
        for (std::size_t i = 0; i < set->elements.size(); ++i) {
            alloc_register();
        }
        for (std::size_t i = 0; i < set->elements.size(); ++i) {
            if (!set->elements[i]) {
                fail("invalid expression", set->loc);
            }
            compile_expr_into(*set->elements[i], first + static_cast<Register>(i));
        }
        emit(Instruction{Opcode::MakeSet, dest, first, static_cast<std::uint32_t>(set->elements.size())});
        return;
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr)) {
        if (!index->object || !index->index) {
            fail("invalid expression", index->loc);
        }
        if (infer_expr(*index->object) == StaticType::Set) {
            fail("cannot index set", index->loc);
        }
        const Register object = compile_expr(*index->object);
        const Register key = compile_expr(*index->index);
        emit(Instruction{Opcode::GetIndex, dest, object, key});
        return;
    }
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expr)) {
        if (!field->object) {
            fail("invalid expression", field->loc);
        }
        if (const auto* ident = dynamic_cast<const IdentifierExpr*>(field->object.get())) {
            if (const Symbol* symbol = lookup_symbol(ident->name)) {
                if (symbol->kind == SymbolKind::Enum) {
                    const std::uint32_t member_index =
                        resolve_enum_member(symbol->enum_index, field->field, field->loc);
                    emit(Instruction{Opcode::LoadEnum, dest, symbol->enum_index, member_index});
                    return;
                }
            }
        }
        const std::optional<std::uint32_t> class_index = infer_class_index(*field->object);
        if (class_index) {
            if (const std::optional<ClassPropertyLookup> property =
                    find_class_property(*class_index, field->field)) {
                if (!property->has_getter) {
                    fail("property '" + field->field + "' has no getter", field->loc);
                }
                check_class_property_access(*property, false, field->loc);
                const Register object = compile_expr(*field->object);
                const std::uint32_t call_index =
                    intern_class_call(ClassCallKind::Method, property->class_index, property->getter_function_index, 0);
                emit(Instruction{Opcode::CallClassMethod, dest, object, call_index});
                return;
            }
            const ClassFieldLookup field_lookup = lookup_class_field(*class_index, field->field, field->loc);
            check_class_field_access(field_lookup, field->loc);
            const Register object = compile_expr(*field->object);
            emit(Instruction{Opcode::GetField, dest, object, field_lookup.field_index});
            return;
        }
        const std::optional<std::uint32_t> struct_index = infer_struct_index(*field->object);
        if (!struct_index) {
            const StaticType object_type = infer_expr(*field->object);
            if (object_type == StaticType::Integer || object_type == StaticType::Float ||
                object_type == StaticType::Bool || object_type == StaticType::String ||
                object_type == StaticType::List || object_type == StaticType::Tuple ||
                object_type == StaticType::Set || object_type == StaticType::Enum) {
                fail("cannot resolve field '" + field->field + "'", field->loc);
            }
            const Register object = compile_expr(*field->object);
            const Register key = emit_string_constant(field->field);
            emit(Instruction{Opcode::GetIndex, dest, object, key});
            return;
        }
        check_field_access(*struct_index, field->field, field->loc);
        const std::uint32_t field_index = resolve_field_index(*struct_index, field->field, field->loc);
        const Register object = compile_expr(*field->object);
        emit(Instruction{Opcode::GetField, dest, object, field_index});
        return;
    }
    if (const auto* call = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (!call->object) {
            fail("invalid expression", call->loc);
        }
        if (const auto* ident = dynamic_cast<const IdentifierExpr*>(call->object.get())) {
            if (ident->name == "embed" && call->method == "text") {
                compile_embed_call_into(call->loc, call->arguments, dest, true);
                return;
            }
            if (ident->name == "parallel" && call->method == "map") {
                Register first = 0;
                if (!call->arguments.empty()) {
                    first = next_register_;
                    for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                        alloc_register();
                    }
                    for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                        if (!call->arguments[i]) {
                            fail("invalid expression", call->loc);
                        }
                        compile_expr_into(*call->arguments[i], first + static_cast<Register>(i));
                    }
                } else {
                    fail("'parallel.map' expects 2 arguments", call->loc);
                }
                if (call->arguments.size() != 2) {
                    fail("'parallel.map' expects 2 arguments", call->loc);
                }
                const std::uint32_t info =
                    intern_call("parallel_map", static_cast<std::uint32_t>(call->arguments.size()));
                emit(Instruction{Opcode::CallBuiltin, dest, first, info});
                return;
            }
            if (ident->name == "super" || ident->name == "parent") {
                if (!class_context_) {
                    fail("invalid use of '" + ident->name + "'", ident->loc);
                }
                const ClassMethodLookup method =
                    lookup_super_method(class_context_->class_index, call->method, call->loc);
                const Register first = next_register_;
                alloc_register();
                for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                    alloc_register();
                }
                emit(Instruction{Opcode::LoadLocal, first, 0, 0});
                for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                    if (!call->arguments[i]) {
                        fail("invalid expression", call->loc);
                    }
                    compile_expr_into(*call->arguments[i], first + 1 + static_cast<Register>(i));
                }
                const std::uint32_t info =
                    intern_class_call(ClassCallKind::SuperMethod, method.class_index, method.function_index,
                                      static_cast<std::uint32_t>(call->arguments.size()));
                emit(Instruction{Opcode::CallClassMethod, dest, first, info});
                return;
            }
            if (const Symbol* symbol = lookup_symbol(ident->name)) {
                if (symbol->kind == SymbolKind::Enum) {
                    compile_enum_variant_call(dest, *symbol, call->method, call->arguments, call->loc);
                    return;
                }
            }
        }
        if (const std::optional<std::uint32_t> class_index = infer_class_index(*call->object)) {
            check_method_argument_types(*call->object, call->method, call->arguments, call->loc);
            const ClassMethodLookup method = lookup_class_method(*class_index, call->method, call->loc);
            const Register first = next_register_;
            alloc_register();
            for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                alloc_register();
            }
            compile_expr_into(*call->object, first);
            for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                if (!call->arguments[i]) {
                    fail("invalid expression", call->loc);
                }
                compile_expr_into(*call->arguments[i], first + 1 + static_cast<Register>(i));
            }
            const std::uint32_t info =
                intern_class_call(ClassCallKind::Method, method.class_index, method.function_index,
                                  static_cast<std::uint32_t>(call->arguments.size()));
            emit(Instruction{Opcode::CallClassMethod, dest, first, info});
            return;
        }
        check_method_argument_types(*call->object, call->method, call->arguments, call->loc);
        const Register first = next_register_;
        alloc_register();
        for (std::size_t i = 0; i < call->arguments.size(); ++i) {
            alloc_register();
        }
        compile_expr_into(*call->object, first);
        for (std::size_t i = 0; i < call->arguments.size(); ++i) {
            if (!call->arguments[i]) {
                fail("invalid expression", call->loc);
            }
            compile_expr_into(*call->arguments[i], first + 1 + static_cast<Register>(i));
        }
        const std::uint32_t info =
            intern_call(call->method, static_cast<std::uint32_t>(call->arguments.size()));
        emit(Instruction{Opcode::CallMethod, dest, first, info});
        return;
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (call->name == "embed") {
            compile_embed_call_into(call->loc, call->arguments, dest, false);
            return;
        }
        if (call->name == "super" || call->name == "parent") {
            if (!class_context_ || !class_context_->in_constructor) {
                fail("invalid use of '" + call->name + "'", call->loc);
            }
            const ClassInfo& current = bytecode_.classes[class_context_->class_index];
            if (current.parent_index == kInvalidIndex) {
                fail("invalid use of '" + call->name + "'", call->loc);
            }
            const ClassInfo& parent = bytecode_.classes[current.parent_index];
            if (parent.constructor_index == kInvalidIndex) {
                fail("parent class has no constructor", call->loc);
            }
            const FunctionInfo& parent_ctor = bytecode_.functions[parent.constructor_index];
            const std::uint32_t argc = static_cast<std::uint32_t>(call->arguments.size());
            if (argc != parent_ctor.arity) {
                fail("parent constructor expects " + std::to_string(parent_ctor.arity) + " arguments", call->loc);
            }
            Register first = 0;
            if (!call->arguments.empty()) {
                first = next_register_;
                for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                    alloc_register();
                }
                for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                    if (!call->arguments[i]) {
                        fail("invalid expression", call->loc);
                    }
                    compile_expr_into(*call->arguments[i], first + static_cast<Register>(i));
                }
            }
            emit_invoke_super(parent.constructor_index, argc, first);
            emit_load_null(dest);
            return;
        }
        if (const Symbol* symbol = lookup_symbol(call->name)) {
            if (symbol->kind == SymbolKind::Struct) {
                const StructInfo& st = bytecode_.structs[symbol->struct_index];
                if (!call->type_arguments.empty() || !st.generic_type_params.empty()) {
                    compile_generic_type_call(dest, *call, *symbol);
                    return;
                }
                if (call->arguments.empty()) {
                    emit(Instruction{Opcode::MakeStruct, dest, symbol->struct_index, 0});
                    return;
                }
                if (call->arguments.size() != st.fields.size()) {
                    fail("'" + call->name + "' expects " + std::to_string(st.fields.size()) + " arguments",
                         call->loc);
                }
                emit(Instruction{Opcode::MakeStruct, dest, symbol->struct_index, 0});
                for (std::size_t i = 0; i < st.fields.size(); ++i) {
                    const Register value = compile_expr(*call->arguments[i]);
                    emit(Instruction{Opcode::SetField, dest, static_cast<std::uint32_t>(i), value});
                }
                return;
            }
            if (symbol->kind == SymbolKind::Class) {
                const ClassInfo& cls = bytecode_.classes[symbol->class_index];
                if (!call->type_arguments.empty() || !cls.generic_type_params.empty()) {
                    compile_generic_type_call(dest, *call, *symbol);
                    return;
                }
                const std::uint32_t argc = static_cast<std::uint32_t>(call->arguments.size());
                if (cls.constructor_index != 0xFFFFFFFFu) {
                    const FunctionInfo& ctor =
                        cls.module_index != 0xFFFFFFFFu
                            ? module_bytecode_for(cls.module_index, call->loc)
                                  .functions[cls.constructor_index]
                            : bytecode_.functions[cls.constructor_index];
                    if (argc != ctor.arity) {
                        fail("'" + call->name + "' expects " + std::to_string(ctor.arity) + " arguments", call->loc);
                    }
                } else if (argc != 0) {
                    const bool native_async_stream =
                        cls.native_class_kind ==
                        static_cast<std::uint32_t>(NativeClassKind::AsyncStream);
                    if (!native_async_stream && argc != cls.fields.size()) {
                        fail("'" + call->name + "' expects 0 arguments", call->loc);
                    }
                    if (!native_async_stream) {
                        const std::uint32_t info =
                            intern_class_call(ClassCallKind::Instance, symbol->class_index, 0xFFFFFFFFu, 0);
                        emit(Instruction{Opcode::InvokeClass, dest, 0, info});
                        for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                            if (!call->arguments[i]) {
                                fail("invalid expression", call->loc);
                            }
                            const Register value = compile_expr(*call->arguments[i]);
                            emit(Instruction{Opcode::SetField, dest, static_cast<std::uint32_t>(i), value});
                        }
                        return;
                    }
                }
                Register first = 0;
                if (!call->arguments.empty()) {
                    first = next_register_;
                    for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                        alloc_register();
                    }
                    for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                        if (!call->arguments[i]) {
                            fail("invalid expression", call->loc);
                        }
                        compile_expr_into(*call->arguments[i], first + static_cast<Register>(i));
                    }
                }
                const std::uint32_t ctor_index = cls.constructor_index;
                const std::uint32_t info = intern_class_call(ClassCallKind::Instance, symbol->class_index, ctor_index,
                                                               argc);
                emit(Instruction{Opcode::InvokeClass, dest, first, info});
                return;
            }
            if (symbol->kind == SymbolKind::Function) {
                compile_direct_call(dest, *call, *symbol);
                return;
            }
            if (symbol->kind == SymbolKind::Variable) {
                compile_call_value(dest, *call);
                return;
            }
        }
        if (!is_known_builtin(call->name)) {
            fail("unknown function '" + call->name + "'", call->loc);
        }
        Register first = 0;
        if (!call->arguments.empty()) {
            first = next_register_;
            for (std::size_t i = 0; i < call->arguments.size(); ++i) {
                alloc_register();
            }
            compile_builtin_call_arguments(*call, first);
        } else if (call->name == "delay" || call->name == "timeout") {
            fail(call->name == "delay" ? "'delay' expects 1 argument" : "'timeout' expects 2 arguments", call->loc);
        }
        const std::uint32_t info = intern_call(call->name, static_cast<std::uint32_t>(call->arguments.size()));
        emit(Instruction{Opcode::CallBuiltin, dest, first, info});
        return;
    }
    fail("unsupported expression", expr.loc);
}

void Compiler::compile_assign(const AssignStmt& stmt) {
    const bool is_declaration = stmt.annotation != TypeAnnotationKind::None || stmt.is_const;
    if (is_declaration) {
        if (current_scope_defines(stmt.name)) {
            const auto found = scope_stack_.back().symbols.find(stmt.name);
            if (found == scope_stack_.back().symbols.end() || found->second.kind != SymbolKind::Variable ||
                found->second.initialized) {
                fail("duplicate declaration of '" + stmt.name + "'", stmt.loc);
            }
            if (!in_function_) {
                check_module_level_access(stmt.access, stmt.loc);
                found->second.access = stmt.access;
            }
        } else {
            declare_variable(stmt.name, stmt.type_name, stmt.loc, stmt.access, stmt.is_const);
        }
    } else {
        reject_const_assignment(stmt.name, stmt.loc);
        if (!current_scope_defines(stmt.name) && !in_function_) {
            check_module_level_access(stmt.access, stmt.loc);
        }
        const std::uint32_t slot = resolve_assign(stmt.name, stmt.loc);
        if (!in_function_ && is_module_) {
            if (Symbol* symbol = lookup_symbol(stmt.name)) {
                if (symbol->kind == SymbolKind::Variable && symbol->slot == slot && !symbol->explicit_declaration) {
                    symbol->access = stmt.access;
                }
            }
        }
    }
    VariableType& types = variable_types_[stmt.name];
    if (!stmt.value) {
        const Register src = alloc_register();
        emit_load_null(src);
        emit_store_name(stmt.name, src, stmt.loc);
        return;
    }

    const TypeDescriptor source = infer_type_descriptor(*stmt.value);
    if (types.declared) {
        check_assignable(*types.declared, source, stmt.value->loc);
        check_collection_literal(*stmt.value, *types.declared, stmt.value->loc);
    } else {
        types.inferred = source;
    }
    if (is_declaration && current_scope_defines(stmt.name)) {
        scope_stack_.back().symbols[stmt.name].types = types;
    }

    if (!is_declaration) {
        const bool string_accumulator =
            (types.declared && types.declared->kind == StaticType::String) ||
            (!types.declared && types.inferred && types.inferred->kind == StaticType::String);
        if (string_accumulator) {
            if (const auto* bin = dynamic_cast<const BinaryExpr*>(stmt.value.get())) {
                if (bin->op == TokenKind::Plus) {
                    if (const auto* left_ident = dynamic_cast<const IdentifierExpr*>(bin->left.get());
                        left_ident && left_ident->name == stmt.name) {
                    const Register dest = alloc_register();
                    emit_load_name(dest, stmt.name, stmt.loc);
                    const Register rhs = compile_expr(*bin->right);
                    emit(Instruction{Opcode::Add, dest, dest, rhs});
                    if (types.declared) {
                        apply_assign_conversions(dest, *types.declared, source, stmt.value->loc);
                    }
                    emit_store_name(stmt.name, dest, stmt.loc);
                    return;
                }
                if (const auto* right_ident = dynamic_cast<const IdentifierExpr*>(bin->right.get());
                    right_ident && right_ident->name == stmt.name) {
                    const Register dest = alloc_register();
                    emit_load_name(dest, stmt.name, stmt.loc);
                    const Register lhs = compile_expr(*bin->left);
                    emit(Instruction{Opcode::Add, dest, lhs, dest});
                    if (types.declared) {
                        apply_assign_conversions(dest, *types.declared, source, stmt.value->loc);
                    }
                    emit_store_name(stmt.name, dest, stmt.loc);
                    return;
                }
                }
            }
        }
    }

    const Register src = compile_expr(*stmt.value);
    if (types.declared) {
        apply_assign_conversions(src, *types.declared, source, stmt.value->loc);
    }
    emit_store_name(stmt.name, src, stmt.loc);
    if (Symbol* target = lookup_symbol(stmt.name)) {
        if (const auto* ident = dynamic_cast<const IdentifierExpr*>(stmt.value.get())) {
            if (const Symbol* source = lookup_symbol(ident->name)) {
                if (source->kind == SymbolKind::Function) {
                    target->bound_function_index = source->function_index;
                    target->bound_function_overloads = source->function_overloads;
                    target->bound_callable_name = ident->name;
                    target->bound_import_module_index = source->import_module_index;
                }
            }
        }
    }
}

void Compiler::compile_index_assign(const IndexAssignStmt& stmt) {
    if (!stmt.object || !stmt.index || !stmt.value) {
        fail("invalid assignment", stmt.loc);
    }
    reject_index_write(*stmt.object, stmt.loc);
    const Register object = compile_expr(*stmt.object);
    const Register index = compile_expr(*stmt.index);
    const Register value = compile_expr(*stmt.value);
    emit(Instruction{Opcode::SetIndex, object, index, value});
}

void Compiler::emit_check_destruct(Register value, std::uint32_t expected_count, std::uint32_t kind) {
    emit(Instruction{Opcode::CheckDestruct, value, expected_count, kind});
}

void Compiler::emit_destruct_rest(Register dest, Register source, std::uint32_t start_index) {
    emit(Instruction{Opcode::DestructRest, dest, source, start_index});
}

void Compiler::emit_destruct_dict_rest(Register dest, Register source,
                                       const std::vector<std::string>& excluded_keys) {
    const Register first = next_register_;
    for (std::size_t i = 0; i < excluded_keys.size(); ++i) {
        alloc_register();
    }
    for (std::size_t i = 0; i < excluded_keys.size(); ++i) {
        Constant constant;
        constant.kind = ConstantKind::String;
        constant.lexeme = excluded_keys[i];
        emit(Instruction{Opcode::LoadConst, first + static_cast<Register>(i), intern_constant(std::move(constant)),
                         0});
    }
    const std::uint32_t packed =
        (static_cast<std::uint32_t>(first) << 16) | static_cast<std::uint32_t>(excluded_keys.size());
    emit(Instruction{Opcode::DestructDictRest, dest, source, packed});
}

Register Compiler::compile_dict_key_index(Register source, const std::string& key, const Location& loc) {
    const Register key_reg = alloc_register();
    Constant constant;
    constant.kind = ConstantKind::String;
    constant.lexeme = key;
    emit(Instruction{Opcode::LoadConst, key_reg, intern_constant(std::move(constant)), 0});
    const Register item = alloc_register();
    emit(Instruction{Opcode::GetIndex, item, source, key_reg});
    return item;
}

const Expr* find_dict_literal_value(const DictExpr& dict, const std::string& key) {
    for (const DictEntry& entry : dict.entries) {
        if (!entry.key) {
            continue;
        }
        const auto* literal = dynamic_cast<const LiteralExpr*>(entry.key.get());
        if (literal && literal->kind == LiteralExpr::Kind::String && literal->lexeme == key) {
            return entry.value.get();
        }
    }
    return nullptr;
}

void Compiler::compile_dict_destructure_pattern(const DestructPattern& pattern, Register source,
                                                const Expr* rhs_hint, const Location& loc) {
    const auto* dict_rhs = rhs_hint ? dynamic_cast<const DictExpr*>(rhs_hint) : nullptr;
    std::vector<std::string> excluded_keys;
    excluded_keys.reserve(pattern.dict_entries.size());
    for (const DestructDictEntry& entry : pattern.dict_entries) {
        if (!entry.pattern) {
            fail("invalid destructuring assignment", loc);
        }
        excluded_keys.push_back(entry.key);
        const Register item = compile_dict_key_index(source, entry.key, loc);
        const Expr* child_rhs = dict_rhs ? find_dict_literal_value(*dict_rhs, entry.key) : nullptr;
        compile_destructure_pattern(*entry.pattern, item, child_rhs, loc);
    }
    if (pattern.dict_rest) {
        const Register rest_value = alloc_register();
        if (dict_rhs) {
            std::vector<std::pair<std::string, Register>> pairs;
            const Register first = next_register_;
            std::size_t pair_count = 0;
            for (const DictEntry& entry : dict_rhs->entries) {
                if (!entry.key || !entry.value) {
                    continue;
                }
                const auto* literal = dynamic_cast<const LiteralExpr*>(entry.key.get());
                if (!literal || literal->kind != LiteralExpr::Kind::String) {
                    continue;
                }
                bool excluded = false;
                for (const std::string& key : excluded_keys) {
                    if (key == literal->lexeme) {
                        excluded = true;
                        break;
                    }
                }
                if (excluded) {
                    continue;
                }
                alloc_register();
                alloc_register();
                Constant constant;
                constant.kind = ConstantKind::String;
                constant.lexeme = literal->lexeme;
                emit(Instruction{Opcode::LoadConst, first + static_cast<Register>(pair_count * 2),
                                 intern_constant(std::move(constant)), 0});
                compile_expr_into(*entry.value, first + static_cast<Register>(pair_count * 2 + 1));
                ++pair_count;
            }
            if (pair_count == 0) {
                emit(Instruction{Opcode::MakeDict, rest_value, 0, 0});
            } else {
                emit(Instruction{Opcode::MakeDict, rest_value, first, static_cast<std::uint32_t>(pair_count)});
            }
        } else {
            emit_destruct_dict_rest(rest_value, source, excluded_keys);
        }
        resolve_assign(pattern.dict_rest->name, loc);
        emit_store_name(pattern.dict_rest->name, rest_value, loc);
    }
}

void Compiler::compile_struct_rest_dict(Register dest, Register source, std::uint32_t struct_index,
                                        const std::vector<std::string>& excluded_fields, const Location& loc) {
    if (struct_index >= bytecode_.structs.size()) {
        fail("invalid struct type", loc);
    }
    const StructInfo& info = bytecode_.structs[struct_index];
    const Register first = next_register_;
    std::size_t pair_count = 0;
    for (std::size_t field_index = 0; field_index < info.fields.size(); ++field_index) {
        const StructFieldInfo& field = info.fields[field_index];
        bool excluded = false;
        for (const std::string& name : excluded_fields) {
            if (name == field.name) {
                excluded = true;
                break;
            }
        }
        if (excluded || field.access != MemberAccess::Public) {
            continue;
        }
        alloc_register();
        alloc_register();
        Constant constant;
        constant.kind = ConstantKind::String;
        constant.lexeme = field.name;
        emit(Instruction{Opcode::LoadConst, first + static_cast<Register>(pair_count * 2),
                         intern_constant(std::move(constant)), 0});
        emit(Instruction{Opcode::GetField, first + static_cast<Register>(pair_count * 2 + 1), source,
                         static_cast<std::uint32_t>(field_index)});
        ++pair_count;
    }
    if (pair_count == 0) {
        emit(Instruction{Opcode::MakeDict, dest, 0, 0});
    } else {
        emit(Instruction{Opcode::MakeDict, dest, first, static_cast<std::uint32_t>(pair_count)});
    }
}

void Compiler::compile_struct_destructure_pattern(const DestructPattern& pattern, Register source,
                                                  const Expr* rhs_hint, const Location& loc) {
    std::string struct_name = pattern.struct_type_name;
    const std::size_t lt = struct_name.find('<');
    if (lt != std::string::npos) {
        struct_name = struct_name.substr(0, lt);
    }
    const std::uint32_t struct_index = resolve_struct_index(struct_name, loc);
    std::vector<std::string> excluded_fields;
    for (const auto& element : pattern.elements) {
        if (!element) {
            fail("invalid destructuring assignment", loc);
        }
        if (element->kind == DestructPatternKind::Rest) {
            const Register rest_value = alloc_register();
            compile_struct_rest_dict(rest_value, source, struct_index, excluded_fields, loc);
            resolve_assign(element->name, loc);
            emit_store_name(element->name, rest_value, loc);
            return;
        }
        const std::string field_name = struct_pattern_field_name(*element);
        if (field_name.empty()) {
            fail("invalid destructuring assignment", loc);
        }
        excluded_fields.push_back(field_name);
        check_field_access(struct_index, field_name, loc);
        const std::uint32_t field_index = resolve_field_index(struct_index, field_name, loc);
        const Register item = alloc_register();
        emit(Instruction{Opcode::GetField, item, source, field_index});
        compile_destructure_pattern(*element, item, nullptr, loc);
    }
}

void Compiler::compile_parameter_destructure(const DestructPattern& pattern, Register source, const Location& loc) {
    compile_destructure_pattern(pattern, source, nullptr, loc);
}

void Compiler::validate_destructure_pattern_rhs(const DestructPattern& pattern, const Expr& rhs,
                                                const Location& loc) {
    if (pattern.kind == DestructPatternKind::Dict) {
        const auto* dict = dynamic_cast<const DictExpr*>(&rhs);
        if (!dict) {
            return;
        }
        for (const DestructDictEntry& entry : pattern.dict_entries) {
            if (!entry.pattern) {
                fail("invalid destructuring assignment", loc);
            }
            const Expr* value = find_dict_literal_value(*dict, entry.key);
            if (!value) {
                fail("undefined dictionary key", loc);
            }
            validate_destructure_pattern_rhs(*entry.pattern, *value, loc);
        }
        return;
    }
    if (pattern.kind == DestructPatternKind::Struct) {
        return;
    }
    if (pattern.kind == DestructPatternKind::List) {
        const auto* list = dynamic_cast<const ListExpr*>(&rhs);
        if (!list) {
            return;
        }
        const DestructCollectionShape shape = analyze_collection_pattern(pattern);
        if (shape.has_rest) {
            if (list->elements.size() < shape.fixed_slots) {
                fail("destructuring mismatch", loc);
            }
            for (std::size_t i = 0; i < shape.fixed_slots; ++i) {
                if (!pattern.elements[i] || !list->elements[i]) {
                    fail("invalid destructuring assignment", loc);
                }
                validate_destructure_pattern_rhs(*pattern.elements[i], *list->elements[i], loc);
            }
            return;
        }
        if (list->elements.size() != pattern.elements.size()) {
            fail("destructuring mismatch", loc);
        }
        for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
            if (!pattern.elements[i] || !list->elements[i]) {
                fail("invalid destructuring assignment", loc);
            }
            validate_destructure_pattern_rhs(*pattern.elements[i], *list->elements[i], loc);
        }
        return;
    }
    if (pattern.kind == DestructPatternKind::Tuple) {
        const auto* tuple = dynamic_cast<const TupleExpr*>(&rhs);
        if (!tuple) {
            return;
        }
        const DestructCollectionShape shape = analyze_collection_pattern(pattern);
        if (shape.has_rest) {
            if (tuple->elements.size() < shape.fixed_slots) {
                fail("destructuring mismatch", loc);
            }
            for (std::size_t i = 0; i < shape.fixed_slots; ++i) {
                if (!pattern.elements[i] || !tuple->elements[i]) {
                    fail("invalid destructuring assignment", loc);
                }
                validate_destructure_pattern_rhs(*pattern.elements[i], *tuple->elements[i], loc);
            }
            return;
        }
        if (tuple->elements.size() != pattern.elements.size()) {
            fail("destructuring mismatch", loc);
        }
        for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
            if (!pattern.elements[i] || !tuple->elements[i]) {
                fail("invalid destructuring assignment", loc);
            }
            validate_destructure_pattern_rhs(*pattern.elements[i], *tuple->elements[i], loc);
        }
    }
}

void Compiler::compile_collection_destructure_pattern(const DestructPattern& pattern, Register source,
                                                    const Expr* rhs_hint, const Location& loc,
                                                    std::uint32_t collection_kind) {
    const auto* list_rhs = rhs_hint ? dynamic_cast<const ListExpr*>(rhs_hint) : nullptr;
    const auto* tuple_rhs = rhs_hint ? dynamic_cast<const TupleExpr*>(rhs_hint) : nullptr;
    const DestructCollectionShape shape = analyze_collection_pattern(pattern);
    if (!list_rhs && !tuple_rhs) {
        if (shape.has_rest) {
            emit_check_destruct(source, static_cast<std::uint32_t>(shape.fixed_slots), collection_kind + 2);
        } else {
            emit_check_destruct(source, static_cast<std::uint32_t>(pattern.elements.size()), collection_kind);
        }
    }
    std::size_t source_index = 0;
    for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
        const DestructPattern& element = *pattern.elements[i];
        if (element.kind == DestructPatternKind::Rest) {
            const Register rest_value = alloc_register();
            if (collection_kind == 0 && list_rhs) {
                const std::size_t rest_count = list_rhs->elements.size() - source_index;
                if (rest_count == 0) {
                    emit(Instruction{Opcode::MakeList, rest_value, 0, 0});
                } else {
                    const Register first = next_register_;
                    for (std::size_t j = 0; j < rest_count; ++j) {
                        alloc_register();
                    }
                    for (std::size_t j = 0; j < rest_count; ++j) {
                        compile_expr_into(*list_rhs->elements[source_index + j], first + static_cast<Register>(j));
                    }
                    emit(Instruction{Opcode::MakeList, rest_value, first, static_cast<std::uint32_t>(rest_count)});
                }
            } else if (collection_kind == 1 && tuple_rhs) {
                const std::size_t rest_count = tuple_rhs->elements.size() - source_index;
                if (rest_count == 0) {
                    emit(Instruction{Opcode::MakeList, rest_value, 0, 0});
                } else {
                    const Register first = next_register_;
                    for (std::size_t j = 0; j < rest_count; ++j) {
                        alloc_register();
                    }
                    for (std::size_t j = 0; j < rest_count; ++j) {
                        compile_expr_into(*tuple_rhs->elements[source_index + j], first + static_cast<Register>(j));
                    }
                    emit(Instruction{Opcode::MakeList, rest_value, first, static_cast<std::uint32_t>(rest_count)});
                }
            } else {
                emit_destruct_rest(rest_value, source, static_cast<std::uint32_t>(source_index));
            }
            resolve_assign(element.name, loc);
            emit_store_name(element.name, rest_value, loc);
            return;
        }
        if (element.kind == DestructPatternKind::Wildcard) {
            ++source_index;
            continue;
        }
        const Register item = alloc_register();
        const Register index = alloc_register();
        compile_integer_into(index, static_cast<std::int64_t>(source_index));
        emit(Instruction{Opcode::GetIndex, item, source, index});
        const Expr* child_rhs = nullptr;
        if (collection_kind == 0 && list_rhs && source_index < list_rhs->elements.size()) {
            child_rhs = list_rhs->elements[source_index].get();
        } else if (collection_kind == 1 && tuple_rhs && source_index < tuple_rhs->elements.size()) {
            child_rhs = tuple_rhs->elements[source_index].get();
        }
        compile_destructure_pattern(element, item, child_rhs, loc);
        ++source_index;
    }
}

void Compiler::compile_destructure_pattern(const DestructPattern& pattern, Register source, const Expr* rhs_hint,
                                           const Location& loc) {
    if (pattern.kind == DestructPatternKind::Literal) {
        fail("invalid destructuring assignment", loc);
    }
    if (pattern.kind == DestructPatternKind::Positional) {
        fail("invalid destructuring assignment", loc);
    }
    if (pattern.kind == DestructPatternKind::Identifier) {
        resolve_assign(pattern.name, loc);
        emit_store_name(pattern.name, source, loc);
        return;
    }
    if (pattern.kind == DestructPatternKind::Wildcard) {
        return;
    }
    if (pattern.kind == DestructPatternKind::Rest) {
        fail("invalid destructuring assignment", loc);
    }
    if (pattern.kind == DestructPatternKind::List) {
        compile_collection_destructure_pattern(pattern, source, rhs_hint, loc, 0);
        return;
    }
    if (pattern.kind == DestructPatternKind::Tuple) {
        compile_collection_destructure_pattern(pattern, source, rhs_hint, loc, 1);
        return;
    }
    if (pattern.kind == DestructPatternKind::Dict) {
        compile_dict_destructure_pattern(pattern, source, rhs_hint, loc);
        return;
    }
    if (pattern.kind == DestructPatternKind::Struct) {
        compile_struct_destructure_pattern(pattern, source, rhs_hint, loc);
    }
}

void Compiler::compile_destructure_assign(const DestructureAssignStmt& stmt) {
    if (!stmt.pattern || !stmt.value) {
        fail("invalid destructuring assignment", stmt.loc);
    }
    if (stmt.pattern->kind == DestructPatternKind::Identifier) {
        fail("invalid destructuring assignment", stmt.loc);
    }
    validate_destructure_pattern_rhs(*stmt.pattern, *stmt.value, stmt.loc);
    const Register source = compile_expr(*stmt.value);
    compile_destructure_pattern(*stmt.pattern, source, stmt.value.get(), stmt.loc);
}

void Compiler::reject_bare_delay_call(const Expr& expr) {
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (call->name == "delay") {
            fail("'delay' must be awaited", call->loc);
        }
    }
}

void Compiler::compile_expr_stmt(const ExprStmt& stmt) {
    if (!stmt.expression) {
        fail("invalid statement", stmt.loc);
    }
    reject_bare_delay_call(*stmt.expression);
    compile_expr(*stmt.expression);
}

void Compiler::compile_print(const PrintStmt& stmt) {
    if (stmt.expressions.empty()) {
        fail("invalid statement", stmt.loc);
    }
    for (const auto& expr : stmt.expressions) {
        if (!expr) {
            fail("invalid statement", stmt.loc);
        }
    }

    if (stmt.expressions.size() == 1) {
        const Register src = compile_expr(*stmt.expressions[0]);
        emit(Instruction{Opcode::Print, src, 0, 0});
        return;
    }

    const Register first = next_register_;
    for (std::size_t i = 0; i < stmt.expressions.size(); ++i) {
        alloc_register();
    }
    for (std::size_t i = 0; i < stmt.expressions.size(); ++i) {
        compile_expr_into(*stmt.expressions[i], first + static_cast<Register>(i));
    }
    emit(Instruction{Opcode::PrintN, first, static_cast<std::uint32_t>(stmt.expressions.size()), 0});
}

void Compiler::compile_block(const std::vector<std::unique_ptr<Stmt>>& statements, const Location& loc) {
    for (const auto& stmt : statements) {
        if (!stmt) {
            fail("invalid statement", loc);
        }
        if (dynamic_cast<const FuncStmt*>(stmt.get())) {
            continue;
        }
        compile_stmt(*stmt);
    }
}

void Compiler::compile_scoped_block(const std::vector<std::unique_ptr<Stmt>>& statements, const Location& loc) {
    push_scope();
    const std::vector<NestedFunction> nested = hoist_functions(statements);
    predeclare_statements(statements);
    compile_nested_functions(nested, true);
    emit_closures(nested);
    compile_block(statements, loc);
    pop_scope();
}

void Compiler::compile_if(const IfStmt& stmt) {
    if (!stmt.condition) {
        fail("invalid statement", stmt.loc);
    }

    std::vector<std::uint32_t> end_jumps;
    const bool has_else = stmt.else_body.has_value();

    auto compile_conditional = [&](const Expr& condition, const std::vector<std::unique_ptr<Stmt>>& body,
                                   bool jump_to_end) {
        const Register cond = compile_expr(condition);
        const std::uint32_t jump_false = emit_jump_if_false(cond);
        compile_scoped_block(body, stmt.loc);
        if (jump_to_end) {
            end_jumps.push_back(emit_jump());
        }
        patch_jump(jump_false, current_ip());
    };

    compile_conditional(*stmt.condition, stmt.body, !stmt.elif_branches.empty() || has_else);

    for (std::size_t i = 0; i < stmt.elif_branches.size(); ++i) {
        const ElifBranch& branch = stmt.elif_branches[i];
        if (!branch.condition) {
            fail("invalid statement", branch.loc);
        }
        const bool more = i + 1 < stmt.elif_branches.size() || has_else;
        compile_conditional(*branch.condition, branch.body, more);
    }

    if (has_else) {
        compile_scoped_block(*stmt.else_body, stmt.loc);
    }

    const std::uint32_t end = current_ip();
    for (const std::uint32_t jump : end_jumps) {
        patch_jump(jump, end);
    }
}

void Compiler::compile_integer_into(Register dest, std::int64_t value) {
    Constant constant;
    constant.kind = ConstantKind::Integer;
    constant.lexeme = std::to_string(value);
    emit(Instruction{Opcode::LoadConst, dest, intern_constant(std::move(constant)), 0});
}

void Compiler::compile_duration_into(const Expr& expr, Register dest) {
    const auto* literal = dynamic_cast<const LiteralExpr*>(&expr);
    if (!literal || literal->kind != LiteralExpr::Kind::Duration) {
        fail("expected a duration literal", expr.loc);
    }
    DurationParseError parse_error;
    const std::optional<std::int64_t> ms = duration_lexeme_to_milliseconds(literal->lexeme, &parse_error);
    if (!ms) {
        fail(parse_error.message.empty() ? "invalid duration literal" : parse_error.message, expr.loc);
    }
    compile_integer_into(dest, *ms);
}

void Compiler::compile_embed_call_into(const Location& loc, const std::vector<std::unique_ptr<Expr>>& arguments,
                                       Register dest, bool text_mode) {
    if (arguments.size() != 1) {
        fail(text_mode ? "embed.text expects 1 argument" : "embed expects 1 argument", loc);
    }
    const auto* literal = dynamic_cast<const LiteralExpr*>(arguments[0].get());
    if (literal == nullptr || literal->kind != LiteralExpr::Kind::String) {
        fail("embed path must be a string literal", loc);
    }
    Constant path_constant;
    path_constant.kind = ConstantKind::String;
    path_constant.lexeme = literal->lexeme;
    Constant embedded;
    try {
        const std::vector<std::uint8_t> bytes =
            load_embed_bytes(bytecode_, bytecode_.source_file, path_constant);
        embedded = text_mode ? make_embed_text_constant(bytes) : make_embed_bytes_constant(bytes);
    } catch (const std::runtime_error& error) {
        fail(error.what(), loc);
    }
    emit(Instruction{Opcode::LoadConst, dest, intern_constant(embedded), 0});
}

void Compiler::compile_builtin_call_arguments(const CallExpr& call, Register first) {
    if (call.name == "delay") {
        if (call.arguments.size() != 1) {
            fail("'delay' expects 1 argument", call.loc);
        }
        if (!call.arguments[0]) {
            fail("invalid expression", call.loc);
        }
        compile_duration_into(*call.arguments[0], first);
        return;
    }
    if (call.name == "timeout") {
        if (call.arguments.size() != 2) {
            fail("'timeout' expects 2 arguments", call.loc);
        }
        if (!call.arguments[0] || !call.arguments[1]) {
            fail("invalid expression", call.loc);
        }
        compile_expr_into(*call.arguments[0], first);
        compile_duration_into(*call.arguments[1], first + 1);
        return;
    }
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
        if (!call.arguments[i]) {
            fail("invalid expression", call.loc);
        }
        compile_expr_into(*call.arguments[i], first + static_cast<Register>(i));
    }
}

void Compiler::patch_loop_jumps(const std::vector<std::uint32_t>& jumps, std::uint32_t target) {
    for (const std::uint32_t jump : jumps) {
        patch_jump(jump, target);
    }
}

void Compiler::compile_while(const WhileStmt& stmt) {
    if (!stmt.condition) {
        fail("invalid statement", stmt.loc);
    }

    loop_stack_.push_back(LoopContext{});
    const std::uint32_t loop_start = current_ip();
    const Register cond = compile_expr(*stmt.condition);
    const std::uint32_t jump_false = emit_jump_if_false(cond);
    compile_scoped_block(stmt.body, stmt.loc);
    emit(Instruction{Opcode::Jump, loop_start, 0, 0});
    const std::uint32_t loop_end = current_ip();
    patch_jump(jump_false, loop_end);
    patch_loop_jumps(loop_stack_.back().break_jumps, loop_end);
    patch_loop_jumps(loop_stack_.back().continue_jumps, loop_start);
    loop_stack_.pop_back();
}

void Compiler::compile_for(const ForStmt& stmt) {
    if (!stmt.iterable) {
        fail("invalid statement", stmt.loc);
    }

    const Register collection = compile_expr(*stmt.iterable);
    const Register iterator = alloc_register();
    bool user_class_iterable = false;
    std::optional<std::uint32_t> next_call;
    if (const std::optional<std::uint32_t> class_index = infer_class_index(*stmt.iterable)) {
        if (const std::optional<ClassMethodLookup> iter_method = find_class_method(*class_index, "iter")) {
            user_class_iterable = true;
            const ClassMethodInfo& iter_info =
                bytecode_.classes[iter_method->class_index].methods[iter_method->method_index];
            if (iter_info.iterator_class_index != kInvalidIndex) {
                if (const std::optional<ClassMethodLookup> next_method =
                        find_class_method(iter_info.iterator_class_index, "next")) {
                    next_call = intern_class_call(ClassCallKind::Method, next_method->class_index,
                                                  next_method->function_index, 0);
                }
            }
            const std::uint32_t info = intern_class_call(ClassCallKind::Method, iter_method->class_index,
                                                           iter_method->function_index, 0);
            emit(Instruction{Opcode::CallClassMethod, iterator, collection, info});
        } else {
            emit(Instruction{Opcode::GetIter, iterator, collection, 0});
        }
    } else {
        emit(Instruction{Opcode::GetIter, iterator, collection, 0});
    }

    const Register item = alloc_register();
    push_scope();
    resolve_assign(stmt.item, stmt.loc);
    loop_stack_.push_back(LoopContext{});

    const bool use_user_iterator_loop =
        user_class_iterable || infer_class_index(*stmt.iterable).has_value();

    if (use_user_iterator_loop) {
        if (!next_call) {
            if (const std::optional<std::uint32_t> class_index = infer_class_index(*stmt.iterable)) {
                if (const std::optional<ClassMethodLookup> next_method = find_class_method(*class_index, "next")) {
                    next_call = intern_class_call(ClassCallKind::Method, next_method->class_index,
                                                  next_method->function_index, 0);
                }
            }
        }
        const std::uint32_t stop_index = find_class_by_name("StopIteration");
        if (stop_index == kInvalidIndex) {
            fail("unknown exception type 'StopIteration'", stmt.loc);
        }

        HandlerInfo handler;
        const std::uint32_t handler_index = static_cast<std::uint32_t>(bytecode_.handlers.size());
        bytecode_.handlers.push_back(handler);

        try_stack_.push_back(TryContext{handler_index, false});
        const std::uint32_t loop_start = current_ip();
        emit(Instruction{Opcode::EnterTry, handler_index, 0, 0});
        const std::uint32_t iter_next_index = current_ip();
        if (next_call) {
            emit(Instruction{Opcode::CallClassMethod, item, iterator, *next_call});
        } else {
            emit(Instruction{Opcode::IterNext, item, iterator, 0});
        }
        emit_store_name(stmt.item, item, stmt.loc);
        compile_scoped_block(stmt.body, stmt.loc);
        const std::uint32_t continue_target = current_ip();
        emit(Instruction{Opcode::LeaveTry, handler_index, 0, 0});
        emit(Instruction{Opcode::Jump, loop_start, 0, 0});
        const std::uint32_t catch_ip = current_ip();
        emit(Instruction{Opcode::LeaveTry, handler_index, 0, 0});
        const std::uint32_t loop_end = current_ip();
        if (!next_call) {
            bytecode_.instructions[iter_next_index].c = loop_end;
        }

        CatchEntry entry;
        entry.catch_ip = catch_ip;
        entry.catch_slot = 0xFFFFFFFFu;
        entry.catch_class_index = stop_index;
        bytecode_.handlers[handler_index].catches.push_back(entry);
        bytecode_.handlers[handler_index].end_ip = loop_end;

        try_stack_.pop_back();
        patch_loop_jumps(loop_stack_.back().break_jumps, loop_end);
        patch_loop_jumps(loop_stack_.back().continue_jumps, continue_target);
        loop_stack_.pop_back();
        pop_scope();
        return;
    }

    const std::uint32_t loop_start = current_ip();
    const std::uint32_t iter_next_index = current_ip();
    emit(Instruction{Opcode::IterNext, item, iterator, 0});
    emit_store_name(stmt.item, item, stmt.loc);
    compile_scoped_block(stmt.body, stmt.loc);
    const std::uint32_t continue_target = current_ip();
    emit(Instruction{Opcode::Jump, loop_start, 0, 0});
    const std::uint32_t loop_end = current_ip();
    bytecode_.instructions[iter_next_index].c = loop_end;
    patch_loop_jumps(loop_stack_.back().break_jumps, loop_end);
    patch_loop_jumps(loop_stack_.back().continue_jumps, continue_target);
    loop_stack_.pop_back();
    pop_scope();
}

void Compiler::compile_async_for(const AsyncForStmt& stmt) {
    if (!stmt.iterable) {
        fail("invalid statement", stmt.loc);
    }
    if (async_function_depth_ == 0) {
        fail("async for outside async function", stmt.loc);
    }

    const Register collection = compile_expr(*stmt.iterable);
    const Register iterator = alloc_register();
    if (const std::optional<std::uint32_t> class_index = infer_class_index(*stmt.iterable)) {
        if (const std::optional<ClassMethodLookup> iter_method = find_class_method(*class_index, "iter")) {
            const std::uint32_t info = intern_class_call(ClassCallKind::Method, iter_method->class_index,
                                                           iter_method->function_index, 0);
            emit(Instruction{Opcode::CallClassMethod, iterator, collection, info});
        } else {
            emit(Instruction{Opcode::GetIter, iterator, collection, 0});
        }
    } else {
        emit(Instruction{Opcode::GetIter, iterator, collection, 0});
    }

    const Register item = alloc_register();
    push_scope();
    resolve_assign(stmt.item, stmt.loc);
    loop_stack_.push_back(LoopContext{});

    const std::uint32_t stop_index = find_class_by_name("StopIteration");
    if (stop_index == 0xFFFFFFFFu) {
        fail("unknown exception type 'StopIteration'", stmt.loc);
    }

    std::optional<std::uint32_t> next_call;
    if (const std::optional<std::uint32_t> class_index = infer_class_index(*stmt.iterable)) {
        if (const std::optional<ClassMethodLookup> anext = find_class_method(*class_index, "anext")) {
            next_call = intern_class_call(ClassCallKind::Method, anext->class_index, anext->function_index, 0);
        } else if (const std::optional<ClassMethodLookup> next_method = find_class_method(*class_index, "next")) {
            next_call = intern_class_call(ClassCallKind::Method, next_method->class_index, next_method->function_index, 0);
        }
    }
    if (!next_call) {
        fail("async for requires an iterable with 'anext' or 'next'", stmt.loc);
    }

    HandlerInfo handler;
    const std::uint32_t handler_index = static_cast<std::uint32_t>(bytecode_.handlers.size());
    bytecode_.handlers.push_back(handler);
    try_stack_.push_back(TryContext{handler_index, false});

    const std::uint32_t loop_start = current_ip();
    emit(Instruction{Opcode::EnterTry, handler_index, 0, 0});
    const Register next_task = alloc_register();
    emit(Instruction{Opcode::CallClassMethod, next_task, iterator, *next_call});
    emit(Instruction{Opcode::Await, item, next_task, 0});
    emit_store_name(stmt.item, item, stmt.loc);
    compile_scoped_block(stmt.body, stmt.loc);
    const std::uint32_t continue_target = current_ip();
    emit(Instruction{Opcode::LeaveTry, handler_index, 0, 0});
    emit(Instruction{Opcode::Jump, loop_start, 0, 0});
    const std::uint32_t catch_ip = current_ip();
    emit(Instruction{Opcode::LeaveTry, handler_index, 0, 0});
    const std::uint32_t loop_end = current_ip();

    CatchEntry entry;
    entry.catch_ip = catch_ip;
    entry.catch_slot = 0xFFFFFFFFu;
    entry.catch_class_index = stop_index;
    bytecode_.handlers[handler_index].catches.push_back(entry);
    bytecode_.handlers[handler_index].end_ip = loop_end;

    try_stack_.pop_back();
    patch_loop_jumps(loop_stack_.back().break_jumps, loop_end);
    patch_loop_jumps(loop_stack_.back().continue_jumps, continue_target);
    loop_stack_.pop_back();
    pop_scope();
}

void Compiler::compile_async_with(const AsyncWithStmt& stmt) {
    if (!stmt.manager) {
        fail("invalid statement", stmt.loc);
    }
    if (async_function_depth_ == 0) {
        fail("async with outside async function", stmt.loc);
    }

    const Register manager = compile_expr(*stmt.manager);
    declare_variable("__async_mgr", std::nullopt, stmt.loc);
    emit_store_name("__async_mgr", manager, stmt.loc);
    const Register entered = alloc_register();
    const std::optional<std::uint32_t> class_index = infer_class_index(*stmt.manager);
    std::string enter_name = "enter";
    std::string exit_name = "exit";
    if (class_index && find_class_method(*class_index, "aenter")) {
        enter_name = "aenter";
    }
    if (class_index && find_class_method(*class_index, "aexit")) {
        exit_name = "aexit";
    }
    if (const std::optional<ClassMethodLookup> enter_method = class_index
                                                                 ? find_class_method(*class_index, enter_name)
                                                                 : std::nullopt) {
        const std::uint32_t enter_info = intern_class_call(ClassCallKind::Method, enter_method->class_index,
                                                             enter_method->function_index, 0);
        const Register mgr_reg = alloc_register();
        emit_load_name(mgr_reg, "__async_mgr", stmt.loc);
        const FunctionInfo& enter_fn = bytecode_.functions[enter_method->function_index];
        const bool task_group_native =
            class_index && *class_index < bytecode_.classes.size() &&
            bytecode_.classes[*class_index].native_class_kind ==
                static_cast<std::uint32_t>(NativeClassKind::TaskGroup);
        const bool await_enter = enter_fn.is_async || task_group_native;
        if (await_enter) {
            const Register enter_result = alloc_register();
            emit(Instruction{Opcode::CallClassMethod, enter_result, mgr_reg, enter_info});
            emit(Instruction{Opcode::Await, entered, enter_result, 0});
        } else {
            emit(Instruction{Opcode::CallClassMethod, entered, mgr_reg, enter_info});
        }
    } else {
        fail("async with manager has no enter method", stmt.loc);
    }

    if (stmt.binding) {
        resolve_assign(*stmt.binding, stmt.loc);
        emit_store_name(*stmt.binding, entered, stmt.loc);
    }

    HandlerInfo handler;
    const std::uint32_t handler_index = static_cast<std::uint32_t>(bytecode_.handlers.size());
    bytecode_.handlers.push_back(handler);
    try_stack_.push_back(TryContext{handler_index, true});

    emit(Instruction{Opcode::EnterTry, handler_index, 0, 0});
    compile_block(stmt.body, stmt.loc);
    const std::uint32_t after_body = current_ip();
    emit(Instruction{Opcode::LeaveTry, handler_index, 0, 0});

    const std::uint32_t finally_ip = current_ip();
    bytecode_.handlers[handler_index].finally_ip = finally_ip;

    if (const std::optional<ClassMethodLookup> exit_method =
            class_index ? find_class_method(*class_index, exit_name) : std::nullopt) {
        const Register first = next_register_;
        alloc_register();
        alloc_register();
        emit_load_name(first, "__async_mgr", stmt.loc);
        emit(Instruction{Opcode::LoadPendingException, first + 1, 0, 0});
        const std::uint32_t exit_info =
            intern_class_call(ClassCallKind::Method, exit_method->class_index, exit_method->function_index, 1);
        const Register exit_result = alloc_register();
        emit(Instruction{Opcode::CallClassMethod, exit_result, first, exit_info});
        const FunctionInfo& exit_fn = bytecode_.functions[exit_method->function_index];
        const bool task_group_native =
            class_index && *class_index < bytecode_.classes.size() &&
            bytecode_.classes[*class_index].native_class_kind ==
                static_cast<std::uint32_t>(NativeClassKind::TaskGroup);
        if (exit_fn.is_async || task_group_native) {
            emit(Instruction{Opcode::Await, exit_result, exit_result, 0});
        }
    }
    emit(Instruction{Opcode::EndFinally, 0, 0, 0});
    bytecode_.handlers[handler_index].end_ip = current_ip();
    try_stack_.pop_back();
    (void)after_body;
}

std::optional<std::uint32_t> Compiler::detect_iterator_class_from_method(const FuncStmt& method) const {
    if (method.name != "iter") {
        return std::nullopt;
    }

    const auto scan_block =
        [&](const auto& self, const std::vector<std::unique_ptr<Stmt>>& body) -> std::optional<std::uint32_t> {
        for (const std::unique_ptr<Stmt>& stmt : body) {
            if (!stmt) {
                continue;
            }
            if (const auto* ret = dynamic_cast<const ReturnStmt*>(stmt.get())) {
                if (!ret->value) {
                    continue;
                }
                if (const auto* call = dynamic_cast<const CallExpr*>(ret->value.get())) {
                    if (const Symbol* symbol = lookup_symbol(call->name)) {
                        if (symbol->kind == SymbolKind::Class) {
                            return symbol->class_index;
                        }
                    }
                }
            }
            if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt.get())) {
                if (auto found = self(self, if_stmt->body)) {
                    return found;
                }
                for (const ElifBranch& branch : if_stmt->elif_branches) {
                    if (auto found = self(self, branch.body)) {
                        return found;
                    }
                }
                if (if_stmt->else_body) {
                    if (auto found = self(self, *if_stmt->else_body)) {
                        return found;
                    }
                }
            }
            if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt.get())) {
                if (auto found = self(self, while_stmt->body)) {
                    return found;
                }
            }
            if (const auto* for_stmt = dynamic_cast<const ForStmt*>(stmt.get())) {
                if (auto found = self(self, for_stmt->body)) {
                    return found;
                }
            }
            if (const auto* try_stmt = dynamic_cast<const TryStmt*>(stmt.get())) {
                if (auto found = self(self, try_stmt->try_body)) {
                    return found;
                }
            }
        }
        return std::nullopt;
    };

    return scan_block(scan_block, method.body);
}

void Compiler::compile_break(const BreakStmt& stmt) {
    if (loop_stack_.empty()) {
        fail("break outside loop", stmt.loc);
    }
    if (!try_stack_.empty() && try_stack_.back().has_finally) {
        const TryContext& context = try_stack_.back();
        const std::uint32_t defer_index = current_ip();
        emit(Instruction{Opcode::DeferBreak, context.handler_index, 0, 0});
        const std::uint32_t jump_index = emit_jump();
        bytecode_.instructions[defer_index].b = jump_index;
        loop_stack_.back().break_jumps.push_back(jump_index);
        return;
    }
    const std::uint32_t jump_index = emit_jump();
    loop_stack_.back().break_jumps.push_back(jump_index);
    if (!try_stack_.empty()) {
        emit(Instruction{Opcode::LeaveTry, try_stack_.back().handler_index, 0, 0});
    }
}

void Compiler::compile_continue(const ContinueStmt& stmt) {
    if (loop_stack_.empty()) {
        fail("continue outside loop", stmt.loc);
    }
    if (!try_stack_.empty() && try_stack_.back().has_finally) {
        const TryContext& context = try_stack_.back();
        const std::uint32_t defer_index = current_ip();
        emit(Instruction{Opcode::DeferContinue, context.handler_index, 0, 0});
        const std::uint32_t jump_index = emit_jump();
        bytecode_.instructions[defer_index].b = jump_index;
        loop_stack_.back().continue_jumps.push_back(jump_index);
        return;
    }
    const std::uint32_t jump_index = emit_jump();
    loop_stack_.back().continue_jumps.push_back(jump_index);
    if (!try_stack_.empty()) {
        emit(Instruction{Opcode::LeaveTry, try_stack_.back().handler_index, 0, 0});
    }
}

void Compiler::compile_logical_into(const BinaryExpr& expr, Register dest) {
    compile_expr_into(*expr.left, dest);
    const std::uint32_t skip = is_logical_and(expr.op) ? emit_jump_if_false(dest) : emit_jump_if_true(dest);
    compile_expr_into(*expr.right, dest);
    patch_jump(skip, current_ip());
}

void Compiler::compile_null_coalesce_into(const BinaryExpr& expr, Register dest) {
    if (!expr.left || !expr.right) {
        fail("invalid expression", expr.loc);
    }
    compile_expr_into(*expr.left, dest);
    const Register null_reg = alloc_register();
    emit(Instruction{Opcode::LoadConst, null_reg, intern_constant(Constant{ConstantKind::Null, "null"}), 0});
    const Register is_null = alloc_register();
    emit(Instruction{Opcode::Equal, is_null, dest, null_reg});
    const std::uint32_t skip = emit_jump_if_false(is_null);
    compile_expr_into(*expr.right, dest);
    patch_jump(skip, current_ip());
}

void Compiler::compile_ternary_into(const TernaryExpr& expr, Register dest) {
    if (!expr.condition || !expr.then_expr || !expr.else_expr) {
        fail("invalid expression", expr.loc);
    }
    const Register cond = compile_expr(*expr.condition);
    const std::uint32_t jump_false = emit_jump_if_false(cond);
    compile_expr_into(*expr.then_expr, dest);
    const std::uint32_t jump_end = emit_jump();
    patch_jump(jump_false, current_ip());
    compile_expr_into(*expr.else_expr, dest);
    patch_jump(jump_end, current_ip());
}

void Compiler::compile_membership_into(const MembershipExpr& expr, Register dest) {
    if (!expr.value || !expr.container) {
        fail("invalid expression", expr.loc);
    }
    const Register value = compile_expr(*expr.value);
    const Register container = compile_expr(*expr.container);
    emit(Instruction{Opcode::Contains, dest, value, container});
    if (expr.negated) {
        emit(Instruction{Opcode::UnaryNot, dest, dest, 0});
    }
}

void Compiler::reject_index_write(const Expr& object, const Location& loc) {
    if (infer_expr(object) == StaticType::Tuple) {
        fail("tuple is immutable", loc);
    }
    if (infer_expr(object) == StaticType::Set) {
        fail("cannot index set", loc);
    }
}

void Compiler::compile_assign_expr_into(const AssignExpr& expr, Register dest) {
    if (!expr.target || !expr.value) {
        fail("invalid assignment", expr.loc);
    }

    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(expr.target.get())) {
        if (ident->name == "this" || ident->name == "super" || ident->name == "parent") {
            fail("cannot assign to '" + ident->name + "'", expr.loc);
        }
        reject_const_assignment(ident->name, ident->loc);
        if (expr.op == TokenKind::Equal) {
            resolve_assign(ident->name, ident->loc);
            VariableType& types = variable_types_[ident->name];
            const TypeDescriptor source = infer_type_descriptor(*expr.value);
            if (types.declared) {
                check_assignable(*types.declared, source, expr.value->loc);
                check_collection_literal(*expr.value, *types.declared, expr.value->loc);
            } else {
                types.inferred = source;
            }
            compile_expr_into(*expr.value, dest);
            if (types.declared) {
                apply_assign_conversions(dest, *types.declared, source, expr.value->loc);
            }
            emit_store_name(ident->name, dest, ident->loc);
            return;
        }
        emit_load_name(dest, ident->name, ident->loc);
        const Register rhs = compile_expr(*expr.value);
        emit(Instruction{binary_opcode(compound_binary(expr.op), expr.loc), dest, dest, rhs});
        VariableType& types = variable_types_[ident->name];
        const TypeDescriptor source = infer_type_descriptor(expr);
        if (types.declared) {
            check_assignable(*types.declared, source, expr.loc);
        } else {
            types.inferred = source;
        }
        emit_store_name(ident->name, dest, ident->loc);
        return;
    }

    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expr.target.get())) {
        if (!field->object) {
            fail("invalid assignment", expr.loc);
        }
        const std::optional<std::uint32_t> class_index = infer_class_index(*field->object);
        if (class_index) {
            if (const std::optional<ClassPropertyLookup> property =
                    find_class_property(*class_index, field->field)) {
                if (!property->has_setter) {
                    fail("cannot assign to read-only property '" + field->field + "'", expr.loc);
                }
                if (expr.op != TokenKind::Equal) {
                    fail("compound assignment to property is not supported", expr.loc);
                }
                check_class_property_access(*property, true, expr.loc);
                const Register first = next_register_;
                alloc_register();
                alloc_register();
                compile_expr_into(*field->object, first);
                compile_expr_into(*expr.value, first + 1);
                const std::uint32_t call_index =
                    intern_class_call(ClassCallKind::Method, property->class_index, property->setter_function_index, 1);
                emit(Instruction{Opcode::CallClassMethod, dest, first, call_index});
                return;
            }
            const ClassFieldLookup field_lookup = lookup_class_field(*class_index, field->field, expr.loc);
            check_class_field_access(field_lookup, expr.loc);
            const Register object = compile_expr(*field->object);
            if (expr.op == TokenKind::Equal) {
                compile_expr_into(*expr.value, dest);
                emit(Instruction{Opcode::SetField, object, field_lookup.field_index, dest});
                return;
            }
            emit(Instruction{Opcode::GetField, dest, object, field_lookup.field_index});
            const Register rhs = compile_expr(*expr.value);
            emit(Instruction{binary_opcode(compound_binary(expr.op), expr.loc), dest, dest, rhs});
            emit(Instruction{Opcode::SetField, object, field_lookup.field_index, dest});
            return;
        }
        const std::optional<std::uint32_t> struct_index = infer_struct_index(*field->object);
        if (!struct_index) {
            if (const auto* ident_obj = dynamic_cast<const IdentifierExpr*>(field->object.get())) {
                const Symbol* owner = lookup_symbol(ident_obj->name);
                if (owner && owner->kind == SymbolKind::Variable) {
                    const Register object = compile_expr(*field->object);
                    const Register key = emit_string_constant(field->field);
                    if (expr.op == TokenKind::Equal) {
                        compile_expr_into(*expr.value, dest);
                        emit(Instruction{Opcode::SetIndex, object, key, dest});
                        return;
                    }
                    emit(Instruction{Opcode::GetIndex, dest, object, key});
                    const Register rhs = compile_expr(*expr.value);
                    emit(Instruction{binary_opcode(compound_binary(expr.op), expr.loc), dest, dest, rhs});
                    emit(Instruction{Opcode::SetIndex, object, key, dest});
                    return;
                }
            }
            fail("cannot resolve field '" + field->field + "'", expr.loc);
        }
        check_field_access(*struct_index, field->field, expr.loc);
        const std::uint32_t field_index = resolve_field_index(*struct_index, field->field, expr.loc);
        const Register object = compile_expr(*field->object);
        if (expr.op == TokenKind::Equal) {
            compile_expr_into(*expr.value, dest);
            emit(Instruction{Opcode::SetField, object, field_index, dest});
            return;
        }
        emit(Instruction{Opcode::GetField, dest, object, field_index});
        const Register rhs = compile_expr(*expr.value);
        emit(Instruction{binary_opcode(compound_binary(expr.op), expr.loc), dest, dest, rhs});
        emit(Instruction{Opcode::SetField, object, field_index, dest});
        return;
    }

    const auto* index = dynamic_cast<const IndexExpr*>(expr.target.get());
    if (!index || !index->object || !index->index) {
        fail("invalid assignment", expr.loc);
    }
    reject_index_write(*index->object, expr.loc);
    const Register object = compile_expr(*index->object);
    const Register key = compile_expr(*index->index);
    if (expr.op == TokenKind::Equal) {
        compile_expr_into(*expr.value, dest);
        emit(Instruction{Opcode::SetIndex, object, key, dest});
        return;
    }
    emit(Instruction{Opcode::GetIndex, dest, object, key});
    const Register rhs = compile_expr(*expr.value);
    emit(Instruction{binary_opcode(compound_binary(expr.op), expr.loc), dest, dest, rhs});
    emit(Instruction{Opcode::SetIndex, object, key, dest});
}

void Compiler::compile_update_into(const UpdateExpr& expr, Register dest) {
    if (!expr.target) {
        fail("invalid increment", expr.loc);
    }
    const Opcode arith = expr.op == TokenKind::PlusPlus ? Opcode::Add : Opcode::Sub;
    const Register delta = alloc_register();
    Constant one;
    one.kind = ConstantKind::Integer;
    one.lexeme = "1";
    emit(Instruction{Opcode::LoadConst, delta, intern_constant(std::move(one)), 0});

    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(expr.target.get())) {
        if (expr.prefix) {
            emit_load_name(dest, ident->name, ident->loc);
            emit(Instruction{arith, dest, dest, delta});
            emit_store_name(ident->name, dest, ident->loc);
            return;
        }
        emit_load_name(dest, ident->name, ident->loc);
        const Register next = alloc_register();
        emit(Instruction{arith, next, dest, delta});
        emit_store_name(ident->name, next, ident->loc);
        return;
    }

    const auto* index = dynamic_cast<const IndexExpr*>(expr.target.get());
    if (!index || !index->object || !index->index) {
        fail("invalid increment", expr.loc);
    }
    reject_index_write(*index->object, expr.loc);
    const Register object = compile_expr(*index->object);
    const Register key = compile_expr(*index->index);
    if (expr.prefix) {
        emit(Instruction{Opcode::GetIndex, dest, object, key});
        emit(Instruction{arith, dest, dest, delta});
        emit(Instruction{Opcode::SetIndex, object, key, dest});
        return;
    }
    emit(Instruction{Opcode::GetIndex, dest, object, key});
    const Register next = alloc_register();
    emit(Instruction{arith, next, dest, delta});
    emit(Instruction{Opcode::SetIndex, object, key, next});
}

void Compiler::push_scope() {
    scope_stack_.emplace_back();
}

void Compiler::pop_scope() {
    if (scope_stack_.empty()) {
        fail("invalid bytecode", Location{});
    }
    std::vector<ScopeBinding> bindings = std::move(scope_stack_.back().bindings);
    scope_stack_.pop_back();
    const std::uint32_t scope_end_ip = static_cast<std::uint32_t>(bytecode_.instructions.size());
    for (auto it = bindings.rbegin(); it != bindings.rend(); ++it) {
        if (it->existed) {
            variable_index_[it->name] = *it->previous_index;
            variable_local_[it->name] = it->previous_local;
            if (it->previous_type) {
                variable_types_[it->name] = *it->previous_type;
            } else {
                variable_types_.erase(it->name);
            }
        } else {
            if (in_function_) {
                close_debug_local_scope(it->name, scope_end_ip);
            }
            variable_index_.erase(it->name);
            variable_types_.erase(it->name);
            variable_local_.erase(it->name);
        }
    }
}

bool Compiler::current_scope_defines(const std::string& name) const {
    if (scope_stack_.empty()) {
        return false;
    }
    return scope_stack_.back().symbols.find(name) != scope_stack_.back().symbols.end();
}

std::optional<std::uint32_t> Compiler::lookup_variable(const std::string& name) const {
    const Symbol* symbol = lookup_symbol(name);
    if (!symbol || symbol->kind != SymbolKind::Variable) {
        return std::nullopt;
    }
    return symbol->slot;
}

std::uint32_t Compiler::introduce_variable(const std::string& name, const std::optional<std::string>& type_name,
                                           const Location& loc, bool explicit_declaration, MemberAccess access,
                                           bool is_const) {
    if (!in_function_) {
        check_module_level_access(access, loc);
    }
    if (scope_stack_.empty()) {
        fail("invalid bytecode", loc);
    }

    ScopeBinding binding;
    binding.name = name;
    const auto it = variable_index_.find(name);
    if (it != variable_index_.end()) {
        binding.existed = true;
        binding.previous_index = it->second;
        binding.previous_local = variable_is_local(name);
        const auto type_it = variable_types_.find(name);
        if (type_it != variable_types_.end()) {
            binding.previous_type = type_it->second;
        }
    }

    std::uint32_t index = 0;
    if (in_function_) {
        index = next_local_++;
        variable_local_[name] = true;
    } else {
        index = static_cast<std::uint32_t>(bytecode_.variables.size());
        bytecode_.variables.push_back(Variable{name, type_name});
        variable_local_[name] = false;
    }
    variable_index_[name] = index;
    VariableType types;
    if (type_name && !type_name->empty()) {
        types.declared = parse_type_descriptor(*type_name, loc);
    }
    variable_types_[name] = types;

    Symbol symbol;
    symbol.name = name;
    symbol.slot = index;
    symbol.types = types;
    symbol.kind = SymbolKind::Variable;
    symbol.access = access;
    symbol.explicit_declaration = explicit_declaration;
    symbol.is_const = is_const;
    symbol.local = in_function_;
    symbol.owner_function = current_function();
    scope_stack_.back().symbols.emplace(name, std::move(symbol));
    scope_stack_.back().bindings.push_back(std::move(binding));
    return index;
}

FunctionDebugInfo& Compiler::current_function_debug() {
    const std::uint32_t idx = current_function();
    if (idx == 0xFFFFFFFFu) {
        static FunctionDebugInfo empty;
        return empty;
    }
    if (bytecode_.function_debug.size() <= idx) {
        bytecode_.function_debug.resize(idx + 1);
    }
    return bytecode_.function_debug[idx];
}

void Compiler::record_debug_local(const std::string& name, std::uint32_t slot, DebugLocalKind kind,
                                  const Location& loc) {
    FunctionDebugInfo& info = current_function_debug();
    for (const DebugLocalBinding& existing : info.locals) {
        if (existing.slot == slot && existing.kind == kind) {
            return;
        }
    }
    DebugLocalBinding binding;
    binding.name = name;
    binding.slot = slot;
    binding.kind = kind;
    binding.decl_line = loc.line > 0 ? static_cast<std::uint32_t>(loc.line - source_line_base_) : 1u;
    if (binding.decl_line == 0) {
        binding.decl_line = 1;
    }
    binding.decl_column = loc.column > 0 ? static_cast<std::uint32_t>(loc.column) : 1u;
    binding.scope_start_ip = static_cast<std::uint32_t>(bytecode_.instructions.size());
    info.locals.push_back(std::move(binding));
}

void Compiler::close_debug_local_scope(const std::string& name, std::uint32_t end_ip) {
    FunctionDebugInfo& info = current_function_debug();
    for (auto it = info.locals.rbegin(); it != info.locals.rend(); ++it) {
        if (it->name == name && it->scope_end_ip == 0xFFFFFFFFu && it->kind == DebugLocalKind::Local) {
            it->scope_end_ip = end_ip;
            return;
        }
    }
}

std::uint32_t Compiler::declare_variable(const std::string& name, const std::optional<std::string>& type_name,
                                         const Location& loc, MemberAccess access, bool is_const) {
    if (current_scope_defines(name)) {
        fail("duplicate declaration of '" + name + "'", loc);
    }
    const std::uint32_t slot = introduce_variable(name, type_name, loc, true, access, is_const);
    if (in_function_ && current_function() != 0xFFFFFFFFu) {
        record_debug_local(name, slot, DebugLocalKind::Local, loc);
    }
    return slot;
}

std::uint32_t Compiler::resolve_assign(const std::string& name, const Location& loc) {
    if (const auto slot = lookup_variable(name)) {
        return *slot;
    }
    if (current_scope_defines(name)) {
        const Symbol* symbol = lookup_symbol(name);
        if (symbol && symbol->kind == SymbolKind::Variable && symbol->is_const) {
            fail("cannot assign to const '" + name + "'", loc);
        }
        if (symbol && symbol->kind == SymbolKind::Function) {
            fail("cannot assign to function '" + name + "'", loc);
        }
        if (symbol && symbol->kind == SymbolKind::Struct) {
            fail("cannot assign to struct type '" + name + "'", loc);
        }
        if (symbol && symbol->kind == SymbolKind::Class) {
            fail("cannot assign to class type '" + name + "'", loc);
        }
        if (symbol && symbol->kind == SymbolKind::Enum) {
            fail("cannot assign to enum type '" + name + "'", loc);
        }
    }
    return introduce_variable(name, std::nullopt, loc, false);
}

std::uint32_t Compiler::intern_binding(const std::string& name, const Location& loc) {
    return declare_variable(name, std::nullopt, loc);
}

const Compiler::Symbol* Compiler::lookup_symbol(const std::string& name) const {
    for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it) {
        const auto found = it->symbols.find(name);
        if (found != it->symbols.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

Compiler::Symbol* Compiler::lookup_symbol(const std::string& name) {
    return const_cast<Symbol*>(static_cast<const Compiler*>(this)->lookup_symbol(name));
}

void Compiler::mark_initialized(const std::string& name) {
    if (Symbol* symbol = lookup_symbol(name)) {
        symbol->initialized = true;
    }
}

bool Compiler::variable_is_local(const std::string& name) const {
    const auto it = variable_local_.find(name);
    return it != variable_local_.end() && it->second;
}

void Compiler::emit_load_name(Register dest, const std::string& name, const Location& loc) {
    const Symbol* symbol = lookup_symbol(name);
    if (!symbol) {
        fail("undefined variable '" + name + "'", loc);
    }
    if (symbol->kind == SymbolKind::Struct) {
        fail("cannot use struct type '" + name + "' as a value", loc);
    }
    if (symbol->kind == SymbolKind::Class) {
        fail("cannot use class type '" + name + "' as a value", loc);
    }
    if (symbol->kind == SymbolKind::Enum) {
        fail("cannot use enum type '" + name + "' as a value", loc);
    }
    if (symbol->kind == SymbolKind::Function) {
        if (symbol->import_module_index != 0xFFFFFFFFu) {
            Constant name_constant;
            name_constant.kind = ConstantKind::String;
            const std::string& export_name = symbol->import_export_name.empty() ? name : symbol->import_export_name;
            name_constant.lexeme = encode_string_lexeme(export_name);
            const std::uint32_t name_index = intern_constant(std::move(name_constant));
            emit(Instruction{Opcode::ImportName, dest, symbol->import_module_index, name_index});
            return;
        }
        if (!symbol->local) {
            emit(Instruction{Opcode::LoadFunction, dest, symbol->function_index, 0});
            return;
        }
        if (symbol->owner_function == current_function()) {
            emit(Instruction{Opcode::LoadLocal, dest, symbol->slot, 0});
            return;
        }
        const std::uint32_t upvalue = intern_capture(*symbol, loc);
        emit(Instruction{Opcode::LoadUpvalue, dest, upvalue, 0});
        return;
    }
    if (symbol->kind != SymbolKind::Variable) {
        fail("undefined variable '" + name + "'", loc);
    }
    if (symbol->owner_function == current_function() && !symbol->initialized) {
        fail("undefined variable '" + name + "'", loc);
    }
    if (!symbol->local) {
        emit(Instruction{Opcode::LoadVar, dest, symbol->slot, 0});
        return;
    }
    if (symbol->owner_function == current_function()) {
        emit(Instruction{Opcode::LoadLocal, dest, symbol->slot, 0});
        return;
    }
    const std::uint32_t upvalue = intern_capture(*symbol, loc);
    emit(Instruction{Opcode::LoadUpvalue, dest, upvalue, 0});
}

void Compiler::emit_store_name(const std::string& name, Register src, const Location& loc) {
    const Symbol* symbol = lookup_symbol(name);
    if (!symbol || symbol->kind != SymbolKind::Variable) {
        fail("undefined variable '" + name + "'", loc);
    }
    mark_initialized(name);
    if (!symbol->local) {
        emit(Instruction{Opcode::StoreVar, symbol->slot, src, 0});
        return;
    }
    if (symbol->owner_function == current_function()) {
        emit(Instruction{Opcode::StoreLocal, symbol->slot, src, 0});
        return;
    }
    const std::uint32_t upvalue = intern_capture(*symbol, loc);
    emit(Instruction{Opcode::StoreUpvalue, upvalue, src, 0});
}

void Compiler::emit_load_null(Register dest) {
    Constant constant;
    constant.kind = ConstantKind::Null;
    emit(Instruction{Opcode::LoadConst, dest, intern_constant(std::move(constant)), 0});
}

void Compiler::note_registers() {
    if (next_register_ > bytecode_.register_count) {
        bytecode_.register_count = next_register_;
    }
}

void Compiler::declare_struct(const StructStmt& stmt) {
    if (!in_function_) {
        check_module_level_access(stmt.access, stmt.loc);
    }
    if (current_scope_defines(stmt.name)) {
        fail("duplicate declaration of '" + stmt.name + "'", stmt.loc);
    }
    StructInfo info;
    info.name = stmt.name;
    info.annotations = copy_annotations(stmt.annotations);
    for (const GenericTypeParam& param : stmt.type_params) {
        info.generic_type_params.push_back(param.name);
        if (param.constraint_type_name) {
            info.generic_type_param_constraints.push_back(*param.constraint_type_name);
        } else {
            info.generic_type_param_constraints.push_back(std::string());
        }
    }
    for (const StructField& field : stmt.fields) {
        StructFieldInfo field_info;
        field_info.name = field.name;
        field_info.type_name = field.type_name;
        field_info.access = field.access;
        info.fields.push_back(std::move(field_info));
    }
    const auto index = static_cast<std::uint32_t>(bytecode_.structs.size());
    bytecode_.structs.push_back(std::move(info));

    Symbol symbol;
    symbol.name = stmt.name;
    symbol.kind = SymbolKind::Struct;
    symbol.access = stmt.access;
    symbol.struct_index = index;
    symbol.owner_function = current_function();
    scope_stack_.back().symbols.emplace(stmt.name, std::move(symbol));
}

void Compiler::declare_enum(const EnumStmt& stmt) {
    const MemberAccess access = access_with_annotations(stmt.access, stmt.annotations);
    if (!in_function_) {
        check_module_level_access(access, stmt.loc);
    }
    if (current_scope_defines(stmt.name)) {
        fail("duplicate declaration of '" + stmt.name + "'", stmt.loc);
    }
    if (stmt.members.empty()) {
        fail("empty enum", stmt.loc);
    }
    EnumInfo info;
    info.name = stmt.name;
    info.members = stmt.members;
    info.member_payloads = stmt.member_payloads;
    info.annotations = copy_annotations(stmt.annotations);
    if (info.member_payloads.size() != info.members.size()) {
        info.member_payloads.resize(info.members.size());
    }
    info.member_annotations.clear();
    info.member_annotations.reserve(stmt.members.size());
    for (std::size_t i = 0; i < stmt.members.size(); ++i) {
        if (i < stmt.member_annotations.size()) {
            info.member_annotations.push_back(copy_annotations(stmt.member_annotations[i]));
        } else {
            info.member_annotations.emplace_back();
        }
    }
    const auto index = static_cast<std::uint32_t>(bytecode_.enums.size());
    bytecode_.enums.push_back(std::move(info));

    Symbol symbol;
    symbol.name = stmt.name;
    symbol.kind = SymbolKind::Enum;
    symbol.access = access;
    symbol.enum_index = index;
    symbol.owner_function = current_function();
    scope_stack_.back().symbols.emplace(stmt.name, std::move(symbol));
}

void Compiler::declare_type_alias(const TypeAliasStmt& stmt) {
    const MemberAccess access = access_with_annotations(stmt.access, stmt.annotations);
    if (!in_function_) {
        check_module_level_access(access, stmt.loc);
    }
    if (current_scope_defines(stmt.name)) {
        fail("duplicate declaration of '" + stmt.name + "'", stmt.loc);
    }
    for (const StructInfo& info : bytecode_.structs) {
        if (info.name == stmt.name) {
            fail("typealias '" + stmt.name + "' conflicts with struct", stmt.loc);
        }
    }
    for (const ClassInfo& info : bytecode_.classes) {
        if (info.name == stmt.name) {
            fail("typealias '" + stmt.name + "' conflicts with class", stmt.loc);
        }
    }
    for (const EnumInfo& info : bytecode_.enums) {
        if (info.name == stmt.name) {
            fail("typealias '" + stmt.name + "' conflicts with enum", stmt.loc);
        }
    }
    for (const TypeAliasInfo& info : bytecode_.type_aliases) {
        if (info.name == stmt.name) {
            fail("duplicate typealias '" + stmt.name + "'", stmt.loc);
        }
    }
    TypeAliasInfo info;
    info.name = stmt.name;
    for (const GenericTypeParam& param : stmt.type_params) {
        info.type_params.push_back(param.name);
    }
    info.target_type = stmt.target_type;
    try {
        (void)parse_type_spec(info.target_type, stmt.loc, bytecode_, info.type_params.empty() ? nullptr : &info.type_params);
    } catch (const CompilerError& ex) {
        fail(ex.message(), stmt.loc);
    }
    bytecode_.type_aliases.push_back(std::move(info));
}

std::uint32_t Compiler::declare_class_function(const std::string& name, std::uint32_t arity, bool has_implicit_this,
                                               bool is_constructor) {
    FunctionInfo info;
    info.name = name;
    info.arity = arity;
    info.has_implicit_this = has_implicit_this;
    info.is_constructor = is_constructor;
    const auto index = static_cast<std::uint32_t>(bytecode_.functions.size());
    bytecode_.functions.push_back(std::move(info));
    return index;
}

void Compiler::predeclare_class(const ClassStmt& stmt) {
    if (!in_function_) {
        check_module_level_access(stmt.access, stmt.loc);
    }
    if (current_scope_defines(stmt.name)) {
        fail("duplicate declaration of '" + stmt.name + "'", stmt.loc);
    }
    ClassInfo info;
    info.name = stmt.name;
    info.annotations = copy_annotations(stmt.annotations);
    for (const GenericTypeParam& param : stmt.type_params) {
        info.generic_type_params.push_back(param.name);
        if (param.constraint_type_name) {
            info.generic_type_param_constraints.push_back(*param.constraint_type_name);
        } else {
            info.generic_type_param_constraints.push_back(std::string());
        }
    }
    const auto index = static_cast<std::uint32_t>(bytecode_.classes.size());
    bytecode_.classes.push_back(std::move(info));
    pending_class_parents_[stmt.name] = stmt.parent_name;

    Symbol symbol;
    symbol.name = stmt.name;
    symbol.kind = SymbolKind::Class;
    symbol.access = stmt.access;
    symbol.class_index = index;
    symbol.owner_function = current_function();
    scope_stack_.back().symbols.emplace(stmt.name, std::move(symbol));
}

void Compiler::finalize_class(const ClassStmt& stmt) {
    const Symbol* symbol = lookup_symbol(stmt.name);
    if (!symbol || symbol->kind != SymbolKind::Class) {
        fail("unknown class '" + stmt.name + "'", stmt.loc);
    }
    const std::uint32_t class_index = symbol->class_index;
    ClassInfo& info = bytecode_.classes[class_index];
    if (!stmt.base_type_specs.empty() || !stmt.implements.empty()) {
        resolve_class_bases(stmt, info);
    } else if (stmt.parent_name) {
        const std::uint32_t parent_index = find_class_by_name(*stmt.parent_name);
        if (parent_index == kInvalidIndex) {
            fail("unknown parent class '" + *stmt.parent_name + "'", stmt.loc);
        }
        if (parent_index == class_index) {
            fail("class cannot inherit from itself", stmt.loc);
        }
        info.parent_index = parent_index;
    }
    for (const StructField& field : stmt.fields) {
        StructFieldInfo field_info;
        field_info.name = field.name;
        field_info.type_name = field.type_name;
        field_info.access = field.access;
        info.fields.push_back(std::move(field_info));
    }
    if (stmt.constructor) {
        info.constructor_index = declare_class_function(stmt.name + ".constructor",
                                                        static_cast<std::uint32_t>(stmt.constructor->parameters.size()),
                                                        true, true);
        FunctionInfo& ctor = bytecode_.functions[info.constructor_index];
        for (const Parameter& param : stmt.constructor->parameters) {
            ctor.parameter_type_names.push_back(param.type_name);
        }
    }
    for (const auto& method : stmt.methods) {
        if (!method) {
            fail("invalid class method", stmt.loc);
        }
        ClassMethodInfo method_info;
        method_info.name = method->name;
        method_info.function_index =
            declare_class_function(stmt.name + "." + method->name,
                                   static_cast<std::uint32_t>(method->parameters.size()), true, false);
        FunctionInfo& method_fn = bytecode_.functions[method_info.function_index];
        method_fn.is_async = method->is_async;
        method_fn.return_type_name = method->return_type_name;
        for (const Parameter& param : method->parameters) {
            method_fn.parameter_type_names.push_back(param.type_name);
        }
        if (method->name == "iter") {
            if (const std::optional<std::uint32_t> iterator_class = detect_iterator_class_from_method(*method)) {
                method_info.iterator_class_index = *iterator_class;
            }
        }
        info.methods.push_back(std::move(method_info));
    }
    for (const ClassProperty& prop : stmt.properties) {
        ClassPropertyInfo* entry = nullptr;
        for (ClassPropertyInfo& existing : info.properties) {
            if (existing.name == prop.name) {
                entry = &existing;
                break;
            }
        }
        if (!entry) {
            ClassPropertyInfo created;
            created.name = prop.name;
            created.type_name = prop.type_name;
            info.properties.push_back(std::move(created));
            entry = &info.properties.back();
        } else if (prop.type_name) {
            entry->type_name = prop.type_name;
        }
        if (prop.getter) {
            entry->getter_access = prop.getter_access;
            entry->getter_function_index =
                declare_class_function(stmt.name + "." + prop.name + ".get", 0, true, false);
        }
        if (prop.setter) {
            entry->setter_access = prop.setter_access;
            entry->setter_function_index =
                declare_class_function(stmt.name + "." + prop.name + ".set",
                                       static_cast<std::uint32_t>(prop.setter->parameters.size()), true, false);
        }
    }
    validate_class_interfaces(stmt, info);
}

void Compiler::compile_class_member(const FuncStmt& stmt, std::uint32_t class_index, std::uint32_t function_index,
                                    bool is_constructor) {
    const bool saved_in_function = in_function_;
    const bool saved_emitting = emitting_body_;
    const int saved_async_depth = async_function_depth_;
    const std::uint32_t saved_registers = next_register_;
    const std::uint32_t saved_locals = next_local_;
    const std::optional<ClassCompileContext> saved_class_context = class_context_;

    in_function_ = true;
    emitting_body_ = false;
    next_register_ = 0;
    next_local_ = 1;
    function_stack_.push_back(function_index);
    if (stmt.is_async) {
        ++async_function_depth_;
        bytecode_.functions[function_index].returns_value = true;
    }
    class_context_ = ClassCompileContext{class_index, is_constructor};
    bytecode_.functions[function_index].parameter_type_names.clear();
    bytecode_.functions[function_index].return_type_name = stmt.return_type_name;
    for (const Parameter& param : stmt.parameters) {
        bytecode_.functions[function_index].parameter_type_names.push_back(param.type_name);
    }
    push_scope();

    for (const Parameter& param : stmt.parameters) {
        declare_variable(param.name, param.type_name, param.loc);
        mark_initialized(param.name);
    }

    const std::vector<NestedFunction> nested = hoist_functions(stmt.body);
    predeclare_statements(stmt.body);
    compile_nested_functions(nested, false);
    bytecode_.functions[function_index].entry = current_ip();
    emitting_body_ = true;
    emit_closures(nested);
    if (is_constructor) {
        const ClassInfo& class_info = bytecode_.classes[class_index];
        if (class_info.parent_index != kInvalidIndex) {
            const ClassInfo& parent = bytecode_.classes[class_info.parent_index];
            if (parent.constructor_index != kInvalidIndex) {
                const FunctionInfo& parent_ctor = bytecode_.functions[parent.constructor_index];
                if (parent_ctor.arity == 0 && !constructor_calls_parent(stmt.body)) {
                    emit_invoke_super(parent.constructor_index, 0, 0);
                }
            }
        }
    }
    compile_function_body(stmt.body, stmt.loc);
    bytecode_.functions[function_index].local_count = next_local_;
    bytecode_.functions[function_index].register_count = next_register_;
    note_registers();
    pop_scope();
    function_stack_.pop_back();
    if (stmt.is_async) {
        --async_function_depth_;
    }
    in_function_ = saved_in_function;
    emitting_body_ = saved_emitting;
    async_function_depth_ = saved_async_depth;
    next_register_ = saved_registers;
    next_local_ = saved_locals;
    class_context_ = saved_class_context;
}

void Compiler::compile_class(const ClassStmt& stmt) {
    if (stmt.name.empty()) {
        fail("invalid class", stmt.loc);
    }
    const Symbol* symbol = lookup_symbol(stmt.name);
    if (!symbol || symbol->kind != SymbolKind::Class) {
        fail("unknown class '" + stmt.name + "'", stmt.loc);
    }
    const std::uint32_t class_index = symbol->class_index;
    ClassInfo& info = bytecode_.classes[class_index];
    std::vector<std::string> class_type_params = info.generic_type_params;
    push_type_param_scope(class_type_params);
    if (stmt.constructor) {
        compile_class_member(*stmt.constructor, class_index, info.constructor_index, true);
    }
    for (std::size_t i = 0; i < stmt.methods.size(); ++i) {
        if (!stmt.methods[i]) {
            fail("invalid class method", stmt.loc);
        }
        compile_class_member(*stmt.methods[i], class_index, info.methods[i].function_index, false);
    }
    for (const ClassProperty& prop : stmt.properties) {
        for (const ClassPropertyInfo& property_info : info.properties) {
            if (property_info.name != prop.name) {
                continue;
            }
            if (prop.getter && property_info.getter_function_index != 0xFFFFFFFFu) {
                compile_class_member(*prop.getter, class_index, property_info.getter_function_index, false);
            }
            if (prop.setter && property_info.setter_function_index != 0xFFFFFFFFu) {
                compile_class_member(*prop.setter, class_index, property_info.setter_function_index, false);
            }
        }
    }
    pop_type_param_scope();
}

std::uint32_t Compiler::intern_class_call(ClassCallKind kind, std::uint32_t class_index, std::uint32_t function_index,
                                          std::uint32_t argc) {
    const std::string key = std::to_string(static_cast<int>(kind)) + ":" + std::to_string(class_index) + ":" +
                            std::to_string(function_index) + ":" + std::to_string(argc);
    const auto found = class_call_keys_.find(key);
    if (found != class_call_keys_.end()) {
        return found->second;
    }
    ClassCallInfo info;
    info.kind = kind;
    info.class_index = class_index;
    info.function_index = function_index;
    info.argc = argc;
    const auto index = static_cast<std::uint32_t>(bytecode_.class_calls.size());
    bytecode_.class_calls.push_back(std::move(info));
    class_call_keys_.emplace(key, index);
    return index;
}

void Compiler::emit_invoke_super(std::uint32_t function_index, std::uint32_t argc, Register first) {
    const std::uint32_t info = intern_class_call(ClassCallKind::SuperCtor, kInvalidIndex, function_index, argc);
    emit(Instruction{Opcode::InvokeSuper, first, info, 0});
}

void Compiler::check_class_field_access(const ClassFieldLookup& field, const Location& loc) const {
    if (field.access == MemberAccess::Public) {
        return;
    }
    if (!class_context_) {
        fail(std::string("cannot access ") + member_access_name(field.access) + " field", loc);
    }
    const std::uint32_t context_class = class_context_->class_index;
    if (field.access == MemberAccess::Private) {
        if (context_class != field.declaring_class_index) {
            fail(std::string("cannot access private field"), loc);
        }
        return;
    }
    if (!is_class_or_subclass(context_class, field.declaring_class_index)) {
        fail(std::string("cannot access protected field"), loc);
    }
}

Compiler::ClassFieldLookup Compiler::lookup_class_field(std::uint32_t class_index, const std::string& field,
                                                        const Location& loc) const {
    if (class_index >= bytecode_.classes.size()) {
        fail("invalid class type", loc);
    }
    const ClassInfo& info = bytecode_.classes[class_index];
    for (std::size_t i = 0; i < info.layout.size(); ++i) {
        if (info.layout[i].name == field) {
            ClassFieldLookup result;
            result.field_index = static_cast<std::uint32_t>(i);
            result.declaring_class_index = info.layout[i].declaring_class_index;
            result.access = info.layout[i].access;
            return result;
        }
    }
    fail("unknown field '" + field + "'", loc);
}

std::optional<Compiler::ClassFieldLookup> Compiler::find_class_field(std::uint32_t class_index,
                                                                     const std::string& field) const {
    if (class_index >= bytecode_.classes.size()) {
        return std::nullopt;
    }
    const ClassInfo& info = bytecode_.classes[class_index];
    for (std::size_t i = 0; i < info.layout.size(); ++i) {
        if (info.layout[i].name == field) {
            ClassFieldLookup result;
            result.field_index = static_cast<std::uint32_t>(i);
            result.declaring_class_index = info.layout[i].declaring_class_index;
            result.access = info.layout[i].access;
            return result;
        }
    }
    return std::nullopt;
}

Compiler::ClassMethodLookup Compiler::lookup_class_method(std::uint32_t class_index, const std::string& method,
                                                          const Location& loc, bool super_call) const {
    std::uint32_t start = class_index;
    if (super_call) {
        if (class_index >= bytecode_.classes.size()) {
            fail("invalid class type", loc);
        }
        const ClassInfo& info = bytecode_.classes[class_index];
        if (info.parent_index == kInvalidIndex) {
            fail("unknown method '" + method + "'", loc);
        }
        start = info.parent_index;
    }
    for (std::uint32_t current = start; current != kInvalidIndex; current = bytecode_.classes[current].parent_index) {
        const ClassInfo& info = bytecode_.classes[current];
        for (std::size_t i = 0; i < info.methods.size(); ++i) {
            if (info.methods[i].name == method) {
                ClassMethodLookup result;
                result.class_index = current;
                result.method_index = static_cast<std::uint32_t>(i);
                result.function_index = info.methods[i].function_index;
                return result;
            }
        }
    }
    fail("unknown method '" + method + "'", loc);
}

std::optional<Compiler::ClassMethodLookup> Compiler::find_class_method(std::uint32_t class_index,
                                                                         const std::string& method) const {
    for (std::uint32_t current = class_index; current != kInvalidIndex;
         current = bytecode_.classes[current].parent_index) {
        const ClassInfo& info = bytecode_.classes[current];
        for (std::size_t i = 0; i < info.methods.size(); ++i) {
            if (info.methods[i].name == method) {
                ClassMethodLookup result;
                result.class_index = current;
                result.method_index = static_cast<std::uint32_t>(i);
                result.function_index = info.methods[i].function_index;
                return result;
            }
        }
    }
    return std::nullopt;
}

Compiler::ClassMethodLookup Compiler::lookup_super_method(std::uint32_t class_index, const std::string& method,
                                                          const Location& loc) const {
    if (!class_context_) {
        fail("invalid use of 'super'", loc);
    }
    if (class_index >= bytecode_.classes.size()) {
        fail("invalid class type", loc);
    }
    const ClassInfo& info = bytecode_.classes[class_index];
    if (info.parent_index == kInvalidIndex) {
        fail("invalid use of 'super'", loc);
    }
    return lookup_class_method(class_index, method, loc, true);
}

bool Compiler::is_class_or_subclass(std::uint32_t class_index, std::uint32_t ancestor_index) const {
    for (std::uint32_t current = class_index; current != kInvalidIndex;
         current = bytecode_.classes[current].parent_index) {
        if (current == ancestor_index) {
            return true;
        }
    }
    return false;
}

void Compiler::build_class_layout(std::uint32_t class_index, const Location& loc) {
    ClassInfo& info = bytecode_.classes[class_index];
    info.layout.clear();
    if (info.parent_index != kInvalidIndex) {
        const ClassInfo& parent = bytecode_.classes[info.parent_index];
        info.layout = parent.layout;
    }
    for (const StructFieldInfo& field : info.fields) {
        for (const ClassFieldLayout& existing : info.layout) {
            if (existing.name == field.name) {
                fail("duplicate field '" + field.name + "'", loc);
            }
        }
        ClassFieldLayout layout;
        layout.name = field.name;
        layout.access = field.access;
        layout.declaring_class_index = class_index;
        info.layout.push_back(std::move(layout));
    }
}

void Compiler::rebuild_all_class_layouts() {
    const std::size_t count = bytecode_.classes.size();
    std::vector<std::uint32_t> depth(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint32_t d = 0;
        for (std::uint32_t parent = bytecode_.classes[i].parent_index; parent != kInvalidIndex;
             parent = bytecode_.classes[parent].parent_index) {
            ++d;
        }
        depth[i] = d;
    }
    std::vector<std::uint32_t> order(count);
    for (std::size_t i = 0; i < count; ++i) {
        order[i] = static_cast<std::uint32_t>(i);
    }
    std::sort(order.begin(), order.end(),
              [&](std::uint32_t a, std::uint32_t b) { return depth[a] < depth[b]; });
    for (const std::uint32_t class_index : order) {
        build_class_layout(class_index, Location{});
    }
}

void Compiler::validate_class_constructors(const std::vector<const ClassStmt*>& classes) {
    for (const ClassStmt* stmt : classes) {
        if (!stmt) {
            continue;
        }
        const Symbol* symbol = lookup_symbol(stmt->name);
        if (!symbol || symbol->kind != SymbolKind::Class) {
            continue;
        }
        const ClassInfo& info = bytecode_.classes[symbol->class_index];
        if (info.parent_index == kInvalidIndex) {
            continue;
        }
        const ClassInfo& parent = bytecode_.classes[info.parent_index];
        if (parent.constructor_index == kInvalidIndex) {
            continue;
        }
        const FunctionInfo& parent_ctor = bytecode_.functions[parent.constructor_index];
        const bool has_child_ctor = stmt->constructor != nullptr;
        const bool explicit_parent =
            has_child_ctor && constructor_calls_parent(stmt->constructor->body);
        if (parent_ctor.arity > 0) {
            if (!has_child_ctor) {
                fail("'" + stmt->name + "' must declare a constructor that initializes the parent class", stmt->loc);
            }
            if (!explicit_parent) {
                fail("missing required parent constructor call", stmt->loc);
            }
        }
    }
}

void Compiler::validate_class_cycles() const {
    for (std::size_t i = 0; i < bytecode_.classes.size(); ++i) {
        std::unordered_set<std::uint32_t> seen;
        for (std::uint32_t current = static_cast<std::uint32_t>(i); current != kInvalidIndex;
             current = bytecode_.classes[current].parent_index) {
            if (!seen.insert(current).second) {
                fail("circular inheritance", Location{});
            }
        }
    }
}

std::uint32_t Compiler::find_class_by_name(const std::string& name) const {
    for (std::size_t i = 0; i < bytecode_.classes.size(); ++i) {
        if (bytecode_.classes[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kInvalidIndex;
}

bool Compiler::constructor_calls_parent(const std::vector<std::unique_ptr<Stmt>>& body) const {
    for (const auto& stmt : body) {
        if (!stmt) {
            continue;
        }
        const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt.get());
        if (!expr_stmt || !expr_stmt->expression) {
            continue;
        }
        const auto* call = dynamic_cast<const CallExpr*>(expr_stmt->expression.get());
        if (!call) {
            continue;
        }
        if (call->name == "super" || call->name == "parent") {
            return true;
        }
    }
    return false;
}

std::optional<std::uint32_t> Compiler::infer_class_index(const Expr& expr) const {
    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(&expr)) {
        if (ident->name == "this" && class_context_) {
            return class_context_->class_index;
        }
        const TypeDescriptor desc = variable_type_descriptor(ident->name);
        if (desc.kind == StaticType::Class && desc.class_index) {
            return desc.class_index;
        }
        return std::nullopt;
    }
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expr)) {
        if (!field->object) {
            return std::nullopt;
        }
        return infer_class_index(*field->object);
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (const Symbol* symbol = lookup_symbol(call->name)) {
            if (symbol->kind == SymbolKind::Class) {
                return symbol->class_index;
            }
        }
    }
    if (const auto* call = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (call->object && call->method == "iter") {
            if (const std::optional<std::uint32_t> class_index = infer_class_index(*call->object)) {
                if (const std::optional<ClassMethodLookup> iter_method = find_class_method(*class_index, "iter")) {
                    const ClassMethodInfo& info =
                        bytecode_.classes[iter_method->class_index].methods[iter_method->method_index];
                    if (info.iterator_class_index != kInvalidIndex) {
                        return info.iterator_class_index;
                    }
                }
            }
        }
    }
    return std::nullopt;
}

void Compiler::track_class_type(const std::string& name, std::uint32_t class_index) {
    TypeDescriptor desc;
    desc.kind = StaticType::Class;
    desc.class_index = class_index;
    variable_types_[name].inferred = desc;
}

std::uint32_t Compiler::resolve_struct_index(const std::string& name, const Location& loc) const {
    for (std::size_t i = 0; i < bytecode_.structs.size(); ++i) {
        if (bytecode_.structs[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    fail("unknown struct type '" + name + "'", loc);
}

std::uint32_t Compiler::resolve_enum_index(const std::string& name, const Location& loc) const {
    for (std::size_t i = 0; i < bytecode_.enums.size(); ++i) {
        if (bytecode_.enums[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    fail("unknown enum type '" + name + "'", loc);
}

std::uint32_t Compiler::resolve_field_index(std::uint32_t struct_index, const std::string& field,
                                            const Location& loc) const {
    if (struct_index >= bytecode_.structs.size()) {
        fail("invalid struct type", loc);
    }
    const StructInfo& info = bytecode_.structs[struct_index];
    for (std::size_t i = 0; i < info.fields.size(); ++i) {
        if (info.fields[i].name == field) {
            return static_cast<std::uint32_t>(i);
        }
    }
    fail("unknown field '" + field + "'", loc);
}

void Compiler::check_field_access(std::uint32_t struct_index, const std::string& field,
                                  const Location& loc) const {
    if (struct_index >= bytecode_.structs.size()) {
        fail("invalid struct type", loc);
    }
    const StructInfo& info = bytecode_.structs[struct_index];
    for (const StructFieldInfo& field_info : info.fields) {
        if (field_info.name != field) {
            continue;
        }
        if (field_info.access == MemberAccess::Public) {
            return;
        }
        fail(std::string("cannot access ") + member_access_name(field_info.access) + " field '" + field + "'", loc);
    }
    fail("unknown field '" + field + "'", loc);
}

const char* Compiler::member_access_name(MemberAccess access) {
    switch (access) {
        case MemberAccess::Public:
            return "public";
        case MemberAccess::Protected:
            return "protected";
        case MemberAccess::Private:
            return "private";
    }
    return "unknown";
}

std::uint32_t Compiler::resolve_enum_member(std::uint32_t enum_index, const std::string& member,
                                            const Location& loc) const {
    if (enum_index >= bytecode_.enums.size()) {
        fail("invalid enum type", loc);
    }
    const EnumInfo& info = bytecode_.enums[enum_index];
    for (std::size_t i = 0; i < info.members.size(); ++i) {
        if (info.members[i] == member) {
            return static_cast<std::uint32_t>(i);
        }
    }
    fail("unknown enum member '" + member + "'", loc);
}

namespace {

std::optional<std::uint32_t> struct_index_from_descriptor(const TypeDescriptor& desc) {
    if (desc.kind == StaticType::Struct && desc.struct_index) {
        return desc.struct_index;
    }
    if (desc.kind == StaticType::List && !desc.type_args.empty()) {
        return struct_index_from_descriptor(desc.type_args[0]);
    }
    if (desc.kind == StaticType::Dict && desc.type_args.size() >= 2) {
        return struct_index_from_descriptor(desc.type_args[1]);
    }
    if (desc.kind == StaticType::Tuple && !desc.type_args.empty()) {
        for (const TypeDescriptor& element : desc.type_args) {
            if (const std::optional<std::uint32_t> index = struct_index_from_descriptor(element)) {
                return index;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<std::uint32_t> Compiler::infer_struct_index(const Expr& expr) const {
    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(&expr)) {
        const auto it = variable_types_.find(ident->name);
        if (it != variable_types_.end()) {
            if (it->second.declared) {
                if (const std::optional<std::uint32_t> index = struct_index_from_descriptor(*it->second.declared)) {
                    return index;
                }
            }
            if (it->second.inferred) {
                if (const std::optional<std::uint32_t> index = struct_index_from_descriptor(*it->second.inferred)) {
                    return index;
                }
            }
        }
        return std::nullopt;
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr)) {
        if (!index->object) {
            return std::nullopt;
        }
        if (const auto* ident = dynamic_cast<const IdentifierExpr*>(index->object.get())) {
            const auto it = variable_types_.find(ident->name);
            if (it != variable_types_.end()) {
                if (it->second.declared) {
                    if (const std::optional<std::uint32_t> struct_index =
                            struct_index_from_descriptor(*it->second.declared)) {
                        return struct_index;
                    }
                }
                if (it->second.inferred) {
                    if (const std::optional<std::uint32_t> struct_index =
                            struct_index_from_descriptor(*it->second.inferred)) {
                        return struct_index;
                    }
                }
            }
        }
        return infer_struct_index(*index->object);
    }
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expr)) {
        if (!field->object) {
            return std::nullopt;
        }
        const std::optional<std::uint32_t> outer = infer_struct_index(*field->object);
        if (!outer || *outer >= bytecode_.structs.size()) {
            return std::nullopt;
        }
        const StructInfo& info = bytecode_.structs[*outer];
        for (const StructFieldInfo& struct_field : info.fields) {
            if (struct_field.name == field->field && struct_field.type_name) {
                for (std::size_t i = 0; i < bytecode_.structs.size(); ++i) {
                    if (bytecode_.structs[i].name == *struct_field.type_name) {
                        return static_cast<std::uint32_t>(i);
                    }
                }
            }
        }
        return std::nullopt;
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (const Symbol* symbol = lookup_symbol(call->name)) {
            if (symbol->kind == SymbolKind::Struct) {
                return symbol->struct_index;
            }
        }
    }
    return std::nullopt;
}

void Compiler::track_struct_type(const std::string& name, std::uint32_t struct_index) {
    TypeDescriptor desc;
    desc.kind = StaticType::Struct;
    desc.struct_index = struct_index;
    variable_types_[name].inferred = desc;
}

void Compiler::track_enum_type(const std::string& name, std::uint32_t enum_index) {
    TypeDescriptor desc;
    desc.kind = StaticType::Enum;
    desc.enum_index = enum_index;
    variable_types_[name].inferred = desc;
}

void Compiler::declare_function(const FuncStmt& stmt) {
    const MemberAccess access = access_with_annotations(stmt.access, stmt.annotations);
    if (!in_function_) {
        check_module_level_access(access, stmt.loc);
    }
    FunctionInfo info;
    info.name = stmt.name;
    info.is_async = stmt.is_async;
    info.arity = static_cast<std::uint32_t>(stmt.parameters.size());
    for (const Parameter& param : stmt.parameters) {
        info.parameter_type_names.push_back(param.type_name);
        info.parameter_annotations.push_back(copy_annotations(param.annotations));
    }
    info.return_type_name = stmt.return_type_name;
    info.annotations = copy_annotations(stmt.annotations);
    for (const GenericTypeParam& param : stmt.type_params) {
        info.generic_type_params.push_back(param.name);
        if (param.constraint_type_name) {
            info.generic_type_param_constraints.push_back(*param.constraint_type_name);
        } else {
            info.generic_type_param_constraints.push_back(std::string());
        }
    }
    std::uint32_t min_arity = 0;
    bool variadic = false;
    for (const Parameter& param : stmt.parameters) {
        if (param.is_variadic) {
            variadic = true;
            break;
        }
        if (!param.default_value) {
            ++min_arity;
        }
    }
    info.min_arity = min_arity;
    info.variadic = variadic;
    const auto index = static_cast<std::uint32_t>(bytecode_.functions.size());
    bytecode_.functions.push_back(std::move(info));
    function_parameters_[index] = &stmt.parameters;

    Symbol* existing = nullptr;
    if (current_scope_defines(stmt.name)) {
        existing = lookup_symbol(stmt.name);
        if (!existing || existing->kind != SymbolKind::Function) {
            fail("duplicate declaration of '" + stmt.name + "'", stmt.loc);
        }
        const FunctionInfo& prior = bytecode_.functions[existing->function_index];
        const std::vector<std::uint32_t> prior_indices =
            existing->function_overloads.empty()
                ? std::vector<std::uint32_t>{existing->function_index}
                : existing->function_overloads;
        for (const std::uint32_t overload_index : prior_indices) {
            if (function_signatures_conflict(bytecode_.functions[overload_index], bytecode_.functions[index])) {
                fail("duplicate function signature for '" + stmt.name + "'", stmt.loc);
            }
        }
        if (existing->function_overloads.empty()) {
            existing->function_overloads.push_back(existing->function_index);
        }
        existing->function_overloads.push_back(index);
    }

    Symbol symbol;
    symbol.name = stmt.name;
    symbol.kind = SymbolKind::Function;
    symbol.access = access;
    symbol.function_index = existing ? existing->function_index : index;
    if (existing) {
        symbol.function_overloads = existing->function_overloads;
    }
    symbol.owner_function = current_function();
    if (in_function_) {
        ScopeBinding binding;
        binding.name = stmt.name;
        const auto it = variable_index_.find(stmt.name);
        if (it != variable_index_.end()) {
            binding.existed = true;
            binding.previous_index = it->second;
            binding.previous_local = variable_is_local(stmt.name);
            const auto type_it = variable_types_.find(stmt.name);
            if (type_it != variable_types_.end()) {
                binding.previous_type = type_it->second;
            }
        }
        symbol.local = true;
        symbol.slot = next_local_++;
        variable_index_[stmt.name] = symbol.slot;
        variable_local_[stmt.name] = true;
        scope_stack_.back().bindings.push_back(std::move(binding));
    }
    if (existing) {
        scope_stack_.back().symbols[stmt.name] = std::move(symbol);
    } else {
        scope_stack_.back().symbols.emplace(stmt.name, std::move(symbol));
    }
    function_decl_index_[&stmt] = index;
}

void Compiler::compile_module_async_entry(std::uint32_t function_index,
                                          const std::vector<const Stmt*>& statements) {
    if (function_index >= bytecode_.functions.size()) {
        fail("invalid module async entry", Location{1, 1});
    }
    const bool saved_in_function = in_function_;
    const bool saved_emitting = emitting_body_;
    const int saved_async_depth = async_function_depth_;
    const std::uint32_t saved_registers = next_register_;
    const std::uint32_t saved_locals = next_local_;
    in_function_ = true;
    emitting_body_ = false;
    next_register_ = 0;
    next_local_ = 0;
    function_stack_.push_back(function_index);
    ++async_function_depth_;
    bytecode_.functions[function_index].returns_value = false;
    push_scope();
    bytecode_.functions[function_index].entry = current_ip();
    emitting_body_ = true;
    for (const Stmt* stmt : statements) {
        if (!stmt) {
            continue;
        }
        compile_stmt(*stmt);
    }
    emit_return_null();
    bytecode_.functions[function_index].local_count = next_local_;
    bytecode_.functions[function_index].register_count = next_register_;
    note_registers();
    pop_scope();
    function_stack_.pop_back();
    --async_function_depth_;
    in_function_ = saved_in_function;
    emitting_body_ = saved_emitting;
    async_function_depth_ = saved_async_depth;
    next_register_ = saved_registers;
    next_local_ = saved_locals;
}

void Compiler::compile_function(const FuncStmt& stmt) {
    const Symbol* symbol = lookup_symbol(stmt.name);
    if (!symbol || symbol->kind != SymbolKind::Function) {
        fail("undefined function '" + stmt.name + "'", stmt.loc);
    }
    const auto decl_it = function_decl_index_.find(&stmt);
    const std::uint32_t function_index =
        decl_it != function_decl_index_.end() ? decl_it->second : symbol->function_index;

    const bool saved_in_function = in_function_;
    const bool saved_emitting = emitting_body_;
    const int saved_async_depth = async_function_depth_;
    const std::uint32_t saved_registers = next_register_;
    const std::uint32_t saved_locals = next_local_;
    in_function_ = true;
    emitting_body_ = false;
    next_register_ = 0;
    next_local_ = 0;
    function_stack_.push_back(function_index);
    if (stmt.is_async) {
        ++async_function_depth_;
        bytecode_.functions[function_index].returns_value = true;
    }
    bytecode_.functions[function_index].parameter_type_names.clear();
    bytecode_.functions[function_index].return_type_name = stmt.return_type_name;
    for (const Parameter& param : stmt.parameters) {
        bytecode_.functions[function_index].parameter_type_names.push_back(param.type_name);
    }
    function_parameters_[function_index] = &stmt.parameters;
    std::vector<std::string> generic_names;
    for (const GenericTypeParam& param : stmt.type_params) {
        generic_names.push_back(param.name);
    }
    push_type_param_scope(generic_names);
    push_scope();
    for (const Parameter& param : stmt.parameters) {
        if (current_scope_defines(param.name)) {
            fail("duplicate declaration of '" + param.name + "'", param.loc);
        }
        const std::uint32_t slot = introduce_variable(param.name, param.type_name, param.loc, true);
        record_debug_local(param.name, slot, DebugLocalKind::Parameter, param.loc);
        mark_initialized(param.name);
    }
    const std::vector<NestedFunction> nested = hoist_functions(stmt.body);
    predeclare_statements(stmt.body);
    compile_nested_functions(nested, false);
    bytecode_.functions[function_index].entry = current_ip();
    emitting_body_ = true;
    emit_closures(nested);
    for (const Parameter& param : stmt.parameters) {
        if (!param.pattern) {
            continue;
        }
        const Symbol* arg_symbol = lookup_symbol(param.name);
        if (!arg_symbol) {
            fail("invalid destructuring parameter", param.loc);
        }
        const Register src = alloc_register();
        if (arg_symbol->local) {
            emit(Instruction{Opcode::LoadLocal, src, arg_symbol->slot, 0});
        } else {
            emit(Instruction{Opcode::LoadVar, src, arg_symbol->slot, 0});
        }
        compile_parameter_destructure(*param.pattern, src, param.loc);
    }
    compile_function_body(stmt.body, stmt.loc);
    bytecode_.functions[function_index].local_count = next_local_;
    bytecode_.functions[function_index].register_count = next_register_;
    note_registers();
    pop_scope();
    pop_type_param_scope();
    function_stack_.pop_back();
    if (stmt.is_async) {
        --async_function_depth_;
    }
    in_function_ = saved_in_function;
    emitting_body_ = saved_emitting;
    async_function_depth_ = saved_async_depth;
    next_register_ = saved_registers;
    next_local_ = saved_locals;
}

void Compiler::compile_function_body(const std::vector<std::unique_ptr<Stmt>>& statements, const Location& loc) {
    const Stmt* last = last_executable_stmt(statements);
    if (!last) {
        if (current_function() != kInvalidIndex) {
            const FunctionInfo& fn = bytecode_.functions[current_function()];
            if (fn.return_type_name && !fn.return_type_name->empty()) {
                fail("function requires a return value", loc);
            }
            bytecode_.functions[current_function()].returns_value = false;
        }
        emit_return_null();
        return;
    }
    for (const auto& stmt : statements) {
        if (!stmt || dynamic_cast<const FuncStmt*>(stmt.get()) || stmt.get() == last) {
            continue;
        }
        compile_stmt(*stmt);
    }
    if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(last)) {
        if (!expr_stmt->expression) {
            fail("invalid statement", expr_stmt->loc);
        }
        reject_bare_delay_call(*expr_stmt->expression);
        const auto function_returns = [&](const std::string& name) -> bool {
            const Symbol* symbol = lookup_symbol(name);
            if (!symbol || symbol->kind != SymbolKind::Function) {
                return true;
            }
            return bytecode_.functions[symbol->function_index].returns_value;
        };
        const bool returns_value = expr_returns_value(*expr_stmt->expression, function_returns);
        if (current_function() != kInvalidIndex) {
            bytecode_.functions[current_function()].returns_value = returns_value;
        }
        if (!returns_value) {
            compile_stmt(*last);
            emit_return_null();
            return;
        }
        const Register src = alloc_register();
        compile_expr_into(*expr_stmt->expression, src);
        if (current_function() != kInvalidIndex) {
            const FunctionInfo& fn = bytecode_.functions[current_function()];
            check_expression_matches_return(*expr_stmt->expression, fn, expr_stmt->loc);
            if (fn.return_type_name && !fn.return_type_name->empty()) {
                const TypeDescriptor expected = function_body_return_type(fn, expr_stmt->loc);
                const TypeDescriptor actual = infer_type_descriptor(*expr_stmt->expression);
                apply_assign_conversions(src, expected, actual, expr_stmt->loc);
            }
        }
        emit_return_value(src);
        return;
    }
    if (const auto* return_stmt = dynamic_cast<const ReturnStmt*>(last)) {
        if (current_function() != kInvalidIndex) {
            bytecode_.functions[current_function()].returns_value = return_stmt->value != nullptr;
        }
        compile_stmt(*last);
        return;
    }
    if (current_function() != kInvalidIndex) {
        const FunctionInfo& fn = bytecode_.functions[current_function()];
        if (fn.return_type_name && !fn.return_type_name->empty()) {
            fail("function requires a return value", loc);
        }
        bytecode_.functions[current_function()].returns_value = false;
    }
    compile_stmt(*last);
    emit_return_null();
}

std::vector<Compiler::NestedFunction> Compiler::hoist_functions(const std::vector<std::unique_ptr<Stmt>>& statements) {
    std::vector<NestedFunction> nested;
    for (const auto& stmt : statements) {
        const auto* fn = dynamic_cast<const FuncStmt*>(stmt.get());
        if (!fn) {
            continue;
        }
        declare_function(*fn);
        const Symbol* symbol = lookup_symbol(fn->name);
        NestedFunction item;
        item.stmt = fn;
        item.index = symbol->function_index;
        item.slot = symbol->slot;
        nested.push_back(item);
    }
    return nested;
}

void Compiler::predeclare_statements(const std::vector<std::unique_ptr<Stmt>>& statements) {
    for (const auto& stmt : statements) {
        if (stmt && !dynamic_cast<const FuncStmt*>(stmt.get())) {
            predeclare_stmt(*stmt);
        }
    }
}

void Compiler::predeclare_stmt(const Stmt& stmt) {
    if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
        if (assign->annotation != TypeAnnotationKind::None) {
            if (current_scope_defines(assign->name)) {
                fail("duplicate declaration of '" + assign->name + "'", assign->loc);
            }
            declare_variable(assign->name, assign->type_name, assign->loc);
            return;
        }
        resolve_assign(assign->name, assign->loc);
        return;
    }
    if (const auto* destructure = dynamic_cast<const DestructureAssignStmt*>(&stmt)) {
        if (!destructure->pattern) {
            fail("invalid destructuring assignment", destructure->loc);
        }
        std::vector<std::string> names;
        collect_pattern_names(*destructure->pattern, names);
        for (const std::string& name : names) {
            resolve_assign(name, destructure->loc);
        }
        return;
    }
    if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(&stmt)) {
        if (const auto* assign = dynamic_cast<const AssignExpr*>(expr_stmt->expression.get())) {
            if (assign->op == TokenKind::Equal) {
                if (const auto* ident = dynamic_cast<const IdentifierExpr*>(assign->target.get())) {
                    resolve_assign(ident->name, ident->loc);
                }
            }
        }
    }
}

void Compiler::compile_nested_functions(const std::vector<NestedFunction>& nested, bool jump_over) {
    if (nested.empty()) {
        return;
    }
    std::optional<std::uint32_t> skip;
    if (jump_over) {
        skip = emit_jump();
    }
    for (const NestedFunction& item : nested) {
        compile_function(*item.stmt);
    }
    if (skip) {
        patch_jump(*skip, current_ip());
    }
}

void Compiler::emit_closures(const std::vector<NestedFunction>& nested) {
    for (const NestedFunction& item : nested) {
        const Register dest = alloc_register();
        emit(Instruction{Opcode::MakeClosure, dest, item.index, 0});
        emit(Instruction{Opcode::StoreLocal, item.slot, dest, 0});
    }
}

void Compiler::compile_direct_call(Register dest, const CallExpr& call, const Symbol& symbol) {
    const std::string& resolved_name = symbol.name.empty() ? call.name : symbol.name;
    std::vector<TypeDescriptor> arg_types;
    for (const auto& argument : call.arguments) {
        if (!argument) {
            fail("invalid expression", call.loc);
        }
        arg_types.push_back(infer_type_descriptor(*argument));
    }
    const std::vector<std::uint32_t> candidates = function_candidates(symbol);
    const Bytecode* fn_bytecode = nullptr;
    if (symbol.import_module_index != 0xFFFFFFFFu) {
        fn_bytecode = &module_bytecode_for(symbol.import_module_index, call.loc);
    }
    const std::optional<OverloadCandidate> chosen = pick_best_overload(
        candidates, function_parameters_, arg_types, call.type_arguments, bytecode_, active_type_params(), true,
        static_cast<std::uint32_t>(call.arguments.size()), fn_bytecode);
    std::optional<OverloadCandidate> resolved = chosen;
    if (!resolved) {
        if (candidates.size() > 1) {
            fail("ambiguous overload for '" + resolved_name + "'", call.loc);
        }
        const Bytecode& sole_bc = fn_bytecode ? *fn_bytecode : bytecode_;
        const FunctionInfo& sole = sole_bc.functions[candidates.front()];
        const std::uint32_t argc = static_cast<std::uint32_t>(call.arguments.size());
        if (sole.variadic) {
            if (argc < sole.min_arity) {
                fail("'" + resolved_name + "' expects at least " + std::to_string(sole.min_arity) + " arguments",
                     call.loc);
            }
            resolved = OverloadCandidate{candidates.front(), 0, {}};
        } else if (argc >= sole.min_arity && argc <= sole.arity) {
            resolved = OverloadCandidate{candidates.front(), 0, {}};
        } else {
            fail("'" + resolved_name + "' expects " + std::to_string(sole.arity) + " arguments", call.loc);
        }
    }
    const OverloadCandidate& match = *resolved;
    const std::uint32_t function_index = match.function_index;
    const FunctionInfo& fn = function_info_for_symbol(
        [&]() {
            Symbol resolved = symbol;
            resolved.function_index = function_index;
            return resolved;
        }(),
        call.loc);
    warn_deprecated_use(fn.annotations, call.loc);
    if (!fn.generic_type_params.empty() &&
        !validate_generic_constraints(fn.generic_type_params, fn.generic_type_param_constraints, match.generic_subst,
                                      [this, &call](const TypeDescriptor& type, const std::string& constraint) {
                                          return type_satisfies_constraint(type, constraint, call.loc);
                                      })) {
        fail("type argument does not satisfy generic constraint for '" + resolved_name + "'", call.loc);
    }
    const std::vector<Parameter>& params =
        parameters_for_function(function_index, symbol.import_module_index, call.loc);
    const std::unordered_map<std::string, TypeDescriptor> subst = match.generic_subst;

    const bool imported_callee = symbol.import_module_index != 0xFFFFFFFFu && !symbol.local;
    std::optional<Register> import_callee;
    if (imported_callee) {
        import_callee = alloc_register();
    }

    Register base = 0;
    Register first = 0;
    if (symbol.local) {
        base = alloc_register();
        first = next_register_;
    } else if (import_callee) {
        first = next_register_;
    } else {
        first = next_register_;
    }
    for (std::uint32_t i = 0; i < fn.arity; ++i) {
        alloc_register();
    }
    std::size_t arg_index = 0;
    std::size_t out_index = 0;
    if (params.empty() && !call.arguments.empty()) {
        for (std::size_t i = 0; i < call.arguments.size(); ++i) {
            if (!call.arguments[i]) {
                fail("invalid expression", call.loc);
            }
            compile_expr_into(*call.arguments[i], first + static_cast<Register>(i));
        }
    }
    for (std::size_t param_index = 0; param_index < params.size(); ++param_index) {
        const Parameter& param = params[param_index];
        const Register out_reg = first + static_cast<Register>(out_index);
        if (param.is_variadic) {
            const std::size_t rest = call.arguments.size() - arg_index;
            if (rest == 0) {
                emit(Instruction{Opcode::MakeList, out_reg, 0, 0});
            } else {
                const Register pack_first = next_register_;
                for (std::size_t i = 0; i < rest; ++i) {
                    alloc_register();
                }
                for (std::size_t i = 0; i < rest; ++i) {
                    compile_expr_into(*call.arguments[arg_index + i], pack_first + static_cast<Register>(i));
                }
                emit(Instruction{Opcode::MakeList, out_reg, pack_first, static_cast<std::uint32_t>(rest)});
            }
            ++out_index;
            break;
        }
        if (arg_index < call.arguments.size()) {
            compile_expr_into(*call.arguments[arg_index], out_reg);
            if (param.type_name && !param.type_name->empty()) {
                TypeDescriptor target = parse_type_spec(*param.type_name, call.loc, bytecode_, &fn.generic_type_params);
                TypeDescriptor concrete;
                if (!substitute_type_descriptor(concrete, target, subst)) {
                    concrete = target;
                }
                const TypeDescriptor source = infer_type_descriptor(*call.arguments[arg_index]);
                check_assignable(concrete, source, call.arguments[arg_index]->loc);
                apply_call_argument_conversions(out_reg, concrete, source, call.arguments[arg_index]->loc);
            }
            ++arg_index;
        } else if (param.default_value) {
            compile_expr_into(*param.default_value, out_reg);
        } else {
            fail("'" + resolved_name + "' missing argument for '" + param.name + "'", call.loc);
        }
        ++out_index;
    }
    if (symbol.local) {
        emit_load_name(base, resolved_name, call.loc);
        emit(Instruction{Opcode::CallValue, dest, base, fn.arity});
        return;
    }
    if (import_callee) {
        Constant name_constant;
        name_constant.kind = ConstantKind::String;
        const std::string& export_name =
            symbol.import_export_name.empty() ? resolved_name : symbol.import_export_name;
        name_constant.lexeme = encode_string_lexeme(export_name);
        const std::uint32_t name_index = intern_constant(std::move(name_constant));
        emit(Instruction{Opcode::ImportName, *import_callee, symbol.import_module_index, name_index});
        emit(Instruction{Opcode::CallValue, dest, *import_callee, fn.arity});
        return;
    }
    emit(Instruction{Opcode::Call, dest, first, function_index});
}

void Compiler::compile_call_value(Register dest, const CallExpr& call) {
    const Register callee = next_register_;
    alloc_register();
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
        alloc_register();
    }
    emit_load_name(callee, call.name, call.loc);
    for (std::size_t i = 0; i < call.arguments.size(); ++i) {
        if (!call.arguments[i]) {
            fail("invalid expression", call.loc);
        }
        compile_expr_into(*call.arguments[i], callee + 1 + static_cast<Register>(i));
    }
    emit(Instruction{Opcode::CallValue, dest, callee, static_cast<std::uint32_t>(call.arguments.size())});
}

std::uint32_t Compiler::current_function() const {
    if (function_stack_.empty()) {
        return 0xFFFFFFFFu;
    }
    return function_stack_.back();
}

std::uint32_t Compiler::intern_capture(const Symbol& symbol, const Location& loc) {
    return intern_capture_in(current_function(), symbol, loc);
}

std::uint32_t Compiler::intern_capture_in(std::uint32_t function_index, const Symbol& symbol, const Location& loc) {
    if (function_index == 0xFFFFFFFFu) {
        fail("invalid closure access", loc);
    }
    FunctionInfo& fn = bytecode_.functions[function_index];
    const bool want_local = symbol.owner_function == function_index;
    if (want_local) {
        fail("invalid closure access", loc);
    }

    std::uint32_t parent = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < function_stack_.size(); ++i) {
        if (function_stack_[i] == function_index && i > 0) {
            parent = function_stack_[i - 1];
            break;
        }
    }
    if (parent == 0xFFFFFFFFu) {
        fail("invalid closure access", loc);
    }

    const bool from_local = symbol.owner_function == parent;
    std::uint32_t index = symbol.slot;
    if (!from_local) {
        index = intern_capture_in(parent, symbol, loc);
    }

    for (std::size_t i = 0; i < fn.captures.size(); ++i) {
        if (fn.captures[i].from_local == from_local && fn.captures[i].index == index) {
            return static_cast<std::uint32_t>(i);
        }
    }
    Capture capture;
    capture.from_local = from_local;
    capture.index = index;
    fn.captures.push_back(capture);
    const std::uint32_t upvalue_index = static_cast<std::uint32_t>(fn.captures.size() - 1);
    FunctionDebugInfo& debug_info = current_function_debug();
    if (debug_info.upvalue_names.size() <= upvalue_index) {
        debug_info.upvalue_names.resize(upvalue_index + 1);
    }
    debug_info.upvalue_names[upvalue_index] = symbol.name;
    return upvalue_index;
}

void Compiler::emit_return_value(Register src) {
    emit(Instruction{Opcode::Return, src, 0, 0});
}

void Compiler::emit_return_null() {
    const Register result = alloc_register();
    emit_load_null(result);
    emit_return_value(result);
}

void Compiler::compile_return(const ReturnStmt& stmt) {
    if (!in_function_) {
        fail("return outside function", stmt.loc);
    }
    const FunctionInfo& fn = bytecode_.functions[current_function()];
    if (class_context_ && class_context_->in_constructor && stmt.value) {
        fail("constructor cannot return a value", stmt.loc);
    }
    const Register src = alloc_register();
    if (stmt.value) {
        compile_expr_into(*stmt.value, src);
        check_expression_matches_return(*stmt.value, fn, stmt.loc);
        if (fn.return_type_name && !fn.return_type_name->empty()) {
            const TypeDescriptor expected = function_body_return_type(fn, stmt.loc);
            const TypeDescriptor actual = infer_type_descriptor(*stmt.value);
            apply_assign_conversions(src, expected, actual, stmt.loc);
        }
    } else {
        if (fn.return_type_name && !fn.return_type_name->empty()) {
            fail("function requires a return value", stmt.loc);
        }
        emit_load_null(src);
    }
    if (!try_stack_.empty()) {
        const TryContext& context = try_stack_.back();
        if (context.has_finally) {
            emit(Instruction{Opcode::DeferReturn, context.handler_index, src, 0});
            return;
        }
        emit(Instruction{Opcode::LeaveTry, context.handler_index, 0, 0});
    }
    emit_return_value(src);
}

void Compiler::validate_catch_clauses(const TryStmt& stmt) const {
    for (std::size_t i = 0; i < stmt.catches.size(); ++i) {
        const CatchClause& clause = stmt.catches[i];
        if (!clause.type_name) {
            continue;
        }
        const std::uint32_t catch_index = find_class_by_name(*clause.type_name);
        if (catch_index == kInvalidIndex) {
            fail("unknown catch type '" + *clause.type_name + "'", clause.loc);
        }
        for (std::size_t j = 0; j < i; ++j) {
            const CatchClause& prior = stmt.catches[j];
            if (!prior.type_name) {
                continue;
            }
            const std::uint32_t prior_index = find_class_by_name(*prior.type_name);
            if (prior_index != kInvalidIndex && is_class_or_subclass(catch_index, prior_index)) {
                fail("unreachable catch clause for '" + *clause.type_name + "'", clause.loc);
            }
        }
    }
}

void Compiler::compile_try(const TryStmt& stmt) {
    const bool has_catch = !stmt.catches.empty();
    const bool has_finally = stmt.finally_body.has_value();
    validate_catch_clauses(stmt);

    HandlerInfo handler;
    const std::uint32_t handler_index = static_cast<std::uint32_t>(bytecode_.handlers.size());
    bytecode_.handlers.push_back(handler);

    try_stack_.push_back(TryContext{handler_index, has_finally});
    emit(Instruction{Opcode::EnterTry, handler_index, 0, 0});
    compile_scoped_block(stmt.try_body, stmt.loc);
    emit(Instruction{Opcode::LeaveTry, handler_index, 0, 0});

    std::uint32_t skip_catch = 0;
    if (has_catch && !has_finally) {
        skip_catch = emit_jump();
    }

    std::vector<std::uint32_t> catch_end_jumps;
    for (const CatchClause& clause : stmt.catches) {
        CatchEntry entry;
        entry.catch_ip = current_ip();
        push_scope();
        if (clause.type_name) {
            entry.catch_slot = declare_variable(clause.binding, clause.type_name, clause.loc);
        } else {
            entry.catch_slot = intern_binding(clause.binding, clause.loc);
        }
        entry.catch_is_local = in_function_;
        if (clause.type_name) {
            entry.catch_class_index = find_class_by_name(*clause.type_name);
            if (entry.catch_class_index == kInvalidIndex) {
                fail("unknown catch type '" + *clause.type_name + "'", clause.loc);
            }
        }
        mark_initialized(clause.binding);
        const std::vector<NestedFunction> nested = hoist_functions(clause.body);
        predeclare_statements(clause.body);
        compile_nested_functions(nested, true);
        emit_closures(nested);
        compile_block(clause.body, clause.loc);
        emit(Instruction{Opcode::LeaveTry, handler_index, 0, 0});
        if (!has_finally) {
            catch_end_jumps.push_back(emit_jump());
        }
        pop_scope();
        bytecode_.handlers[handler_index].catches.push_back(entry);
    }

    if (has_finally) {
        try_stack_.pop_back();
        bytecode_.handlers[handler_index].finally_ip = current_ip();
        compile_scoped_block(*stmt.finally_body, stmt.loc);
        emit(Instruction{Opcode::EndFinally, 0, 0, 0});
    } else {
        try_stack_.pop_back();
    }

    const std::uint32_t end_ip = current_ip();
    bytecode_.handlers[handler_index].end_ip = end_ip;
    if (has_catch && !has_finally) {
        patch_jump(skip_catch, end_ip);
    }
    for (const std::uint32_t jump_index : catch_end_jumps) {
        patch_jump(jump_index, end_ip);
    }
}

void Compiler::compile_throw(const ThrowStmt& stmt) {
    if (!stmt.value) {
        fail("invalid throw statement", stmt.loc);
    }
    const Register value = compile_expr(*stmt.value);
    emit(Instruction{Opcode::Throw, value, 0, 0});
}

void Compiler::compile_propagate_expr_into(const PropagateExpr& expr, Register dest) {
    if (!in_function_) {
        fail("postfix ? outside function", expr.loc);
    }
    if (!expr.operand) {
        fail("invalid expression", expr.loc);
    }
    const std::uint32_t result_enum = resolve_enum_index("Result", expr.loc);
    const std::uint32_t err_member = resolve_enum_member(result_enum, "Err", expr.loc);
    const Register value = compile_expr(*expr.operand);
    const Register is_err = alloc_register();
    const std::uint32_t packed = (err_member << 16) | (result_enum & 0xFFFFu);
    emit(Instruction{Opcode::TestEnumMember, is_err, value, packed});
    const std::uint32_t ok_jump = emit_jump_if_false(is_err);
    emit(Instruction{Opcode::Return, value, 0, 0});
    patch_jump(ok_jump, current_ip());
    emit(Instruction{Opcode::GetEnumPayload, dest, value, 0});
}

void Compiler::compile_enum_variant_call(Register dest, const Symbol& symbol, const std::string& member,
                                         const std::vector<std::unique_ptr<Expr>>& arguments, const Location& loc) {
    const std::uint32_t member_index = resolve_enum_member(symbol.enum_index, member, loc);
    const EnumInfo& info = bytecode_.enums[symbol.enum_index];
    const std::vector<std::string> empty_payload;
    const std::vector<std::string>& payload_fields =
        member_index < info.member_payloads.size() ? info.member_payloads[member_index] : empty_payload;
    if (payload_fields.empty()) {
        if (!arguments.empty()) {
            fail("'" + member + "' expects 0 arguments", loc);
        }
        emit(Instruction{Opcode::LoadEnum, dest, symbol.enum_index, member_index});
        return;
    }
    if (arguments.size() != payload_fields.size()) {
        fail("'" + member + "' expects " + std::to_string(payload_fields.size()) + " arguments", loc);
    }
    Register payload = 0;
    if (payload_fields.size() == 1) {
        payload = compile_expr(*arguments[0]);
    } else {
        payload = alloc_register();
        const Register first = next_register_;
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            alloc_register();
            compile_expr_into(*arguments[i], first + static_cast<Register>(i));
        }
        emit(Instruction{Opcode::MakeTuple, payload, first, static_cast<std::uint32_t>(arguments.size())});
    }
    const std::uint32_t packed = (member_index << 16) | (symbol.enum_index & 0xFFFFu);
    emit(Instruction{Opcode::MakeEnumEx, dest, packed, payload});
}

void Compiler::compile_defer(const DeferStmt& stmt) {
    if (!in_function_) {
        fail("defer outside function", stmt.loc);
    }
    if (!stmt.expression) {
        fail("invalid defer statement", stmt.loc);
    }
    const std::uint32_t function_index = static_cast<std::uint32_t>(bytecode_.functions.size());
    FunctionInfo fn;
    fn.name = "<defer>";
    fn.arity = 0;
    fn.returns_value = false;
    bytecode_.functions.push_back(fn);

    const std::uint32_t skip = emit_jump();
    const std::uint32_t entry = current_ip();
    bytecode_.functions[function_index].entry = entry;

    const bool saved_in_function = in_function_;
    const bool saved_emitting = emitting_body_;
    const std::uint32_t saved_registers = next_register_;
    const std::uint32_t saved_locals = next_local_;
    in_function_ = true;
    emitting_body_ = true;
    next_register_ = 0;
    next_local_ = 0;
    function_stack_.push_back(function_index);
    push_scope();

    compile_expr(*stmt.expression);
    emit_return_null();

    pop_scope();
    function_stack_.pop_back();
    bytecode_.functions[function_index].local_count = next_local_;
    bytecode_.functions[function_index].register_count = next_register_;
    note_registers();
    in_function_ = saved_in_function;
    emitting_body_ = saved_emitting;
    next_register_ = saved_registers;
    next_local_ = saved_locals;

    patch_jump(skip, current_ip());

    const Register closure = alloc_register();
    emit(Instruction{Opcode::MakeClosure, closure, function_index, 0});
    emit(Instruction{Opcode::PushDefer, closure, 0, 0});
}


std::uint32_t Compiler::current_ip() const {
    return static_cast<std::uint32_t>(bytecode_.instructions.size());
}

std::uint32_t Compiler::emit_jump() {
    const std::uint32_t index = current_ip();
    emit(Instruction{Opcode::Jump, 0, 0, 0});
    return index;
}

std::uint32_t Compiler::emit_jump_if_false(Register condition) {
    const std::uint32_t index = current_ip();
    emit(Instruction{Opcode::JumpIfFalse, condition, 0, 0});
    return index;
}

std::uint32_t Compiler::emit_jump_if_true(Register condition) {
    const std::uint32_t index = current_ip();
    emit(Instruction{Opcode::JumpIfTrue, condition, 0, 0});
    return index;
}

void Compiler::patch_jump(std::uint32_t jump_index, std::uint32_t target) {
    if (jump_index >= bytecode_.instructions.size()) {
        fail("invalid bytecode", Location{});
    }
    Instruction& instruction = bytecode_.instructions[jump_index];
    if (instruction.opcode == Opcode::Jump) {
        instruction.a = target;
        return;
    }
    if (instruction.opcode == Opcode::JumpIfFalse || instruction.opcode == Opcode::JumpIfTrue) {
        instruction.b = target;
        return;
    }
    fail("invalid bytecode", Location{});
}

StaticType Compiler::infer_expr(const Expr& expr) const {
    if (const auto* literal = dynamic_cast<const LiteralExpr*>(&expr)) {
        return infer_literal(*literal);
    }
    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(&expr)) {
        return variable_static_type(ident->name);
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(&expr)) {
        return infer_unary(*unary);
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
        return infer_binary(*binary);
    }
    if (dynamic_cast<const ListExpr*>(&expr)) {
        return StaticType::List;
    }
    if (dynamic_cast<const DictExpr*>(&expr)) {
        return StaticType::Dict;
    }
    if (dynamic_cast<const TupleExpr*>(&expr)) {
        return StaticType::Tuple;
    }
    if (dynamic_cast<const SetExpr*>(&expr)) {
        return StaticType::Set;
    }
    if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expr)) {
        if (field->object) {
            if (const auto* ident = dynamic_cast<const IdentifierExpr*>(field->object.get())) {
                if (const Symbol* symbol = lookup_symbol(ident->name)) {
                    if (symbol->kind == SymbolKind::Enum) {
                        return StaticType::Enum;
                    }
                }
            }
            if (infer_class_index(*field->object)) {
                return StaticType::Class;
            }
            if (infer_struct_index(*field->object)) {
                return StaticType::Struct;
            }
        }
        return StaticType::Unknown;
    }
    if (dynamic_cast<const IndexExpr*>(&expr)) {
        return StaticType::Unknown;
    }
    if (const auto* call = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (call->method == "is_empty" || call->method == "contains" || call->method == "contains_key" ||
            call->method == "starts_with" || call->method == "ends_with") {
            return StaticType::Bool;
        }
        if (call->method == "length" || call->method == "find" || call->method == "count") {
            return StaticType::Integer;
        }
        if (call->method == "keys" || call->method == "values") {
            return StaticType::List;
        }
        if (call->method == "union" || call->method == "intersection" || call->method == "difference") {
            return StaticType::Set;
        }
        if (call->method == "replace") {
            return StaticType::String;
        }
        if (call->method == "trim" || call->method == "lower" || call->method == "upper") {
            return StaticType::String;
        }
        if (call->method == "split") {
            return StaticType::List;
        }
        return StaticType::Unknown;
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (call->name == "int") {
            return StaticType::Integer;
        }
        if (call->name == "float") {
            return StaticType::Float;
        }
        if (call->name == "str" || call->name == "input") {
            return StaticType::String;
        }
        if (call->name == "bool") {
            return StaticType::Bool;
        }
        if (call->name == "type") {
            return StaticType::String;
        }
        if (call->name == "delay" || call->name == "race" || call->name == "timeout" ||
            call->name == "async_input") {
            return StaticType::Task;
        }
        if (call->name == "abs" || call->name == "round" || call->name == "min" || call->name == "max" ||
            call->name == "clamp") {
            return StaticType::Unknown;
        }
        if (const Symbol* symbol = lookup_symbol(call->name)) {
            if (symbol->kind == SymbolKind::Function) {
                const FunctionInfo& fn = bytecode_.functions[symbol->function_index];
                if (fn.is_async) {
                    return StaticType::Task;
                }
                if (fn.return_type_name && !fn.return_type_name->empty()) {
                    return function_body_return_type(fn, call->loc).kind;
                }
            }
            if (symbol->kind == SymbolKind::Struct) {
                return StaticType::Struct;
            }
            if (symbol->kind == SymbolKind::Class) {
                return StaticType::Class;
            }
        }
        return StaticType::Unknown;
    }
    if (const auto* await_expr = dynamic_cast<const AwaitExpr*>(&expr)) {
        if (await_expr->operand) {
            const TypeDescriptor desc = infer_type_descriptor(*await_expr->operand);
            if (desc.kind == StaticType::Task && !desc.type_args.empty()) {
                return desc.type_args[0].kind;
            }
        }
        return StaticType::Unknown;
    }
    if (dynamic_cast<const MembershipExpr*>(&expr)) {
        return StaticType::Bool;
    }
    if (const auto* ternary = dynamic_cast<const TernaryExpr*>(&expr)) {
        if (!ternary->then_expr || !ternary->else_expr) {
            return StaticType::Unknown;
        }
        const StaticType then_type = infer_expr(*ternary->then_expr);
        const StaticType else_type = infer_expr(*ternary->else_expr);
        if (then_type == else_type) {
            return then_type;
        }
        return StaticType::Unknown;
    }
    if (const auto* assign = dynamic_cast<const AssignExpr*>(&expr)) {
        if (!assign->value || !assign->target) {
            return StaticType::Unknown;
        }
        if (assign->op == TokenKind::Equal) {
            return infer_expr(*assign->value);
        }
        return combine_binary(infer_expr(*assign->target), infer_expr(*assign->value),
                              compound_binary(assign->op));
    }
    if (const auto* update = dynamic_cast<const UpdateExpr*>(&expr)) {
        if (!update->target) {
            return StaticType::Unknown;
        }
        return infer_expr(*update->target);
    }
    return StaticType::Unknown;
}

StaticType Compiler::infer_literal(const LiteralExpr& expr) const {
    switch (expr.kind) {
        case LiteralExpr::Kind::Integer:
            return StaticType::Integer;
        case LiteralExpr::Kind::Float:
            return StaticType::Float;
        case LiteralExpr::Kind::String:
            return StaticType::String;
        case LiteralExpr::Kind::True:
        case LiteralExpr::Kind::False:
            return StaticType::Bool;
        case LiteralExpr::Kind::Null:
            return StaticType::Null;
        case LiteralExpr::Kind::Duration:
            return StaticType::Unknown;
    }
    return StaticType::Unknown;
}

StaticType Compiler::infer_unary(const UnaryExpr& expr) const {
    if (!expr.operand) {
        return StaticType::Unknown;
    }
    const StaticType operand = infer_expr(*expr.operand);
    if (is_unary_not(expr.op)) {
        return StaticType::Bool;
    }
    if (expr.op == TokenKind::Tilde) {
        return StaticType::Integer;
    }
    if (operand == StaticType::Integer || operand == StaticType::Float) {
        return operand;
    }
    return StaticType::Unknown;
}

StaticType Compiler::infer_binary(const BinaryExpr& expr) const {
    if (!expr.left || !expr.right) {
        return StaticType::Unknown;
    }
    if (is_null_coalesce(expr.op)) {
        const StaticType right = infer_expr(*expr.right);
        const StaticType left = infer_expr(*expr.left);
        if (left == StaticType::Null) {
            return right;
        }
        return right == StaticType::Unknown ? left : right;
    }
    return combine_binary(infer_expr(*expr.left), infer_expr(*expr.right), expr.op);
}

StaticType Compiler::variable_static_type(const std::string& name) const {
    return variable_type_descriptor(name).kind;
}

StaticType Compiler::parse_type_name(const std::string& name, const Location& loc) const {
    if (name == "int") {
        return StaticType::Integer;
    }
    if (name == "float") {
        return StaticType::Float;
    }
    if (name == "bool") {
        return StaticType::Bool;
    }
    if (name == "string" || name == "str") {
        return StaticType::String;
    }
    if (name == "any") {
        return StaticType::Any;
    }
    if (name == "null") {
        fail("Type error: 'null' is not a variable type", loc);
    }
    for (const StructInfo& info : bytecode_.structs) {
        if (info.name == name) {
            return StaticType::Struct;
        }
    }
    for (const ClassInfo& info : bytecode_.classes) {
        if (info.name == name) {
            return StaticType::Class;
        }
    }
    for (const EnumInfo& info : bytecode_.enums) {
        if (info.name == name) {
            return StaticType::Enum;
        }
    }
    fail("Type error: unknown type '" + name + "'", loc);
}

TypeDescriptor Compiler::parse_type_descriptor(const std::string& spec, const Location& loc) const {
    return parse_type_spec(spec, loc, bytecode_, active_type_params());
}

const std::vector<std::string>* Compiler::active_type_params() const {
    if (type_param_scopes_.empty()) {
        return nullptr;
    }
    return &type_param_scopes_.back();
}

void Compiler::push_type_param_scope(const std::vector<std::string>& names) {
    type_param_scopes_.push_back(names);
}

void Compiler::pop_type_param_scope() {
    if (!type_param_scopes_.empty()) {
        type_param_scopes_.pop_back();
    }
}

std::vector<std::uint32_t> Compiler::function_candidates(const Symbol& symbol) const {
    if (!symbol.function_overloads.empty()) {
        return symbol.function_overloads;
    }
    return {symbol.function_index};
}

TypeDescriptor Compiler::variable_type_descriptor(const std::string& name) const {
    const auto it = variable_types_.find(name);
    if (it == variable_types_.end()) {
        return TypeDescriptor{};
    }
    if (it->second.declared) {
        return *it->second.declared;
    }
    if (it->second.inferred) {
        return *it->second.inferred;
    }
    return TypeDescriptor{};
}

std::optional<TypeDescriptor> Compiler::try_unify_literal_types(const TypeDescriptor& left,
                                                                const TypeDescriptor& right) const {
    if (left.kind == StaticType::Unknown) {
        return right;
    }
    if (right.kind == StaticType::Unknown) {
        return left;
    }
    if (types_exactly_equal(left, right)) {
        return left;
    }
    if (left.kind == StaticType::Integer && right.kind == StaticType::Float) {
        return right;
    }
    if (left.kind == StaticType::Float && right.kind == StaticType::Integer) {
        return left;
    }
    return std::nullopt;
}

TypeDescriptor Compiler::unify_literal_types(const TypeDescriptor& left, const TypeDescriptor& right,
                                             const Location& loc) const {
    if (const std::optional<TypeDescriptor> unified = try_unify_literal_types(left, right)) {
        return *unified;
    }
    fail("Type error: cannot infer generic collection element type", loc);
}

TypeDescriptor Compiler::infer_type_descriptor(const Expr& expr) const {
    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(&expr)) {
        return variable_type_descriptor(ident->name);
    }
    if (const auto* list = dynamic_cast<const ListExpr*>(&expr)) {
        TypeDescriptor desc;
        desc.kind = StaticType::List;
        if (list->elements.empty()) {
            return desc;
        }
        TypeDescriptor element = infer_type_descriptor(*list->elements[0]);
        bool homogeneous = true;
        for (std::size_t i = 1; i < list->elements.size(); ++i) {
            if (!list->elements[i]) {
                continue;
            }
            const std::optional<TypeDescriptor> unified =
                try_unify_literal_types(element, infer_type_descriptor(*list->elements[i]));
            if (!unified) {
                homogeneous = false;
                break;
            }
            element = *unified;
        }
        if (homogeneous) {
            desc.type_args.push_back(element);
        }
        return desc;
    }
    if (const auto* tuple = dynamic_cast<const TupleExpr*>(&expr)) {
        TypeDescriptor desc;
        desc.kind = StaticType::Tuple;
        for (const auto& element : tuple->elements) {
            if (element) {
                desc.type_args.push_back(infer_type_descriptor(*element));
            }
        }
        return desc;
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(&expr)) {
        if (!index->object || !index->index) {
            TypeDescriptor unknown;
            unknown.kind = StaticType::Unknown;
            return unknown;
        }
        const TypeDescriptor object_type = infer_type_descriptor(*index->object);
        if (object_type.kind == StaticType::List && !object_type.type_args.empty()) {
            return object_type.type_args[0];
        }
        if (object_type.kind == StaticType::Dict && object_type.type_args.size() >= 2) {
            return object_type.type_args[1];
        }
        if (object_type.kind == StaticType::Tuple && !object_type.type_args.empty()) {
            if (const auto* lit = dynamic_cast<const LiteralExpr*>(index->index.get())) {
                if (lit->kind == LiteralExpr::Kind::Integer) {
                    const std::size_t idx = static_cast<std::size_t>(std::stoull(lit->lexeme));
                    if (idx < object_type.type_args.size()) {
                        return object_type.type_args[idx];
                    }
                }
            }
        }
        TypeDescriptor unknown;
        unknown.kind = StaticType::Unknown;
        return unknown;
    }
    if (const auto* dict = dynamic_cast<const DictExpr*>(&expr)) {
        TypeDescriptor desc;
        desc.kind = StaticType::Dict;
        TypeDescriptor key_desc;
        key_desc.kind = StaticType::String;
        if (dict->entries.empty()) {
            return desc;
        }
        TypeDescriptor value_desc = infer_type_descriptor(*dict->entries[0].value);
        bool homogeneous = true;
        for (std::size_t i = 1; i < dict->entries.size(); ++i) {
            if (!dict->entries[i].value) {
                continue;
            }
            const std::optional<TypeDescriptor> unified =
                try_unify_literal_types(value_desc, infer_type_descriptor(*dict->entries[i].value));
            if (!unified) {
                homogeneous = false;
                break;
            }
            value_desc = *unified;
        }
        if (homogeneous) {
            desc.type_args.push_back(key_desc);
            desc.type_args.push_back(value_desc);
        }
        return desc;
    }

    TypeDescriptor desc;
    desc.kind = infer_expr(expr);
    if (const auto* ident = dynamic_cast<const IdentifierExpr*>(&expr)) {
        if (const Symbol* symbol = lookup_symbol(ident->name)) {
            if (symbol->kind == SymbolKind::Enum) {
                desc.enum_index = symbol->enum_index;
            }
        }
    } else if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (const Symbol* symbol = lookup_symbol(call->name)) {
            if (symbol->kind == SymbolKind::Struct) {
                desc.struct_index = symbol->struct_index;
            } else if (symbol->kind == SymbolKind::Class) {
                desc.class_index = symbol->class_index;
            }
        }
    } else if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expr)) {
        if (field->object) {
            if (const auto* ident = dynamic_cast<const IdentifierExpr*>(field->object.get())) {
                if (const Symbol* symbol = lookup_symbol(ident->name)) {
                    if (symbol->kind == SymbolKind::Enum) {
                        desc.enum_index = symbol->enum_index;
                    }
                }
            }
        }
    } else if (const auto* call = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (call->object && call->method == "iter") {
            if (const std::optional<std::uint32_t> class_index = infer_class_index(*call->object)) {
                if (const std::optional<ClassMethodLookup> iter_method = find_class_method(*class_index, "iter")) {
                    const ClassMethodInfo& info =
                        bytecode_.classes[iter_method->class_index].methods[iter_method->method_index];
                    if (info.iterator_class_index != kInvalidIndex) {
                        desc.kind = StaticType::Class;
                        desc.class_index = info.iterator_class_index;
                        return desc;
                    }
                }
            }
        }
    }
    if (const auto* await_expr = dynamic_cast<const AwaitExpr*>(&expr)) {
        if (!await_expr->operand) {
            TypeDescriptor unknown;
            unknown.kind = StaticType::Unknown;
            return unknown;
        }
        const TypeDescriptor task_type = infer_type_descriptor(*await_expr->operand);
        if (task_type.kind == StaticType::Task && !task_type.type_args.empty()) {
            return task_type.type_args[0];
        }
        TypeDescriptor unknown;
        unknown.kind = StaticType::Unknown;
        return unknown;
    }
    if (const auto* call = dynamic_cast<const CallExpr*>(&expr)) {
        if (const Symbol* symbol = lookup_symbol(call->name)) {
            if (symbol->kind == SymbolKind::Function) {
                const FunctionInfo& fn = bytecode_.functions[symbol->function_index];
                if (fn.is_async) {
                    return async_function_result_type(fn, expr.loc);
                }
                if (fn.return_type_name && !fn.return_type_name->empty()) {
                    return function_body_return_type(fn, expr.loc);
                }
            }
        }
    }
    if (const auto* call = dynamic_cast<const MethodCallExpr*>(&expr)) {
        if (call->object) {
            if (const std::optional<std::uint32_t> class_index = infer_class_index(*call->object)) {
                if (const std::optional<ClassMethodLookup> method = find_class_method(*class_index, call->method)) {
                    const FunctionInfo& fn = bytecode_.functions[method->function_index];
                    if (fn.is_async) {
                        return async_function_result_type(fn, expr.loc);
                    }
                    if (fn.return_type_name && !fn.return_type_name->empty()) {
                        return function_body_return_type(fn, expr.loc);
                    }
                }
            }
        }
    }
    return desc;
}

TypeDescriptor Compiler::function_body_return_type(const FunctionInfo& fn, const Location& loc) const {
    if (!fn.return_type_name || fn.return_type_name->empty()) {
        TypeDescriptor unknown;
        unknown.kind = StaticType::Unknown;
        return unknown;
    }
    return parse_type_descriptor(*fn.return_type_name, loc);
}

TypeDescriptor Compiler::async_function_result_type(const FunctionInfo& fn, const Location& loc) const {
    TypeDescriptor task;
    task.kind = StaticType::Task;
    if (fn.return_type_name && !fn.return_type_name->empty()) {
        task.type_args.push_back(parse_type_descriptor(*fn.return_type_name, loc));
    }
    return task;
}

void Compiler::check_expression_matches_return(const Expr& expr, const FunctionInfo& fn, const Location& loc) const {
    if (!fn.return_type_name || fn.return_type_name->empty()) {
        return;
    }
    const TypeDescriptor expected = function_body_return_type(fn, loc);
    const TypeDescriptor actual = infer_type_descriptor(expr);
    check_assignable(expected, actual, loc);
}

void Compiler::reject_const_assignment(const std::string& name, const Location& loc) const {
    const Symbol* symbol = lookup_symbol(name);
    if (symbol && symbol->kind == SymbolKind::Variable && symbol->is_const) {
        fail("cannot assign to const '" + name + "'", loc);
    }
}

void Compiler::check_assignable(const TypeDescriptor& target, const TypeDescriptor& source, const Location& loc) const {
    const AssignabilityResult result =
        is_assignable(target, source, [this](std::uint32_t child, std::uint32_t ancestor) {
            return is_class_or_subclass(child, ancestor);
        });
    if (!result.ok) {
        fail(std::string("Type error: expected ") + type_descriptor_to_string(target) + ", got " +
                 type_descriptor_to_string(source),
             loc);
    }
}

void Compiler::check_collection_literal(const Expr& expr, const TypeDescriptor& target, const Location& loc) const {
    if (const auto* list = dynamic_cast<const ListExpr*>(&expr)) {
        if (target.kind != StaticType::List || target.type_args.empty()) {
            return;
        }
        for (const auto& element : list->elements) {
            if (!element) {
                continue;
            }
            const TypeDescriptor element_type = infer_type_descriptor(*element);
            check_assignable(target.type_args[0], element_type, element->loc);
        }
        return;
    }
    if (const auto* dict = dynamic_cast<const DictExpr*>(&expr)) {
        if (target.kind != StaticType::Dict || target.type_args.size() < 2) {
            return;
        }
        for (const auto& entry : dict->entries) {
            if (!entry.value) {
                continue;
            }
            if (!entry.key) {
                fail("dictionary key must be a string literal", loc);
            }
            const auto* key_literal = dynamic_cast<const LiteralExpr*>(entry.key.get());
            if (!key_literal || key_literal->kind != LiteralExpr::Kind::String) {
                fail("dictionary key must be a string literal", loc);
            }
            TypeDescriptor key_type;
            key_type.kind = StaticType::String;
            check_assignable(target.type_args[0], key_type, entry.key->loc);
            check_assignable(target.type_args[1], infer_type_descriptor(*entry.value), entry.value->loc);
        }
    }
}

void Compiler::check_method_argument_types(const Expr& object, const std::string& method,
                                           const std::vector<std::unique_ptr<Expr>>& arguments,
                                           const Location& loc) const {
    if (arguments.empty()) {
        return;
    }
    const TypeDescriptor object_type = infer_type_descriptor(object);
    if (object_type.kind == StaticType::Class && !object_type.type_args.empty()) {
        if (method == "push" || method == "enqueue" || method == "push_front" || method == "push_back") {
            if (arguments.size() == 1 && arguments[0]) {
                check_assignable(object_type.type_args[0], infer_type_descriptor(*arguments[0]),
                                 arguments[0]->loc);
            }
        }
        return;
    }
    if (object_type.kind != StaticType::List || object_type.type_args.empty()) {
        return;
    }
    if (method != "append" && method != "prepend" && method != "insert") {
        return;
    }
    if (arguments.size() != 1 || !arguments[0]) {
        return;
    }
    check_assignable(object_type.type_args[0], infer_type_descriptor(*arguments[0]), arguments[0]->loc);
}

std::uint32_t Compiler::intern_type_check(const TypeDescriptor& target) {
    TypeCheckInfo info;
    info.expected = target.kind;
    if (target.struct_index) {
        info.struct_index = *target.struct_index;
    }
    if (target.class_index) {
        info.class_index = *target.class_index;
    }
    if (target.enum_index) {
        info.enum_index = *target.enum_index;
    }
    if (target.kind == StaticType::Union) {
        for (const TypeDescriptor& member : target.type_args) {
            info.union_member_indices.push_back(intern_type_check(member));
        }
    } else {
        for (const TypeDescriptor& arg : target.type_args) {
            info.type_arg_indices.push_back(intern_type_check(arg));
        }
    }
    for (std::size_t i = 0; i < bytecode_.type_checks.size(); ++i) {
        const TypeCheckInfo& existing = bytecode_.type_checks[i];
        if (existing.expected == info.expected && existing.struct_index == info.struct_index &&
            existing.class_index == info.class_index && existing.enum_index == info.enum_index &&
            existing.type_arg_indices == info.type_arg_indices &&
            existing.union_member_indices == info.union_member_indices) {
            return static_cast<std::uint32_t>(i);
        }
    }
    const auto index = static_cast<std::uint32_t>(bytecode_.type_checks.size());
    bytecode_.type_checks.push_back(info);
    return index;
}

void Compiler::apply_assign_conversions(Register reg, const TypeDescriptor& target, const TypeDescriptor& source,
                                        const Location& loc) {
    const AssignabilityResult result =
        is_assignable(target, source, [this](std::uint32_t child, std::uint32_t ancestor) {
            return is_class_or_subclass(child, ancestor);
        });
    if (!result.ok) {
        fail(std::string("Type error: expected ") + type_descriptor_to_string(target) + ", got " +
                 type_descriptor_to_string(source),
             loc);
    }
    if (result.coerce_int_to_float) {
        emit(Instruction{Opcode::CoerceToFloat, reg, reg, 0});
    }
    if (result.runtime_check) {
        emit(Instruction{Opcode::CheckAssign, reg, reg, intern_type_check(target)});
    }
}

void Compiler::apply_call_argument_conversions(Register reg, const TypeDescriptor& target, const TypeDescriptor& source,
                                               const Location& loc) {
    apply_assign_conversions(reg, target, source, loc);
}

Register Compiler::alloc_register() {
    return next_register_++;
}

std::uint32_t Compiler::intern_constant(Constant constant) {
    const std::string key = constant_key(constant);
    const auto it = constant_keys_.find(key);
    if (it != constant_keys_.end()) {
        return it->second;
    }
    const auto index = static_cast<std::uint32_t>(bytecode_.constants.size());
    bytecode_.constants.push_back(std::move(constant));
    constant_keys_.emplace(key, index);
    return index;
}

std::uint32_t Compiler::intern_call(std::string name, std::uint32_t argc) {
    for (std::size_t i = 0; i < bytecode_.calls.size(); ++i) {
        if (bytecode_.calls[i].name == name && bytecode_.calls[i].argc == argc) {
            return static_cast<std::uint32_t>(i);
        }
    }
    const auto index = static_cast<std::uint32_t>(bytecode_.calls.size());
    bytecode_.calls.push_back(CallInfo{std::move(name), argc});
    return index;
}

void Compiler::emit(Instruction instruction) {
    bytecode_.instructions.push_back(instruction);
    SourceLocation location;
    const int adjusted_line = current_emit_loc_.line - source_line_base_;
    location.line = adjusted_line > 0 ? static_cast<std::uint32_t>(adjusted_line) : 1u;
    location.column = current_emit_loc_.column > 0 ? static_cast<std::uint32_t>(current_emit_loc_.column) : 1u;
    bytecode_.instruction_locations.push_back(location);
}

Opcode Compiler::binary_opcode(TokenKind op, const Location& loc) {
    switch (op) {
        case TokenKind::Plus:
            return Opcode::Add;
        case TokenKind::Minus:
            return Opcode::Sub;
        case TokenKind::Star:
            return Opcode::Mul;
        case TokenKind::Slash:
            return Opcode::Div;
        case TokenKind::Percent:
            return Opcode::Mod;
        case TokenKind::EqualEqual:
            return Opcode::Equal;
        case TokenKind::BangEqual:
            return Opcode::NotEqual;
        case TokenKind::Less:
            return Opcode::Less;
        case TokenKind::LessEqual:
            return Opcode::LessEqual;
        case TokenKind::Greater:
            return Opcode::Greater;
        case TokenKind::GreaterEqual:
            return Opcode::GreaterEqual;
        case TokenKind::Amp:
            return Opcode::BitAnd;
        case TokenKind::Pipe:
            return Opcode::BitOr;
        case TokenKind::Caret:
            return Opcode::BitXor;
        case TokenKind::LessLess:
            return Opcode::Shl;
        case TokenKind::GreaterGreater:
            return Opcode::Shr;
        default:
            fail("unsupported binary operator", loc);
    }
}

[[noreturn]] void Compiler::fail(const std::string& message, const Location& loc) const {
    throw CompilerError{message, loc.line, loc.column};
}

void Compiler::set_module_mode(bool enabled) {
    is_module_ = enabled;
}

void Compiler::set_module_cache(ModuleCompileCache* cache) {
    module_cache_ = cache;
}

void Compiler::set_root_bytecode(Bytecode* root) {
    root_bytecode_ = root;
}

void Compiler::set_compiling_module_index(std::uint32_t module_index) {
    compiling_module_index_ = module_index;
}

const std::vector<ModuleExport>& Compiler::module_exports() const {
    return module_exports_;
}

void Compiler::set_compile_options(const CompileOptions& options) {
    compile_options_ = options;
    bytecode_.source_file = options.source_file;
    bytecode_.source_line_base = options.source_line_base;
    source_line_base_ = options.source_line_base;
}

void Compiler::track_export(const std::string& name, ExportKind kind, std::uint32_t index) {
    if (!is_module_) {
        return;
    }
    for (const ModuleExport& existing : module_exports_) {
        if (existing.name == name) {
            return;
        }
    }
    ModuleExport export_entry;
    export_entry.name = name;
    export_entry.kind = kind;
    export_entry.index = index;
    module_exports_.push_back(std::move(export_entry));
}

void Compiler::build_module_exports() {
    if (scope_stack_.empty()) {
        return;
    }
    module_exports_.clear();
    for (const auto& entry : scope_stack_.front().symbols) {
        const Symbol& symbol = entry.second;
        if (symbol.local || in_function_ || symbol.access != SymbolAccess::Public) {
            continue;
        }
        switch (symbol.kind) {
            case SymbolKind::Variable:
                track_export(symbol.name, ExportKind::Variable, symbol.slot);
                break;
            case SymbolKind::Function: {
                ModuleExport export_entry;
                export_entry.name = symbol.name;
                export_entry.kind = ExportKind::Function;
                export_entry.index = symbol.function_index;
                if (!symbol.function_overloads.empty()) {
                    export_entry.function_overload_indices = symbol.function_overloads;
                }
                module_exports_.push_back(std::move(export_entry));
                break;
            }
            case SymbolKind::Interface:
                track_export(symbol.name, ExportKind::Interface, symbol.interface_index);
                break;
            case SymbolKind::Class:
                track_export(symbol.name, ExportKind::Class, symbol.class_index);
                break;
            case SymbolKind::Enum:
                track_export(symbol.name, ExportKind::Enum, symbol.enum_index);
                break;
            case SymbolKind::Struct:
                track_export(symbol.name, ExportKind::Struct, symbol.struct_index);
                break;
        }
    }
}

void Compiler::check_module_level_access(MemberAccess access, const Location& loc) const {
    if (!in_function_ && access == MemberAccess::Protected) {
        fail("protected is not allowed at module level", loc);
    }
}

const Bytecode& Compiler::module_bytecode_for(std::uint32_t module_index, const Location& loc) const {
    if (!root_bytecode_ || module_index >= root_bytecode_->modules.size()) {
        fail("invalid module", loc);
    }
    const ModuleDescriptor& descriptor = root_bytecode_->modules[module_index];
    if (descriptor.bytecode_index >= root_bytecode_->module_bytecodes.size()) {
        fail("invalid module bytecode", loc);
    }
    return root_bytecode_->module_bytecodes[descriptor.bytecode_index];
}

std::uint32_t Compiler::link_class_from_module(std::uint32_t module_index, const ModuleExport& export_info,
                                               const Location& loc) {
    const Bytecode& module_bc = module_bytecode_for(module_index, loc);
    if (export_info.kind != ExportKind::Class || export_info.index >= module_bc.classes.size()) {
        fail("invalid module export", loc);
    }
    const ClassInfo& source = module_bc.classes[export_info.index];
    for (std::size_t i = 0; i < bytecode_.classes.size(); ++i) {
        const ClassInfo& existing = bytecode_.classes[i];
        if (existing.module_index == module_index && existing.name == source.name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    ClassInfo linked = source;
    linked.module_index = module_index;
    linked.external_index = export_info.index;
    if (linked.parent_index != 0xFFFFFFFFu && linked.parent_index < module_bc.classes.size()) {
        const ClassInfo& parent = module_bc.classes[linked.parent_index];
        linked.parent_index = link_class_from_module(
            module_index, ModuleExport{parent.name, ExportKind::Class, linked.parent_index}, loc);
    }
    const auto index = static_cast<std::uint32_t>(bytecode_.classes.size());
    bytecode_.classes.push_back(std::move(linked));
    return index;
}

std::uint32_t Compiler::link_enum_from_module(std::uint32_t module_index, const ModuleExport& export_info,
                                              const Location& loc) {
    const Bytecode& module_bc = module_bytecode_for(module_index, loc);
    if (export_info.kind != ExportKind::Enum || export_info.index >= module_bc.enums.size()) {
        fail("invalid module export", loc);
    }
    const EnumInfo& source = module_bc.enums[export_info.index];
    const auto index = static_cast<std::uint32_t>(bytecode_.enums.size());
    bytecode_.enums.push_back(source);
    return index;
}

std::uint32_t Compiler::link_struct_from_module(std::uint32_t module_index, const ModuleExport& export_info,
                                                const Location& loc) {
    const Bytecode& module_bc = module_bytecode_for(module_index, loc);
    if (export_info.kind != ExportKind::Struct || export_info.index >= module_bc.structs.size()) {
        fail("invalid module export", loc);
    }
    const StructInfo& source = module_bc.structs[export_info.index];
    const auto index = static_cast<std::uint32_t>(bytecode_.structs.size());
    bytecode_.structs.push_back(source);
    return index;
}

void Compiler::declare_imported_symbol(const std::string& local_name, std::uint32_t module_index,
                                       const ModuleExport& export_info, const Location& loc) {
    if (current_scope_defines(local_name)) {
        fail("duplicate declaration of '" + local_name + "'", loc);
    }
    Symbol symbol;
    symbol.name = local_name;
    symbol.access = SymbolAccess::Public;
    symbol.owner_function = current_function();
    switch (export_info.kind) {
        case ExportKind::Variable:
            declare_variable(local_name, std::nullopt, loc);
            return;
        case ExportKind::Function: {
            symbol.kind = SymbolKind::Function;
            symbol.import_module_index = module_index;
            symbol.import_export_name = export_info.name;
            symbol.function_index = export_info.index;
            if (!export_info.function_overload_indices.empty()) {
                symbol.function_overloads = export_info.function_overload_indices;
            }
            scope_stack_.back().symbols.emplace(local_name, std::move(symbol));
            return;
        }
        case ExportKind::Class: {
            symbol.kind = SymbolKind::Class;
            symbol.class_index = link_class_from_module(module_index, export_info, loc);
            track_class_type(local_name, symbol.class_index);
            scope_stack_.back().symbols.emplace(local_name, std::move(symbol));
            return;
        }
        case ExportKind::Enum: {
            symbol.kind = SymbolKind::Enum;
            symbol.enum_index = link_enum_from_module(module_index, export_info, loc);
            track_enum_type(local_name, symbol.enum_index);
            scope_stack_.back().symbols.emplace(local_name, std::move(symbol));
            return;
        }
        case ExportKind::Struct: {
            symbol.kind = SymbolKind::Struct;
            symbol.struct_index = link_struct_from_module(module_index, export_info, loc);
            track_struct_type(local_name, symbol.struct_index);
            scope_stack_.back().symbols.emplace(local_name, std::move(symbol));
            return;
        }
    }
}

std::string Compiler::default_module_binding_name(const std::string& module_path) const {
    const std::size_t dot = module_path.rfind('.');
    if (dot == std::string::npos) {
        return module_path;
    }
    return module_path.substr(dot + 1);
}

Register Compiler::emit_string_constant(const std::string& value) {
    Constant constant;
    constant.kind = ConstantKind::String;
    constant.lexeme = encode_string_lexeme(value);
    const Register dest = alloc_register();
    emit(Instruction{Opcode::LoadConst, dest, intern_constant(std::move(constant)), 0});
    return dest;
}

std::uint32_t Compiler::resolve_module_index(const ImportStmt& stmt) {
    if (!module_cache_ || !root_bytecode_) {
        fail("module imports require a module loader", stmt.loc);
    }
    return module_cache_->compile_module(stmt.module, compile_options_, *root_bytecode_, stmt.loc);
}

void Compiler::declare_import(const ImportStmt& stmt) {
    const std::uint32_t module_index = resolve_module_index(stmt);
    if (stmt.from_import) {
        if (stmt.bindings.empty()) {
            fail("expected import name", stmt.loc);
        }
        for (const ImportBinding& binding : stmt.bindings) {
            const std::string local_name = binding.alias ? *binding.alias : binding.name;
            if (root_bytecode_ && module_index < root_bytecode_->modules.size()) {
                const ModuleDescriptor& descriptor = root_bytecode_->modules[module_index];
                if (!descriptor.canonical_path.empty()) {
                    const ModuleExport* export_info = module_cache_->find_export(module_index, binding.name);
                    if (!export_info) {
                        fail("module '" + stmt.module + "' has no export '" + binding.name + "'", binding.loc);
                    }
                    declare_imported_symbol(local_name, module_index, *export_info, binding.loc);
                    continue;
                }
            }
            if (current_scope_defines(local_name)) {
                fail("duplicate declaration of '" + local_name + "'", binding.loc);
            }
            declare_variable(local_name, std::nullopt, binding.loc);
        }
        return;
    }
    const std::string local_name =
        stmt.module_alias ? *stmt.module_alias : default_module_binding_name(stmt.module);
    if (current_scope_defines(local_name)) {
        fail("duplicate declaration of '" + local_name + "'", stmt.loc);
    }
    declare_variable(local_name, std::nullopt, stmt.loc);
}

void Compiler::compile_import(const ImportStmt& stmt) {
    const std::uint32_t module_index = resolve_module_index(stmt);
    if (stmt.from_import) {
        for (const ImportBinding& binding : stmt.bindings) {
            const std::string local_name = binding.alias ? *binding.alias : binding.name;
            const Symbol* symbol = lookup_symbol(local_name);
            if (symbol && symbol->kind != SymbolKind::Variable) {
                if (symbol->kind == SymbolKind::Function && symbol->import_module_index != 0xFFFFFFFFu) {
                    continue;
                }
                continue;
            }
            Constant name_constant;
            name_constant.kind = ConstantKind::String;
            name_constant.lexeme = encode_string_lexeme(binding.name);
            const std::uint32_t name_index = intern_constant(std::move(name_constant));
            const Register value = alloc_register();
            emit(Instruction{Opcode::ImportName, value, module_index, name_index});
            emit_store_name(local_name, value, binding.loc);
        }
        return;
    }
    const std::string local_name =
        stmt.module_alias ? *stmt.module_alias : default_module_binding_name(stmt.module);
    const Register module_value = alloc_register();
    emit(Instruction{Opcode::ImportModule, module_value, module_index, 0});
    emit_store_name(local_name, module_value, stmt.loc);
}

Bytecode compile(const Program& program, const CompileOptions& options) {
    ModuleCompileCache module_cache;
    Compiler compiler;
    compiler.set_compile_options(options);
    compiler.set_module_cache(&module_cache);
    return compiler.compile(program);
}

const char* exception_prelude_source() {
    return "class Exception:\n"
           "    public message\n"
           "    public cause\n"
           "    public file\n"
           "    public line\n"
           "    public column\n"
           "    public stack_trace\n"
           "    constructor(message, cause):\n"
           "        this.message = message\n"
           "        this.cause = cause\n"
           "        this.file = \"\"\n"
           "        this.line = 0\n"
           "        this.column = 0\n"
           "        this.stack_trace = null\n"
           "class RuntimeException : Exception:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "class ValueException : RuntimeException:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "class TypeException : RuntimeException:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "class NameException : RuntimeException:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "class IndexException : RuntimeException:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "class CancellationException : RuntimeException:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "class TimeoutException : RuntimeException:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "class StopIteration : RuntimeException:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "class NetworkException : RuntimeException:\n"
           "    constructor(message, cause):\n"
           "        parent(message, cause)\n"
           "enum Result:\n"
           "    Ok(value)\n"
           "    Err(error)\n";
}

int exception_prelude_line_count() {
    int lines = 0;
    for (const char* cursor = exception_prelude_source(); *cursor != '\0'; ++cursor) {
        if (*cursor == '\n') {
            ++lines;
        }
    }
    return lines;
}

Bytecode compile(std::string source) {
    return compile(parse(std::move(source)), {});
}

Bytecode compile_with_builtins(std::string source, const std::string& source_file) {
    std::string combined = std::string(exception_prelude_source()) + source;
    CompileOptions options;
    options.source_file = source_file;
    options.source_line_base = exception_prelude_line_count();
    return compile(parse(std::move(combined)), options);
}

}  // namespace pscript

#include "type_parse.cpp"
