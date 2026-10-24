#pragma once

#include "compiler/bytecode.h"
#include "compiler/compiler_error.h"
#include "compiler/generic.h"
#include "compiler/type_compat.h"
#include "compiler/type_parse.h"
#include "compiler/types.h"
#include "parser/ast.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pscript {

class ModuleCompileCache;

class Compiler {
public:
    void set_compile_options(const CompileOptions& options);
    void set_module_mode(bool enabled);
    void set_module_cache(ModuleCompileCache* cache);
    void set_root_bytecode(Bytecode* root);
    void set_compiling_module_index(std::uint32_t module_index);
    const std::vector<ModuleExport>& module_exports() const;
    Bytecode compile(const Program& program);

private:
    enum class SymbolKind { Variable, Function, Struct, Class, Enum, Interface };
    using SymbolAccess = MemberAccess;

    struct VariableType {
        std::optional<TypeDescriptor> declared;
        std::optional<TypeDescriptor> inferred;
    };

    struct LoopContext {
        std::vector<std::uint32_t> break_jumps;
        std::vector<std::uint32_t> continue_jumps;
    };

    struct TryContext {
        std::uint32_t handler_index = 0;
        bool has_finally = false;
    };

    struct ScopeBinding {
        std::string name;
        bool existed = false;
        std::optional<std::uint32_t> previous_index;
        std::optional<VariableType> previous_type;
        bool previous_local = false;
    };

    struct Symbol {
        std::string name;
        std::uint32_t slot = 0;
        VariableType types;
        SymbolKind kind = SymbolKind::Variable;
        SymbolAccess access = SymbolAccess::Public;
        bool explicit_declaration = false;
        bool local = false;
        bool initialized = false;
        bool is_const = false;
        std::uint32_t function_index = 0;
        std::vector<std::uint32_t> function_overloads;
        std::uint32_t struct_index = 0;
        std::uint32_t class_index = 0;
        std::uint32_t enum_index = 0;
        std::uint32_t interface_index = 0;
        std::uint32_t import_module_index = 0xFFFFFFFFu;
        std::string import_export_name;
        std::uint32_t bound_function_index = 0xFFFFFFFFu;
        std::vector<std::uint32_t> bound_function_overloads;
        std::string bound_callable_name;
        std::uint32_t bound_import_module_index = 0xFFFFFFFFu;
        std::uint32_t owner_function = 0xFFFFFFFFu;
    };

    struct Scope {
        std::unordered_map<std::string, Symbol> symbols;
        std::vector<ScopeBinding> bindings;
    };

    struct NestedFunction {
        const FuncStmt* stmt = nullptr;
        std::uint32_t index = 0;
        std::uint32_t slot = 0;
    };

    struct ClassCompileContext {
        std::uint32_t class_index = 0;
        bool in_constructor = false;
    };

    static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

    struct ClassMethodLookup {
        std::uint32_t class_index = 0;
        std::uint32_t method_index = 0;
        std::uint32_t function_index = 0;
    };

    struct ClassFieldLookup {
        std::uint32_t field_index = 0;
        std::uint32_t declaring_class_index = 0;
        MemberAccess access = MemberAccess::Public;
    };

