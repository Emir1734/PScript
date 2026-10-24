#pragma once

#include "compiler/types.h"
#include "parser/ast.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pscript {

using Register = std::uint32_t;

enum class Opcode {
    LoadConst,
    LoadVar,
    StoreVar,
    UnaryPlus,
    UnaryMinus,
    UnaryNot,
    UnaryBitNot,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    Contains,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Print,
    PrintN,
    Jump,
    JumpIfFalse,
    JumpIfTrue,
    MakeList,
    MakeDict,
    MakeTuple,
    MakeSet,
    MakeStruct,
    InvokeClass,
    CallClassMethod,
    InvokeSuper,
    GetField,
    SetField,
    LoadEnum,
    GetIndex,
    SetIndex,
    CheckDestruct,
    TestDestructShape,
    TestInstanceOf,
    DestructRest,
    DestructDictRest,
    CallMethod,
    CallBuiltin,
    Call,
    Return,
    LoadLocal,
    StoreLocal,
    LoadUpvalue,
    StoreUpvalue,
    MakeClosure,
    LoadFunction,
    CallValue,
    IterLen,
    IterGet,
    GetIter,
    IterNext,
    CoerceToFloat,
    CheckAssign,
    EnterTry,
    LeaveTry,
    Throw,
    EndFinally,
    DeferReturn,
    DeferBreak,
    DeferContinue,
    PushDefer,
    SetEnumPayload,
    GetEnumPayload,
    TestEnumMember,
    MakeEnumEx,
    ImportModule,
    ImportName,
    Await,
    LoadPendingException,
    Halt,
};

enum class ConstantKind {
    Integer,
    Float,
    String,
    Bool,
    Null,
    Bytes,
};

struct Constant {
    ConstantKind kind = ConstantKind::Null;
    std::string lexeme;
    bool bool_value = false;
    std::vector<std::uint8_t> bytes_value;
};

// Register instruction encoding:
//   LOAD_CONST  dest, const_index
//   LOAD_VAR    dest, var_index
//   STORE_VAR   var_index, src
//   UNARY_*     dest, src
//   binary ops  dest, lhs, rhs
//   CONTAINS    dest, value, container
//   PRINT          src
//   PRINT_N        first, count
//   JUMP           target
//   JUMP_IF_FALSE  condition, target
//   JUMP_IF_TRUE   condition, target
//   MAKE_LIST      dest, first, count
//   MAKE_DICT      dest, first, pair_count
//   MAKE_TUPLE     dest, first, count
//   MAKE_SET       dest, first, count
//   MAKE_STRUCT    dest, struct_index
//   INVOKE_CLASS   dest, first, call_index ; args at first..first+argc-1
//   CALL_CLASS_METHOD dest, object, call_index ; args at object+1..object+argc
//   INVOKE_SUPER   first, call_index ; parent constructor on this (local 0), args at first..first+argc-1
//   GET_FIELD      dest, object, field_index
//   SET_FIELD      object, field_index, value
//   LOAD_ENUM      dest, enum_index, member_index
//   GET_INDEX      dest, object, index
//   SET_INDEX      object, index, value
//   CALL_METHOD    dest, first, call_index   ; object at first, args at first+1..first+argc
//   CALL_BUILTIN   dest, first, call_index   ; args at first..first+argc-1
//   CALL           dest, first, function_index ; args at first..first+arity-1
//   RETURN         src
//   LOAD_LOCAL     dest, local_index
//   STORE_LOCAL    local_index, src
//   LOAD_UPVALUE   dest, upvalue_index
//   STORE_UPVALUE  upvalue_index, src
//   MAKE_CLOSURE   dest, function_index
//   LOAD_FUNCTION  dest, function_index
//   CALL_VALUE     dest, callee, argc          ; args at callee+1..callee+argc
//   ITER_LEN       dest, collection
//   ITER_GET       dest, collection, index
//   GET_ITER       dest, iterable
//   ITER_NEXT      dest, iterator, exit_target
//   HALT
struct Instruction {
    Opcode opcode = Opcode::Halt;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;
};

