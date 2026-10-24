#include "compiler/bytecode.h"

#include "runtime/runtime_limits.h"

#include <sstream>

namespace pscript {
namespace {

void write_reg(std::ostringstream& out, std::uint32_t index) {
    out << "r" << index;
}

void write_var(std::ostringstream& out, std::uint32_t index) {
    out << "v" << index;
}

}  // namespace

const char* constant_kind_name(ConstantKind kind) {
    switch (kind) {
        case ConstantKind::Integer:
            return "integer";
        case ConstantKind::Float:
            return "float";
        case ConstantKind::String:
            return "string";
        case ConstantKind::Bool:
            return "bool";
        case ConstantKind::Null:
            return "null";
        case ConstantKind::Bytes:
            return "bytes";
    }
    return "unknown";
}

std::string Bytecode::disassemble() const {
    std::ostringstream out;
    out << "Registers: " << register_count << "\n";

    out << "Constants:\n";
    if (constants.empty()) {
        out << "  (none)\n";
    }
    for (std::size_t i = 0; i < constants.size(); ++i) {
        const Constant& c = constants[i];
        out << "  " << i << " " << constant_kind_name(c.kind);
        if (c.kind != ConstantKind::Null) {
            out << " " << c.lexeme;
        }
        out << "\n";
    }

    out << "Variables:\n";
    if (variables.empty()) {
        out << "  (none)\n";
    }
    for (std::size_t i = 0; i < variables.size(); ++i) {
        const Variable& v = variables[i];
        out << "  ";
        write_var(out, static_cast<std::uint32_t>(i));
        out << " " << v.name;
        if (v.type_name) {
            out << " : " << *v.type_name;
        }
        out << "\n";
    }

    out << "Calls:\n";
    if (calls.empty()) {
        out << "  (none)\n";
    }
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const CallInfo& call = calls[i];
        out << "  " << i << " " << call.name << " argc=" << call.argc << "\n";
    }

    out << "Functions:\n";
    if (functions.empty()) {
        out << "  (none)\n";
    }
    for (std::size_t i = 0; i < functions.size(); ++i) {
        const FunctionInfo& fn = functions[i];
        out << "  " << i << " " << fn.name << " arity=" << fn.arity << " entry=" << fn.entry
            << " locals=" << fn.local_count << " regs=" << fn.register_count << "\n";
    }

    out << "Structs:\n";
    if (structs.empty()) {
        out << "  (none)\n";
    }
    for (std::size_t i = 0; i < structs.size(); ++i) {
        const StructInfo& info = structs[i];
        out << "  " << i << " " << info.name << " fields=" << info.fields.size() << "\n";
    }

    out << "Classes:\n";
    if (classes.empty()) {
        out << "  (none)\n";
    }
    for (std::size_t i = 0; i < classes.size(); ++i) {
        const ClassInfo& info = classes[i];
        out << "  " << i << " " << info.name << " fields=" << info.fields.size()
            << " methods=" << info.methods.size() << "\n";
    }

    out << "ClassCalls:\n";
    if (class_calls.empty()) {
        out << "  (none)\n";
    }
    for (std::size_t i = 0; i < class_calls.size(); ++i) {
        const ClassCallInfo& call = class_calls[i];
        out << "  " << i << " class=" << call.class_index << " fn=" << call.function_index
            << " argc=" << call.argc << "\n";
    }

    out << "Enums:\n";
    if (enums.empty()) {
        out << "  (none)\n";
    }
    for (std::size_t i = 0; i < enums.size(); ++i) {
        const EnumInfo& info = enums[i];
        out << "  " << i << " " << info.name << " members=" << info.members.size() << "\n";
    }