    void compile_stmt(const Stmt& stmt);
    Register compile_expr(const Expr& expr);
    void compile_expr_into(const Expr& expr, Register dest);
    void compile_assign(const AssignStmt& stmt);
    void compile_destructure_assign(const DestructureAssignStmt& stmt);
    void compile_destructure_pattern(const DestructPattern& pattern, Register source, const Expr* rhs_hint,
                                     const Location& loc);
    void compile_collection_destructure_pattern(const DestructPattern& pattern, Register source,
                                                const Expr* rhs_hint, const Location& loc,
                                                std::uint32_t collection_kind);
    void validate_destructure_pattern_rhs(const DestructPattern& pattern, const Expr& rhs, const Location& loc);
    void emit_check_destruct(Register value, std::uint32_t expected_count, std::uint32_t kind);
    void emit_destruct_rest(Register dest, Register source, std::uint32_t start_index);
    void emit_destruct_dict_rest(Register dest, Register source, const std::vector<std::string>& excluded_keys);
    void compile_dict_destructure_pattern(const DestructPattern& pattern, Register source, const Expr* rhs_hint,
                                          const Location& loc);
    void compile_struct_destructure_pattern(const DestructPattern& pattern, Register source, const Expr* rhs_hint,
                                            const Location& loc);
    void compile_parameter_destructure(const DestructPattern& pattern, Register source, const Location& loc);
    Register compile_dict_key_index(Register source, const std::string& key, const Location& loc);
    void compile_struct_rest_dict(Register dest, Register source, std::uint32_t struct_index,
                                  const std::vector<std::string>& excluded_fields, const Location& loc);
    void compile_index_assign(const IndexAssignStmt& stmt);
    void compile_expr_stmt(const ExprStmt& stmt);
    void compile_print(const PrintStmt& stmt);
    void compile_if(const IfStmt& stmt);
    void compile_while(const WhileStmt& stmt);
    void compile_for(const ForStmt& stmt);
    void compile_async_for(const AsyncForStmt& stmt);
    void compile_async_with(const AsyncWithStmt& stmt);
    void compile_break(const BreakStmt& stmt);
    void compile_continue(const ContinueStmt& stmt);
    void emit_bool_const(Register dest, bool value);
    Register emit_bool_and(Register left, Register right);
    Register emit_bool_or(Register left, Register right);
    void mark_enum_match_coverage(const MatchPattern& pattern, std::vector<bool>& covered,
                                  const Location& loc) const;
    void compile_match(const MatchStmt& stmt);
    void compile_match_expr_into(const MatchExpr& expr, Register dest);
    void compile_propagate_expr_into(const PropagateExpr& expr, Register dest);
    Register emit_match_pattern_test(const MatchPattern& pattern, Register value_reg, const Location& loc);
    Register emit_match_destruct_pattern_test(const DestructPattern& pattern, Register value_reg, const Location& loc);
    void emit_match_pattern_bindings(const MatchPattern& pattern, Register value_reg, const Location& loc);
    void emit_match_destruct_bindings(const DestructPattern& pattern, Register source, const Location& loc);
    bool match_pattern_is_catch_all(const MatchPattern& pattern) const;
    std::string match_pattern_key(const MatchPattern& pattern, const Location& loc) const;
    struct ClassPropertyLookup {
        std::uint32_t class_index = 0;
        std::uint32_t property_index = 0;
        MemberAccess getter_access = MemberAccess::Public;
        MemberAccess setter_access = MemberAccess::Public;
        std::uint32_t getter_function_index = 0xFFFFFFFFu;
        std::uint32_t setter_function_index = 0xFFFFFFFFu;
        bool has_getter = false;
        bool has_setter = false;
    };
    std::optional<ClassPropertyLookup> find_class_property(std::uint32_t class_index,
                                                           const std::string& name) const;
    void check_class_property_access(const ClassPropertyLookup& property, bool is_write,
                                     const Location& loc) const;
    void compile_function(const FuncStmt& stmt);
    void compile_module_async_entry(std::uint32_t function_index, const std::vector<const Stmt*>& statements);
    void compile_function_body(const std::vector<std::unique_ptr<Stmt>>& statements, const Location& loc);
    void compile_return(const ReturnStmt& stmt);
    void compile_try(const TryStmt& stmt);
    void compile_throw(const ThrowStmt& stmt);
    void compile_defer(const DeferStmt& stmt);
    void compile_enum_variant_call(Register dest, const Symbol& symbol, const std::string& member,
                                   const std::vector<std::unique_ptr<Expr>>& arguments, const Location& loc);
    void declare_import(const ImportStmt& stmt);
    void compile_import(const ImportStmt& stmt);
    std::uint32_t resolve_module_index(const ImportStmt& stmt);
    void build_module_exports();
    void track_export(const std::string& name, ExportKind kind, std::uint32_t index);
    void check_module_level_access(MemberAccess access, const Location& loc) const;
    const Bytecode& module_bytecode_for(std::uint32_t module_index, const Location& loc) const;
    std::uint32_t link_class_from_module(std::uint32_t module_index, const ModuleExport& export_info,
                                         const Location& loc);
    std::uint32_t link_enum_from_module(std::uint32_t module_index, const ModuleExport& export_info,
                                        const Location& loc);
    std::uint32_t link_struct_from_module(std::uint32_t module_index, const ModuleExport& export_info,
                                          const Location& loc);
    void declare_imported_symbol(const std::string& local_name, std::uint32_t module_index,
                                 const ModuleExport& export_info, const Location& loc);
    Register emit_string_constant(const std::string& value);
    std::string default_module_binding_name(const std::string& module_path) const;
    void validate_catch_clauses(const TryStmt& stmt) const;
    void emit_return_value(Register src);
    void emit_return_null();
    void declare_function(const FuncStmt& stmt);
    void declare_interface(const InterfaceStmt& stmt);
    void declare_struct(const StructStmt& stmt);
    void predeclare_class(const ClassStmt& stmt);
    void finalize_class(const ClassStmt& stmt);
    void compile_class(const ClassStmt& stmt);
    void compile_class_member(const FuncStmt& stmt, std::uint32_t class_index, std::uint32_t function_index,
                              bool is_constructor);
    std::uint32_t declare_class_function(const std::string& name, std::uint32_t arity, bool has_implicit_this,
                                         bool is_constructor);
    void declare_enum(const EnumStmt& stmt);
    void declare_type_alias(const TypeAliasStmt& stmt);
    std::uint32_t resolve_struct_index(const std::string& name, const Location& loc) const;
    std::uint32_t resolve_enum_index(const std::string& name, const Location& loc) const;
    std::uint32_t resolve_field_index(std::uint32_t struct_index, const std::string& field,
                                      const Location& loc) const;
    void check_field_access(std::uint32_t struct_index, const std::string& field, const Location& loc) const;
    void check_class_field_access(const ClassFieldLookup& field, const Location& loc) const;
    ClassFieldLookup lookup_class_field(std::uint32_t class_index, const std::string& field,
                                        const Location& loc) const;
    std::optional<ClassFieldLookup> find_class_field(std::uint32_t class_index, const std::string& field) const;
    ClassMethodLookup lookup_class_method(std::uint32_t class_index, const std::string& method,
                                          const Location& loc, bool super_call = false) const;
    std::optional<ClassMethodLookup> find_class_method(std::uint32_t class_index, const std::string& method) const;
    ClassMethodLookup lookup_super_method(std::uint32_t class_index, const std::string& method,
                                          const Location& loc) const;
    bool is_class_or_subclass(std::uint32_t class_index, std::uint32_t ancestor_index) const;
    void build_class_layout(std::uint32_t class_index, const Location& loc);
    void rebuild_all_class_layouts();
    void validate_class_constructors(const std::vector<const ClassStmt*>& classes);
    void validate_class_cycles() const;
    std::uint32_t find_class_by_name(const std::string& name) const;
    std::uint32_t find_interface_by_name(const std::string& name) const;
    bool class_implements_interface(std::uint32_t class_index, std::uint32_t interface_index) const;
    bool type_satisfies_constraint(const TypeDescriptor& type, const std::string& constraint_spec,
                                   const Location& loc) const;
    void validate_class_interfaces(const ClassStmt& stmt, ClassInfo& info);
    void resolve_class_bases(const ClassStmt& stmt, ClassInfo& info);
    std::vector<AnnotationInfo> copy_annotations(const std::vector<Annotation>& annotations) const;
    const FunctionInfo& function_info_for_symbol(const Symbol& symbol, const Location& loc) const;
    const std::vector<Parameter>& parameters_for_function(std::uint32_t function_index,
                                                          std::uint32_t import_module_index,
                                                          const Location& loc);
    std::optional<std::uint32_t> detect_iterator_class_from_method(const FuncStmt& method) const;
    bool constructor_calls_parent(const std::vector<std::unique_ptr<Stmt>>& body) const;
    void emit_invoke_super(std::uint32_t function_index, std::uint32_t argc, Register first);
    static const char* member_access_name(MemberAccess access);
    std::uint32_t resolve_enum_member(std::uint32_t enum_index, const std::string& member,
                                      const Location& loc) const;
    std::optional<std::uint32_t> infer_struct_index(const Expr& expr) const;
    std::optional<std::uint32_t> infer_class_index(const Expr& expr) const;
    void track_struct_type(const std::string& name, std::uint32_t struct_index);
    void track_class_type(const std::string& name, std::uint32_t class_index);
    void track_enum_type(const std::string& name, std::uint32_t enum_index);
    std::vector<NestedFunction> hoist_functions(const std::vector<std::unique_ptr<Stmt>>& statements);
    void predeclare_statements(const std::vector<std::unique_ptr<Stmt>>& statements);
    void predeclare_stmt(const Stmt& stmt);
    void compile_nested_functions(const std::vector<NestedFunction>& nested, bool jump_over);
    void emit_closures(const std::vector<NestedFunction>& nested);
    void compile_call_value(Register dest, const CallExpr& call);
    void compile_direct_call(Register dest, const CallExpr& call, const Symbol& symbol);
    void compile_generic_type_call(Register dest, const CallExpr& call, const Symbol& symbol);
    std::vector<std::uint32_t> function_candidates(const Symbol& symbol) const;
    void push_type_param_scope(const std::vector<std::string>& names);
    void pop_type_param_scope();
    const std::vector<std::string>* active_type_params() const;
    std::uint32_t intern_capture(const Symbol& symbol, const Location& loc);
    std::uint32_t intern_capture_in(std::uint32_t function_index, const Symbol& symbol, const Location& loc);
    std::uint32_t current_function() const;
    void compile_logical_into(const BinaryExpr& expr, Register dest);
    void compile_null_coalesce_into(const BinaryExpr& expr, Register dest);
    void compile_ternary_into(const TernaryExpr& expr, Register dest);
    void compile_assign_expr_into(const AssignExpr& expr, Register dest);
    void compile_update_into(const UpdateExpr& expr, Register dest);
    void compile_membership_into(const MembershipExpr& expr, Register dest);
    void reject_index_write(const Expr& object, const Location& loc);
    void compile_block(const std::vector<std::unique_ptr<Stmt>>& statements, const Location& loc);
    void compile_scoped_block(const std::vector<std::unique_ptr<Stmt>>& statements, const Location& loc);
    void compile_integer_into(Register dest, std::int64_t value);
    void compile_duration_into(const Expr& expr, Register dest);
    void compile_builtin_call_arguments(const CallExpr& call, Register first);
    void compile_embed_call_into(const Location& loc, const std::vector<std::unique_ptr<Expr>>& arguments, Register dest,
                                 bool text_mode);
    void reject_bare_delay_call(const Expr& expr);
    void patch_loop_jumps(const std::vector<std::uint32_t>& jumps, std::uint32_t target);
    void push_scope();
    void pop_scope();
    bool current_scope_defines(const std::string& name) const;
    std::optional<std::uint32_t> lookup_variable(const std::string& name) const;
    std::uint32_t introduce_variable(const std::string& name, const std::optional<std::string>& type_name,
                                     const Location& loc, bool explicit_declaration,
                                     MemberAccess access = MemberAccess::Public, bool is_const = false);
    std::uint32_t declare_variable(const std::string& name, const std::optional<std::string>& type_name,
                                   const Location& loc, MemberAccess access = MemberAccess::Public,
                                   bool is_const = false);
    std::uint32_t resolve_assign(const std::string& name, const Location& loc);
    std::uint32_t intern_binding(const std::string& name, const Location& loc);
    const Symbol* lookup_symbol(const std::string& name) const;
    Symbol* lookup_symbol(const std::string& name);
    void mark_initialized(const std::string& name);
    bool variable_is_local(const std::string& name) const;
    void emit_load_name(Register dest, const std::string& name, const Location& loc);
    void emit_store_name(const std::string& name, Register src, const Location& loc);
    void emit_load_null(Register dest);
    void note_registers();