struct Variable {
    std::string name;
    std::optional<std::string> type_name;
};

struct CallInfo {
    std::string name;
    std::uint32_t argc = 0;
};

struct Capture {
    std::uint32_t index = 0;
    bool from_local = true;
};

struct SourceLocation {
    std::uint32_t line = 0;
    std::uint32_t column = 0;
};

struct TypeCheckInfo {
    StaticType expected = StaticType::Unknown;
    std::uint32_t struct_index = 0xFFFFFFFFu;
    std::uint32_t class_index = 0xFFFFFFFFu;
    std::uint32_t enum_index = 0xFFFFFFFFu;
    std::vector<std::uint32_t> type_arg_indices;
    std::vector<std::uint32_t> union_member_indices;
};

struct AnnotationInfo {
    std::string name;
    std::vector<std::pair<std::optional<std::string>, std::string>> args;
    SourceLocation loc;
};

struct CompileWarning {
    std::string message;
    SourceLocation loc;
};

enum class DebugLocalKind : std::uint8_t {
    Parameter,
    Local,
    Upvalue,
};

struct DebugLocalBinding {
    std::string name;
    std::uint32_t slot = 0;
    DebugLocalKind kind = DebugLocalKind::Local;
    std::uint32_t decl_line = 0;
    std::uint32_t decl_column = 0;
    std::uint32_t scope_start_ip = 0;
    std::uint32_t scope_end_ip = 0xFFFFFFFFu;
};

struct FunctionDebugInfo {
    std::vector<DebugLocalBinding> locals;
    std::vector<std::string> upvalue_names;
};

struct FunctionInfo {
    std::string name;
    std::uint32_t arity = 0;
    std::uint32_t min_arity = 0;
    std::vector<std::string> generic_type_param_constraints;
    bool variadic = false;
    std::uint32_t entry = 0;
    std::uint32_t local_count = 0;
    std::uint32_t register_count = 0;
    bool has_implicit_this = false;
    bool is_constructor = false;
    bool is_async = false;
    bool returns_value = false;
    bool is_native = false;
    std::uint32_t native_function_id = 0xFFFFFFFFu;
    std::vector<Capture> captures;
    std::vector<std::optional<std::string>> parameter_type_names;
    std::optional<std::string> return_type_name;
    std::vector<std::string> generic_type_params;
    std::vector<AnnotationInfo> annotations;
    std::vector<std::vector<AnnotationInfo>> parameter_annotations;
    std::uint8_t pgo_hotness = 0;
};

struct InterfaceMethodInfo {
    std::string name;
    std::vector<std::optional<std::string>> parameter_type_names;
    std::optional<std::string> return_type_name;
    bool is_async = false;
};

struct InterfacePropertyInfo {
    std::string name;
    std::optional<std::string> type_name;
    bool has_getter = false;
    bool has_setter = false;
};

struct InterfaceInfo {
    std::string name;
    std::vector<std::string> extends;
    std::vector<std::string> generic_type_params;
    std::vector<InterfaceMethodInfo> methods;
    std::vector<InterfacePropertyInfo> properties;
    std::vector<AnnotationInfo> annotations;
};

struct StructFieldInfo {
    std::string name;
    std::optional<std::string> type_name;
    MemberAccess access = MemberAccess::Public;
};

struct StructInfo {
    std::string name;
    std::vector<std::string> generic_type_params;
    std::vector<std::string> generic_type_param_constraints;
    std::vector<StructFieldInfo> fields;
    std::vector<AnnotationInfo> annotations;
};

struct ClassMethodInfo {
    std::string name;
    std::uint32_t function_index = 0;
    std::uint32_t iterator_class_index = 0xFFFFFFFFu;
};

struct ClassPropertyInfo {
    std::string name;
    std::optional<std::string> type_name;
    MemberAccess getter_access = MemberAccess::Private;
    MemberAccess setter_access = MemberAccess::Private;
    std::uint32_t getter_function_index = 0xFFFFFFFFu;
    std::uint32_t setter_function_index = 0xFFFFFFFFu;
};