    out << "Code:\n";
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        const Instruction& ins = instructions[i];
        out << "  " << i << " " << opcode_name(ins.opcode);
        switch (ins.opcode) {
            case Opcode::LoadConst:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b;
                if (ins.b < constants.size()) {
                    out << "  ; " << constants[ins.b].lexeme;
                }
                break;
            case Opcode::LoadVar:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_var(out, ins.b);
                if (ins.b < variables.size()) {
                    out << "  ; " << variables[ins.b].name;
                }
                break;
            case Opcode::StoreVar:
                out << " ";
                write_var(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                if (ins.a < variables.size()) {
                    out << "  ; " << variables[ins.a].name;
                }
                break;
            case Opcode::UnaryPlus:
            case Opcode::UnaryMinus:
            case Opcode::UnaryNot:
            case Opcode::UnaryBitNot:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod:
            case Opcode::Equal:
            case Opcode::NotEqual:
            case Opcode::Less:
            case Opcode::LessEqual:
            case Opcode::Greater:
            case Opcode::GreaterEqual:
            case Opcode::BitAnd:
            case Opcode::BitOr:
            case Opcode::BitXor:
            case Opcode::Shl:
            case Opcode::Shr:
            case Opcode::Contains:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", ";
                write_reg(out, ins.c);
                break;
            case Opcode::Print:
                out << " ";
                write_reg(out, ins.a);
                break;
            case Opcode::PrintN:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b;
                break;
            case Opcode::Jump:
                out << " " << ins.a;
                break;
            case Opcode::JumpIfFalse:
            case Opcode::JumpIfTrue:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b;
                break;
            case Opcode::MakeList:
            case Opcode::MakeDict:
            case Opcode::MakeTuple:
            case Opcode::MakeSet:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::MakeStruct:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b;
                if (ins.b < structs.size()) {
                    out << "  ; " << structs[ins.b].name;
                }
                break;
            case Opcode::InvokeClass:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                if (ins.c < class_calls.size()) {
                    const ClassCallInfo& call = class_calls[ins.c];
                    if (call.class_index < classes.size()) {
                        out << "  ; " << classes[call.class_index].name;
                    }
                }
                break;
            case Opcode::CallClassMethod:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::InvokeSuper:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b;
                break;
            case Opcode::GetField:
            case Opcode::SetField:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::LoadEnum:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b << ", " << ins.c;
                break;
            case Opcode::CheckDestruct:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b << ", " << ins.c;
                break;
            case Opcode::TestDestructShape:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << (ins.c >> 8) << ", " << (ins.c & 0xFFu);
                break;
            case Opcode::TestInstanceOf:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::DestructRest:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::DestructDictRest:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::GetIndex:
            case Opcode::SetIndex:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", ";
                write_reg(out, ins.c);
                break;
            case Opcode::CallMethod:
            case Opcode::CallBuiltin:
            case Opcode::Call:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                if ((ins.opcode == Opcode::CallMethod || ins.opcode == Opcode::CallBuiltin) && ins.c < calls.size()) {
                    out << "  ; " << calls[ins.c].name;
                }
                if (ins.opcode == Opcode::Call && ins.c < functions.size()) {
                    out << "  ; " << functions[ins.c].name;
                }
                break;
            case Opcode::Return:
                out << " ";
                write_reg(out, ins.a);
                break;
            case Opcode::LoadLocal:
                out << " ";
                write_reg(out, ins.a);
                out << ", l" << ins.b;
                break;
            case Opcode::StoreLocal:
                out << " l" << ins.a << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::LoadUpvalue:
                out << " ";
                write_reg(out, ins.a);
                out << ", u" << ins.b;
                break;
            case Opcode::StoreUpvalue:
                out << " u" << ins.a << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::MakeClosure:
            case Opcode::LoadFunction:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b;
                if (ins.b < functions.size()) {
                    out << "  ; " << functions[ins.b].name;
                }
                break;
            case Opcode::CallValue:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::IterLen:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::IterGet:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", ";
                write_reg(out, ins.c);
                break;
            case Opcode::GetIter:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::IterNext:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::CoerceToFloat:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::CheckAssign:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", tc" << ins.c;
                break;
            case Opcode::EnterTry:
            case Opcode::LeaveTry:
                out << " h" << ins.a;
                break;
            case Opcode::Throw:
                out << " ";
                write_reg(out, ins.a);
                break;
            case Opcode::LoadPendingException:
                out << " ";
                write_reg(out, ins.a);
                break;
            case Opcode::EndFinally:
                break;
            case Opcode::DeferReturn:
                out << " h" << ins.a;
                out << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::DeferBreak:
            case Opcode::DeferContinue:
                out << " h" << ins.a << ", @" << ins.b;
                break;
            case Opcode::PushDefer:
                out << " ";
                write_reg(out, ins.a);
                break;
            case Opcode::SetEnumPayload:
            case Opcode::GetEnumPayload:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::TestEnumMember:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                out << ", " << ins.c;
                break;
            case Opcode::MakeEnumEx:
                out << " ";
                write_reg(out, ins.a);
                out << ", " << ins.b;
                out << ", ";
                write_reg(out, ins.c);
                break;
            case Opcode::Await:
                out << " ";
                write_reg(out, ins.a);
                out << ", ";
                write_reg(out, ins.b);
                break;
            case Opcode::Halt:
                break;
        }
        out << "\n";
    }
    return out.str();
}