    std::uint32_t current_ip() const;
    std::uint32_t emit_jump();
    std::uint32_t emit_jump_if_false(Register condition);
    std::uint32_t emit_jump_if_true(Register condition);
    void patch_jump(std::uint32_t jump_index, std::uint32_t target);

    StaticType infer_expr(const Expr& expr) const;
    StaticType infer_literal(const LiteralExpr& expr) const;
    StaticType infer_unary(const UnaryExpr& expr) const;
    StaticType infer_binary(const BinaryExpr& expr) const;
    StaticType variable_static_type(const std::string& name) const;
    StaticType parse_type_name(const std::string& name, const Location& loc) const;
    TypeDescriptor parse_type_descriptor(const std::string& spec, const Location& loc) const;
    TypeDescriptor variable_type_descriptor(const std::string& name) const;
    TypeDescriptor infer_type_descriptor(const Expr& expr) const;
    std::optional<TypeDescriptor> try_unify_literal_types(const TypeDescriptor& left, const TypeDescriptor& right) const;
    TypeDescriptor unify_literal_types(const TypeDescriptor& left, const TypeDescriptor& right, const Location& loc) const;
    void check_assignable(const TypeDescriptor& target, const TypeDescriptor& source, const Location& loc) const;
    TypeDescriptor function_body_return_type(const FunctionInfo& fn, const Location& loc) const;
    TypeDescriptor async_function_result_type(const FunctionInfo& fn, const Location& loc) const;
    void check_expression_matches_return(const Expr& expr, const FunctionInfo& fn, const Location& loc) const;
    void reject_const_assignment(const std::string& name, const Location& loc) const;
    void check_collection_literal(const Expr& expr, const TypeDescriptor& target, const Location& loc) const;
    void check_method_argument_types(const Expr& object, const std::string& method,
                                     const std::vector<std::unique_ptr<Expr>>& arguments, const Location& loc) const;
    void apply_assign_conversions(Register reg, const TypeDescriptor& target, const TypeDescriptor& source,
                                  const Location& loc);
    void apply_call_argument_conversions(Register reg, const TypeDescriptor& target, const TypeDescriptor& source,
                                         const Location& loc);
    std::uint32_t intern_type_check(const TypeDescriptor& target);