struct ClassFieldLayout {
    std::string name;
    MemberAccess access = MemberAccess::Public;
    std::uint32_t declaring_class_index = 0;
};

struct ClassInfo {
    std::string name;
    std::vector<std::string> generic_type_params;
    std::vector<std::string> generic_type_param_constraints;
    std::uint32_t parent_index = 0xFFFFFFFFu;
    std::vector<std::string> parent_type_argument_specs;
    std::vector<std::uint32_t> interface_indices;
    std::uint32_t module_index = 0xFFFFFFFFu;
    std::uint32_t external_index = 0xFFFFFFFFu;
    std::vector<StructFieldInfo> fields;
    std::vector<AnnotationInfo> annotations;
    std::vector<ClassFieldLayout> layout;
    std::vector<ClassMethodInfo> methods;
    std::vector<ClassPropertyInfo> properties;
    std::uint32_t constructor_index = 0xFFFFFFFFu;
    std::uint32_t native_class_kind = 0;
};

enum class ClassCallKind { Instance, Method, SuperMethod, SuperCtor };

struct ClassCallInfo {
    ClassCallKind kind = ClassCallKind::Method;
    std::uint32_t class_index = 0;
    std::uint32_t function_index = 0xFFFFFFFFu;
    std::uint32_t argc = 0;
};

struct EnumInfo {
    std::string name;
    std::vector<std::string> members;
    std::vector<std::vector<std::string>> member_payloads;
    std::vector<AnnotationInfo> annotations;
    std::vector<std::vector<AnnotationInfo>> member_annotations;
};

struct TypeAliasInfo {
    std::string name;
    std::vector<std::string> type_params;
    std::string target_type;
};

struct CatchEntry {
    std::uint32_t catch_ip = 0xFFFFFFFFu;
    std::uint32_t catch_slot = 0xFFFFFFFFu;
    bool catch_is_local = false;
    std::uint32_t catch_class_index = 0xFFFFFFFFu;
};

struct HandlerInfo {
    std::vector<CatchEntry> catches;
    std::uint32_t finally_ip = 0xFFFFFFFFu;
    std::uint32_t end_ip = 0xFFFFFFFFu;
};

struct CompileOptions {
    std::string source_file = "<stdin>";
    int source_line_base = 0;
};

enum class ExportKind {
    Variable,
    Function,
    Class,
    Enum,
    Struct,
    Interface,
};

struct ModuleExport {
    std::string name;
    ExportKind kind = ExportKind::Variable;
    std::uint32_t index = 0;
    std::vector<std::uint32_t> function_overload_indices;
};

struct ModuleDescriptor {
    std::string specifier;
    std::string canonical_path;
    std::uint32_t bytecode_index = 0;
    std::vector<ModuleExport> exports;
    bool is_native = false;
};

struct Bytecode {
    std::vector<Constant> constants;
    std::vector<Variable> variables;
    std::vector<CallInfo> calls;
    std::vector<FunctionInfo> functions;
    std::vector<StructInfo> structs;
    std::vector<ClassInfo> classes;
    std::vector<ClassCallInfo> class_calls;
    std::vector<EnumInfo> enums;
    std::vector<TypeAliasInfo> type_aliases;
    std::vector<InterfaceInfo> interfaces;
    std::vector<TypeCheckInfo> type_checks;
    std::vector<HandlerInfo> handlers;
    std::vector<Bytecode> module_bytecodes;
    std::vector<ModuleDescriptor> modules;
    std::vector<Instruction> instructions;
    std::vector<SourceLocation> instruction_locations;
    std::vector<FunctionDebugInfo> function_debug;
    std::string source_file;
    std::int32_t source_line_base = 0;
    std::uint32_t register_count = 0;
    std::uint32_t async_main_function_index = 0xFFFFFFFFu;
    std::vector<CompileWarning> warnings;
    std::vector<std::string> compile_dependencies;

    std::string disassemble() const;
    std::optional<std::string> validate() const;
};

const char* opcode_name(Opcode opcode);
const char* constant_kind_name(ConstantKind kind);

}  // namespace pscript