std::optional<std::string> Bytecode::validate() const {
    if (instructions.size() > limits::kMaxBytecodeInstructions) {
        return "bytecode instruction count exceeds limit";
    }
    if (constants.size() > limits::kMaxBytecodeConstants) {
        return "bytecode constant count exceeds limit";
    }
    if (functions.size() > limits::kMaxBytecodeFunctions) {
        return "bytecode function count exceeds limit";
    }
    if (register_count > limits::kMaxBytecodeRegisters) {
        return "bytecode register count exceeds limit";
    }
    if (!instruction_locations.empty() && instruction_locations.size() != instructions.size()) {
        return "instruction location metadata mismatch";
    }
    auto check_reg = [this](std::uint32_t reg) -> std::optional<std::string> {
        if (reg >= register_count) {
            return "invalid register";
        }
        return std::nullopt;
    };
    auto check_const = [this](std::uint32_t index) -> std::optional<std::string> {
        if (index >= constants.size()) {
            return "invalid constant index";
        }
        return std::nullopt;
    };
    auto check_var = [this](std::uint32_t index) -> std::optional<std::string> {
        if (index >= variables.size()) {
            return "invalid variable index";
        }
        return std::nullopt;
    };

    for (const Instruction& ins : instructions) {
        switch (ins.opcode) {
            case Opcode::LoadConst:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_const(ins.b)) {
                    return err;
                }
                break;
            case Opcode::LoadVar:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_var(ins.b)) {
                    return err;
                }
                break;
            case Opcode::StoreVar:
                if (auto err = check_var(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::UnaryPlus:
            case Opcode::UnaryMinus:
            case Opcode::UnaryNot:
            case Opcode::UnaryBitNot:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::Add:
            case Opcode::Sub:
            case Opcode::Mul:
            case Opcode::Div:
            case Opcode::Mod:
            case Opcode::Equal:
            case Opcode::NotEqual:
            case Opcode::Less:
            case Opcode::LessEqual:
            case Opcode::Greater:
            case Opcode::GreaterEqual:
            case Opcode::BitAnd:
            case Opcode::BitOr:
            case Opcode::BitXor:
            case Opcode::Shl:
            case Opcode::Shr:
            case Opcode::Contains:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (auto err = check_reg(ins.c)) {
                    return err;
                }
                break;
            case Opcode::Print:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                break;
            case Opcode::PrintN: {
                if (ins.b == 0) {
                    return "invalid instruction";
                }
                if (ins.a >= register_count || ins.b > register_count - ins.a) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::Jump:
                if (ins.a >= instructions.size()) {
                    return "invalid jump";
                }
                break;
            case Opcode::JumpIfFalse:
            case Opcode::JumpIfTrue:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.b >= instructions.size()) {
                    return "invalid jump";
                }
                break;
            case Opcode::MakeList: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.c != 0 && (ins.b >= register_count || ins.c > register_count - ins.b)) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::MakeTuple: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.c != 0 && (ins.b >= register_count || ins.c > register_count - ins.b)) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::MakeSet: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.c != 0 && (ins.b >= register_count || ins.c > register_count - ins.b)) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::MakeStruct:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.b >= structs.size()) {
                    return "invalid struct index";
                }
                break;
            case Opcode::InvokeClass: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.c >= class_calls.size()) {
                    return "invalid class call index";
                }
                const std::uint32_t argc = class_calls[ins.c].argc;
                if (argc != 0 && (ins.b >= register_count || argc > register_count - ins.b)) {
                    return "invalid register";
                }
                if (class_calls[ins.c].class_index >= classes.size()) {
                    return "invalid class index";
                }
                break;
            }
            case Opcode::CallClassMethod: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (ins.c >= class_calls.size()) {
                    return "invalid class call index";
                }
                const std::uint32_t argc = class_calls[ins.c].argc;
                const std::uint32_t needed = 1 + argc;
                if (ins.b >= register_count || needed > register_count - ins.b) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::InvokeSuper: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.b >= class_calls.size()) {
                    return "invalid class call index";
                }
                const std::uint32_t argc = class_calls[ins.b].argc;
                if (argc != 0 && (ins.a >= register_count || argc > register_count - ins.a)) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::GetField:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::SetField:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.c)) {
                    return err;
                }
                break;
            case Opcode::LoadEnum:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.b >= enums.size()) {
                    return "invalid enum index";
                }
                break;
            case Opcode::MakeDict: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.c != 0) {
                    const std::uint32_t needed = ins.c * 2;
                    if (ins.b >= register_count || needed > register_count - ins.b) {
                        return "invalid register";
                    }
                }
                break;
            }
            case Opcode::GetIndex:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (auto err = check_reg(ins.c)) {
                    return err;
                }
                break;
            case Opcode::SetIndex:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (auto err = check_reg(ins.c)) {
                    return err;
                }
                break;
            case Opcode::CheckDestruct:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                break;
            case Opcode::TestDestructShape:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::TestInstanceOf:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::DestructRest:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::DestructDictRest:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::CallMethod: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.c >= calls.size()) {
                    return "invalid call index";
                }
                const std::uint32_t needed = 1 + calls[ins.c].argc;
                if (ins.b >= register_count || needed > register_count - ins.b) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::CallBuiltin: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.c >= calls.size()) {
                    return "invalid call index";
                }
                const std::uint32_t argc = calls[ins.c].argc;
                if (argc != 0 && (ins.b >= register_count || argc > register_count - ins.b)) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::Call: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.c >= functions.size()) {
                    return "invalid function index";
                }
                const std::uint32_t argc = functions[ins.c].arity;
                if (argc != 0 && (ins.b >= register_count || argc > register_count - ins.b)) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::Return:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                break;
            case Opcode::LoadLocal:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                break;
            case Opcode::StoreLocal:
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::LoadUpvalue:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                break;
            case Opcode::StoreUpvalue:
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::MakeClosure:
            case Opcode::LoadFunction:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (ins.b >= functions.size()) {
                    return "invalid function index";
                }
                break;
            case Opcode::CallValue: {
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (ins.c != 0 && (ins.b + 1 >= register_count || ins.c > register_count - (ins.b + 1))) {
                    return "invalid register";
                }
                break;
            }
            case Opcode::IterLen:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::IterGet:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (auto err = check_reg(ins.c)) {
                    return err;
                }
                break;
            case Opcode::GetIter:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::IterNext:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (ins.c >= instructions.size()) {
                    return "invalid jump";
                }
                break;
            case Opcode::CoerceToFloat:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::CheckAssign:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (ins.c >= type_checks.size()) {
                    return "invalid type check index";
                }
                break;
            case Opcode::EnterTry:
            case Opcode::LeaveTry:
                if (ins.a >= handlers.size()) {
                    return "invalid handler index";
                }
                break;
            case Opcode::Throw:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                break;
            case Opcode::EndFinally:
                break;
            case Opcode::DeferReturn:
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                if (ins.a >= handlers.size()) {
                    return "invalid handler index";
                }
                break;
            case Opcode::DeferBreak:
            case Opcode::DeferContinue:
                if (ins.a >= handlers.size()) {
                    return "invalid handler index";
                }
                if (ins.b >= instructions.size()) {
                    return "invalid jump";
                }
                break;
            case Opcode::PushDefer:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                break;
            case Opcode::SetEnumPayload:
            case Opcode::GetEnumPayload:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::TestEnumMember:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::MakeEnumEx:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.c)) {
                    return err;
                }
                if ((ins.b & 0xFFFFu) >= enums.size()) {
                    return "invalid enum index";
                }
                if ((ins.b >> 16) >= enums[ins.b & 0xFFFFu].members.size()) {
                    return "invalid enum member";
                }
                break;
            case Opcode::ImportModule:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (!modules.empty() && ins.b >= modules.size()) {
                    return "invalid module index";
                }
                break;
            case Opcode::ImportName:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (!modules.empty() && ins.b >= modules.size()) {
                    return "invalid module index";
                }
                if (auto err = check_const(ins.c)) {
                    return err;
                }
                break;
            case Opcode::Await:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                if (auto err = check_reg(ins.b)) {
                    return err;
                }
                break;
            case Opcode::LoadPendingException:
                if (auto err = check_reg(ins.a)) {
                    return err;
                }
                break;
            case Opcode::Halt:
                break;
            default:
                return "invalid instruction";
        }
    }
    return std::nullopt;
}

}  // namespace pscript