    Register alloc_register();
    std::uint32_t intern_constant(Constant constant);
    std::uint32_t intern_call(std::string name, std::uint32_t argc);
    std::uint32_t intern_class_call(ClassCallKind kind, std::uint32_t class_index, std::uint32_t function_index,
                                    std::uint32_t argc);
    void emit(Instruction instruction);
    FunctionDebugInfo& current_function_debug();
    void record_debug_local(const std::string& name, std::uint32_t slot, DebugLocalKind kind, const Location& loc);
    void close_debug_local_scope(const std::string& name, std::uint32_t end_ip);
    void warn_deprecated_use(const std::vector<AnnotationInfo>& annotations, const Location& loc);
    Opcode binary_opcode(TokenKind op, const Location& loc);
    [[noreturn]] void fail(const std::string& message, const Location& loc) const;

    Bytecode bytecode_;
    std::uint32_t next_register_ = 0;
    std::unordered_map<std::string, std::uint32_t> constant_keys_;
    std::unordered_map<std::string, std::uint32_t> variable_index_;
    std::unordered_map<std::string, VariableType> variable_types_;
    std::unordered_map<std::string, bool> variable_local_;
    std::vector<LoopContext> loop_stack_;
    std::vector<TryContext> try_stack_;
    std::vector<Scope> scope_stack_;
    bool in_function_ = false;
    bool emitting_body_ = false;
    std::uint32_t next_local_ = 0;
    std::vector<std::uint32_t> function_stack_;
    int async_function_depth_ = 0;
    std::optional<ClassCompileContext> class_context_;
    std::unordered_map<std::string, std::uint32_t> class_call_keys_;
    std::unordered_map<std::string, std::optional<std::string>> pending_class_parents_;
    Location current_emit_loc_{1, 1};
    int source_line_base_ = 0;
    CompileOptions compile_options_;
    bool is_module_ = false;
    ModuleCompileCache* module_cache_ = nullptr;
    Bytecode* root_bytecode_ = nullptr;
    std::uint32_t compiling_module_index_ = 0xFFFFFFFFu;
    std::vector<ModuleExport> module_exports_;
    std::vector<std::vector<std::string>> type_param_scopes_;
    std::unordered_map<std::uint32_t, const std::vector<Parameter>*> function_parameters_;
    std::unordered_map<std::uint64_t, std::vector<Parameter>> imported_function_parameters_;
    std::unordered_map<const FuncStmt*, std::uint32_t> function_decl_index_;
    std::unordered_map<std::string, TypeDescriptor> call_generic_subst_;
    std::vector<CompileWarning> warnings_;
};

Bytecode compile(const Program& program, const CompileOptions& options = {});
Bytecode compile(std::string source);
const char* exception_prelude_source();
int exception_prelude_line_count();
Bytecode compile_with_builtins(std::string source, const std::string& source_file = "<stdin>");

}  // namespace pscript
