#include "compiler/compiler.h"
#include "compiler/bytecode.h"
#include "lexer/lexer.h"
#include "parser/parser.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

using pscript::Bytecode;
using pscript::CallInfo;
using pscript::CompileOptions;
using pscript::compile;
using pscript::compile_with_builtins;
using pscript::CompilerError;
using pscript::ConstantKind;
using pscript::FunctionInfo;
using pscript::Instruction;
using pscript::Opcode;
using pscript::LexerError;
using pscript::ParserError;
using pscript::parse;

namespace {

int g_failed = 0;

const FunctionInfo* find_user_function(const Bytecode& bc, std::string_view name) {
    for (const FunctionInfo& fn : bc.functions) {
        if (!fn.is_native && fn.name == name) {
            return &fn;
        }
    }
    return nullptr;
}

std::size_t count_user_functions(const Bytecode& bc) {
    std::size_t count = 0;
    for (const FunctionInfo& fn : bc.functions) {
        if (!fn.is_native) {
            ++count;
        }
    }
    return count;
}

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

std::uint32_t test_process_id() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

class ModuleTempDir {
public:
    ModuleTempDir() {
        const int run = ++counter_;
        path_ = std::filesystem::temp_directory_path() / ("pscript_compiler_module_tests-pid" +
                                                           std::to_string(test_process_id()) + "-run" +
                                                           std::to_string(run));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~ModuleTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::filesystem::path write(const std::string& name, std::string_view contents) const {
        const auto file = path_ / name;
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << contents;
        return file;
    }

    std::string main_path(const std::string& name = "main.phs") const { return (path_ / name).string(); }

private:
    static inline std::atomic<int> counter_{0};
    std::filesystem::path path_;
};

bool opcodes_equal(const Bytecode& bc, const std::vector<Opcode>& expected) {
    if (bc.instructions.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (bc.instructions[i].opcode != expected[i]) {
            return false;
        }
    }
    return true;
}

void expect_error_prefix(std::string_view name, const std::string& source, std::string_view prefix) {
    try {
        compile(source);
        fail_test(name, "expected compiler error");
    } catch (const ParserError& err) {
        if (err.message().rfind(prefix, 0) != 0) {
            fail_test(name, "got \"" + err.message() + "\"");
            return;
        }
        pass_test(name);
    } catch (const CompilerError& err) {
        if (err.message().rfind(prefix, 0) != 0) {
            fail_test(name, "got \"" + err.message() + "\"");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_error(std::string_view name, const std::string& source, std::string_view prefix, int line,
                  int column) {
    try {
        compile(source);
        fail_test(name, "expected compiler error");
    } catch (const ParserError& err) {
        if (err.message().rfind(prefix, 0) != 0 || err.line() != line || err.column() != column) {
            fail_test(name, "got \"" + err.message() + "\" at " + std::to_string(err.line()) + ":" +
                                std::to_string(err.column()));
            return;
        }
        pass_test(name);
    } catch (const CompilerError& err) {
        if (err.message().rfind(prefix, 0) != 0 || err.line() != line || err.column() != column) {
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
        {
            const Bytecode bc = compile("x = 10");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::StoreVar, Opcode::Halt}) ||
                bc.constants.size() != 1 || bc.constants[0].kind != ConstantKind::Integer ||
                bc.constants[0].lexeme != "10" || bc.instructions[0].a != 0 || bc.instructions[0].b != 0) {
                fail_test("integer constant", "unexpected bytecode");
            } else {
                pass_test("integer constant");
            }
        }

        {
            const Bytecode bc = compile("x = 1.5");
            if (bc.constants.size() != 1 || bc.constants[0].kind != ConstantKind::Float ||
                bc.constants[0].lexeme != "1.5") {
                fail_test("float constant", "unexpected constant");
            } else {
                pass_test("float constant");
            }
        }

        {
            const Bytecode bc = compile("x = \"hi\"");
            if (bc.constants.size() != 1 || bc.constants[0].kind != ConstantKind::String ||
                bc.constants[0].lexeme != "\"hi\"") {
                fail_test("string constant", "unexpected constant");
            } else {
                pass_test("string constant");
            }
        }

        {
            const Bytecode bc = compile("a = true b = false c = null");
            if (bc.constants.size() != 3 || bc.constants[0].kind != ConstantKind::Bool ||
                !bc.constants[0].bool_value || bc.constants[1].kind != ConstantKind::Bool ||
                bc.constants[1].bool_value || bc.constants[2].kind != ConstantKind::Null) {
                fail_test("bool and null constants", "unexpected constants");
            } else {
                pass_test("bool and null constants");
            }
        }

        {
            const Bytecode bc = compile("x = 10");
            if (bc.variables.size() != 1 || bc.variables[0].name != "x" || bc.instructions[1].opcode != Opcode::StoreVar ||
                bc.instructions[1].a != 0 || bc.instructions[1].b != bc.instructions[0].a) {
                fail_test("variable assignment", "STORE_VAR should write r0 into v0");
            } else {
                pass_test("variable assignment");
            }
        }

        {
            const Bytecode bc = compile("x = 10 print x");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::StoreVar, Opcode::LoadVar, Opcode::Print,
                                    Opcode::Halt}) ||
                bc.instructions[1].a != bc.instructions[2].b ||
                bc.instructions[3].a != bc.instructions[2].a) {
                fail_test("variable load", "LOAD_VAR/PRINT registers mismatch");
            } else {
                pass_test("variable load");
            }
        }

        {
            const Bytecode bc = compile("a = 1 x = -a");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::StoreVar, Opcode::LoadVar, Opcode::UnaryMinus,
                                    Opcode::StoreVar, Opcode::Halt}) ||
                bc.instructions[3].b != bc.instructions[2].a ||
                bc.instructions[4].b != bc.instructions[3].a) {
                fail_test("unary operations", "unexpected unary registers");
            } else {
                pass_test("unary operations");
            }
        }

        {
            const Bytecode bc = compile("x = 1 + 2 * 3");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::LoadConst, Opcode::LoadConst, Opcode::Mul,
                                    Opcode::Add, Opcode::StoreVar, Opcode::Halt})) {
                fail_test("arithmetic", "expected MUL then ADD");
            } else {
                const Instruction& mul = bc.instructions[3];
                const Instruction& add = bc.instructions[4];
                if (mul.b != bc.instructions[1].a || mul.c != bc.instructions[2].a || add.b != bc.instructions[0].a ||
                    add.c != mul.a) {
                    fail_test("arithmetic", "register operands incorrect");
                } else {
                    pass_test("arithmetic");
                }
            }
        }

        {
            const Bytecode bc = compile("x = 1 + 2 == 3");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::LoadConst, Opcode::Add, Opcode::LoadConst,
                                    Opcode::Equal, Opcode::StoreVar, Opcode::Halt}) ||
                bc.instructions[4].b != bc.instructions[2].a ||
                bc.instructions[4].c != bc.instructions[3].a) {
                fail_test("comparison", "expected ADD then EQ with those registers");
            } else {
                pass_test("comparison");
            }
        }

        {
            const Bytecode bc = compile("print(\"Hello\")");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::Print, Opcode::Halt}) ||
                bc.constants[0].lexeme != "\"Hello\"" || bc.instructions[1].a != bc.instructions[0].a) {
                fail_test("print", "unexpected print bytecode");
            } else {
                pass_test("print");
            }
        }

        {
            const Bytecode bc = compile("print \"Hello\"");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::Print, Opcode::Halt}) ||
                bc.instructions[1].a != 0) {
                fail_test("print without parentheses", "expected PRINT r0");
            } else {
                pass_test("print without parentheses");
            }
        }

        {
            const Bytecode bc = compile("print(\"A\", 1, true)");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::LoadConst, Opcode::LoadConst, Opcode::PrintN,
                                    Opcode::Halt}) ||
                bc.instructions[3].a != 0 || bc.instructions[3].b != 3 || bc.register_count != 3 ||
                bc.constants.size() != 3) {
                fail_test("multi-argument print", "expected PRINT_N r0, 3");
            } else {
                pass_test("multi-argument print");
            }
        }

        {
            const Bytecode bc = compile("print \"A\", 1, true");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::LoadConst, Opcode::LoadConst, Opcode::PrintN,
                                    Opcode::Halt}) ||
                bc.instructions[3].b != 3) {
                fail_test("multi-argument print bare", "expected PRINT_N");
            } else {
                pass_test("multi-argument print bare");
            }
        }

        {
            const Bytecode bc = compile("int x = 10 + 20");
            if (bc.variables.size() != 1 || !bc.variables[0].type_name || *bc.variables[0].type_name != "int" ||
                !opcodes_equal(bc, {Opcode::LoadConst, Opcode::LoadConst, Opcode::Add, Opcode::StoreVar,
                                    Opcode::Halt})) {
                fail_test("typed expression assignment", "unexpected bytecode");
            } else {
                pass_test("typed expression assignment");
            }
        }

        {
            const Bytecode bc = compile("x: float = 1.5");
            if (!bc.variables[0].type_name || *bc.variables[0].type_name != "float" ||
                bc.constants[0].kind != ConstantKind::Float) {
                fail_test("colon typed assignment bytecode", "unexpected type metadata");
            } else {
                pass_test("colon typed assignment bytecode");
            }
        }

        {
            const Bytecode bc = compile("x = 10 x = \"hello\"");
            if (bc.variables.size() != 1 || bc.variables[0].type_name) {
                fail_test("inferred reassignment", "inferred variables must stay untyped");
            } else {
                pass_test("inferred reassignment");
            }
        }

        {
            const Bytecode bc = compile("x = 1 + 2 print x");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::LoadConst, Opcode::Add, Opcode::StoreVar,
                                    Opcode::LoadVar, Opcode::Print, Opcode::Halt})) {
                fail_test("instruction order", "unexpected sequence");
            } else {
                pass_test("instruction order");
            }
        }

        {
            const Bytecode bc = compile("int x = 10 y = x");
            if (bc.variables.size() != 2 || bc.variables[0].name != "x" || !bc.variables[0].type_name ||
                *bc.variables[0].type_name != "int" || bc.variables[1].name != "y" ||
                bc.instructions[1].a != 0 || bc.instructions[2].b != 0 || bc.instructions[3].a != 1) {
                fail_test("variable names/operands", "unexpected slots or type");
            } else {
                pass_test("variable names/operands");
            }
        }

        {
            const Bytecode bc = compile("x = 1 + 2");
            if (bc.register_count != 3 || bc.instructions[0].a != 0 || bc.instructions[1].a != 1 ||
                bc.instructions[2].a != 2 || bc.instructions[2].b != 0 || bc.instructions[2].c != 1) {
                fail_test("register operands", "expected r0, r1, ADD r2, r0, r1");
            } else {
                pass_test("register operands");
            }
        }

        {
            const Bytecode bc = compile("x: int");
            if (bc.variables.size() != 1 || !bc.variables[0].type_name || *bc.variables[0].type_name != "int" ||
                !opcodes_equal(bc, {Opcode::LoadConst, Opcode::StoreVar, Opcode::Halt}) ||
                bc.constants.size() != 1 || bc.constants[0].kind != ConstantKind::Null) {
                fail_test("type annotation declaration", "expected null initializer");
            } else {
                pass_test("type annotation declaration");
            }
        }

        {
            const Bytecode bc = compile("x = 1 + 2");
            const std::string text = bc.disassemble();
            if (text.find("LOAD_CONST r0") == std::string::npos || text.find("ADD r2, r0, r1") == std::string::npos ||
                text.find("STORE_VAR v0, r2") == std::string::npos || text.find("HALT") == std::string::npos) {
                fail_test("disassembly", text);
            } else {
                pass_test("disassembly");
            }
        }

        {
            Bytecode bc;
            bc.register_count = 1;
            bc.instructions.push_back(Instruction{Opcode::LoadConst, 0, 0, 0});
            const auto err = bc.validate();
            if (!err || *err != "invalid constant index") {
                fail_test("invalid constant index", err ? *err : "no error");
            } else {
                pass_test("invalid constant index");
            }
        }

        {
            Bytecode bc;
            bc.register_count = 1;
            bc.instructions.push_back(Instruction{Opcode::Print, 3, 0, 0});
            const auto err = bc.validate();
            if (!err || *err != "invalid register") {
                fail_test("invalid register", err ? *err : "no error");
            } else {
                pass_test("invalid register");
            }
        }

        {
            Bytecode bc;
            bc.register_count = 1;
            bc.instructions.push_back(Instruction{Opcode::LoadVar, 0, 0, 0});
            const auto err = bc.validate();
            if (!err || *err != "invalid variable index") {
                fail_test("invalid variable index", err ? *err : "no error");
            } else {
                pass_test("invalid variable index");
            }
        }

        {
            const Bytecode bc = compile("if true:\n    print(\"yes\")");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::JumpIfFalse, Opcode::LoadConst, Opcode::Print,
                                    Opcode::Halt}) ||
                bc.instructions[1].a != bc.instructions[0].a || bc.instructions[1].b != 4) {
                fail_test("simple if", "expected JUMP_IF_FALSE to HALT");
            } else {
                pass_test("simple if");
            }
        }

        {
            const Bytecode bc = compile("if false:\n    print(\"no\")");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::JumpIfFalse, Opcode::LoadConst, Opcode::Print,
                                    Opcode::Halt}) ||
                bc.instructions[1].b != 4) {
                fail_test("if false", "unexpected jump target");
            } else {
                pass_test("if false");
            }
        }

        {
            const Bytecode bc = compile("if false:\n    print(\"no\")\nelse:\n    print(\"yes\")");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::JumpIfFalse, Opcode::LoadConst, Opcode::Print,
                                    Opcode::Jump, Opcode::LoadConst, Opcode::Print, Opcode::Halt}) ||
                bc.instructions[1].b != 5 || bc.instructions[4].a != 7) {
                fail_test("if else", "JUMP_IF_FALSE should skip to else; JUMP should skip to HALT");
            } else {
                pass_test("if else");
            }
        }

        {
            const Bytecode bc = compile("if false:\n    print(\"no\")\nelif true:\n    print(\"yes\")");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::JumpIfFalse, Opcode::LoadConst, Opcode::Print,
                                    Opcode::Jump, Opcode::LoadConst, Opcode::JumpIfFalse, Opcode::LoadConst,
                                    Opcode::Print, Opcode::Halt}) ||
                bc.instructions[1].b != 5 || bc.instructions[4].a != 9 || bc.instructions[6].b != 9) {
                fail_test("if elif", "unexpected elif jump layout");
            } else {
                pass_test("if elif");
            }
        }

        {
            const Bytecode bc = compile("if false:\n    print(\"no\")\nelseif true:\n    print(\"yes\")");
            const Bytecode elif_bc = compile("if false:\n    print(\"no\")\nelif true:\n    print(\"yes\")");
            if (bc.instructions.size() != elif_bc.instructions.size()) {
                fail_test("if elseif", "elseif should match elif opcode count");
            } else {
                bool same = true;
                for (std::size_t i = 0; i < bc.instructions.size(); ++i) {
                    if (bc.instructions[i].opcode != elif_bc.instructions[i].opcode ||
                        bc.instructions[i].a != elif_bc.instructions[i].a ||
                        bc.instructions[i].b != elif_bc.instructions[i].b) {
                        same = false;
                        break;
                    }
                }
                if (!same) {
                    fail_test("if elseif", "elseif should compile like elif");
                } else {
                    pass_test("if elseif");
                }
            }
        }

        {
            const Bytecode bc =
                compile("if false:\n    print(\"a\")\nelif false:\n    print(\"b\")\nelse:\n    print(\"c\")");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::JumpIfFalse, Opcode::LoadConst, Opcode::Print,
                                    Opcode::Jump, Opcode::LoadConst, Opcode::JumpIfFalse, Opcode::LoadConst,
                                    Opcode::Print, Opcode::Jump, Opcode::LoadConst, Opcode::Print, Opcode::Halt}) ||
                bc.instructions[1].b != 5 || bc.instructions[4].a != 12 || bc.instructions[6].b != 10 ||
                bc.instructions[9].a != 12) {
                fail_test("if elif else", "unexpected jump targets");
            } else {
                pass_test("if elif else");
            }
        }

        {
            const Bytecode bc = compile("if true:\n    if false:\n        print(\"a\")\n    else:\n        print(\"b\")");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::JumpIfFalse, Opcode::LoadConst, Opcode::JumpIfFalse,
                                    Opcode::LoadConst, Opcode::Print, Opcode::Jump, Opcode::LoadConst, Opcode::Print,
                                    Opcode::Halt}) ||
                bc.instructions[1].b != 9 || bc.instructions[3].b != 7 || bc.instructions[6].a != 9) {
                fail_test("nested if", "inner jumps must not clobber the outer target");
            } else {
                pass_test("nested if");
            }
        }

        {
            const char* sources[] = {
                "if 10:\n    print(\"yes\")",
                "if 0:\n    print(\"no\")",
                "if \"hello\":\n    print(\"yes\")",
                "if \"\":\n    print(\"no\")",
                "if null:\n    print(\"no\")",
            };
            bool ok = true;
            for (const char* source : sources) {
                const Bytecode bc = compile(source);
                if (bc.instructions.size() < 2 || bc.instructions[1].opcode != Opcode::JumpIfFalse) {
                    fail_test("truthy condition compile", source);
                    ok = false;
                    break;
                }
            }
            if (ok) {
                pass_test("truthy condition compile");
            }
        }

        {
            const char* sources[] = {
                "a = 2\nb = 1\nif (a > b):\n    print(\"if\")",
                "a = 1\nif false:\n    print(\"if\")\nelif ((a - 5) / 3):\n    print(\"elif\")",
                "a = 10\nb = 1\nif false:\n    print(\"if\")\nelseif (a + b * 2) > 10:\n    print(\"elseif\")",
                "if 1 + 2:\n    print(\"if\")",
                "if 0:\n    print(\"if\")\nelif 3 * 4:\n    print(\"elif\")",
                "if 0:\n    print(\"if\")\nelseif -2:\n    print(\"elseif\")",
            };
            bool ok = true;
            for (const char* source : sources) {
                const Bytecode bc = compile(source);
                bool found_jump = false;
                for (const auto& inst : bc.instructions) {
                    if (inst.opcode == Opcode::JumpIfFalse) {
                        found_jump = true;
                        break;
                    }
                }
                if (!found_jump) {
                    fail_test("condition expression compile", source);
                    ok = false;
                    break;
                }
            }
            if (ok) {
                pass_test("condition expression compile");
            }
        }

        {
            const std::string text = compile("if true:\n    print(\"yes\")").disassemble();
            if (text.find("JUMP_IF_FALSE r0, 4") == std::string::npos) {
                fail_test("jump disassembly", text);
            } else {
                pass_test("jump disassembly");
            }
        }

        {
            const Bytecode bc = compile("numbers = [10, 20, 30]");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::LoadConst, Opcode::LoadConst, Opcode::MakeList,
                                    Opcode::StoreVar, Opcode::Halt}) ||
                bc.instructions[3].c != 3) {
                fail_test("list literal compile", "expected MAKE_LIST count 3");
            } else {
                pass_test("list literal compile");
            }
        }

        {
            const Bytecode bc = compile("player = {\"name\": \"Emir\"}");
            if (!opcodes_equal(bc, {Opcode::LoadConst, Opcode::LoadConst, Opcode::MakeDict, Opcode::StoreVar,
                                    Opcode::Halt}) ||
                bc.instructions[2].c != 1) {
                fail_test("dict literal compile", "expected MAKE_DICT pair count 1");
            } else {
                pass_test("dict literal compile");
            }
        }

        {
            const Bytecode bc = compile("x = [] y = {}");
            if (!opcodes_equal(bc, {Opcode::MakeList, Opcode::StoreVar, Opcode::MakeDict, Opcode::StoreVar,
                                    Opcode::Halt})) {
                fail_test("empty collections compile", "expected MAKE_LIST then MAKE_DICT");
            } else {
                pass_test("empty collections compile");
            }
        }

        {
            const Bytecode bc = compile("t = ()\nu = (1, 2)");
            bool saw_empty = false;
            bool saw_pair = false;
            for (const auto& ins : bc.instructions) {
                if (ins.opcode == Opcode::MakeTuple && ins.c == 0) {
                    saw_empty = true;
                }
                if (ins.opcode == Opcode::MakeTuple && ins.c == 2) {
                    saw_pair = true;
                }
            }
            if (!saw_empty || !saw_pair) {
                fail_test("tuple compile", "expected MAKE_TUPLE");
            } else {
                pass_test("tuple compile");
            }
        }

        {
            const Bytecode bc = compile("x = [1] print(x[0])");
            if (bc.instructions.size() < 4 || bc.instructions[bc.instructions.size() - 3].opcode != Opcode::GetIndex) {
                fail_test("index read compile", "expected GET_INDEX");
            } else {
                pass_test("index read compile");
            }
        }

        {
            const Bytecode bc = compile("x = [1] x[0] = 2");
            if (bc.instructions.empty() || bc.instructions[bc.instructions.size() - 2].opcode != Opcode::SetIndex) {
                fail_test("index write compile", "expected SET_INDEX");
            } else {
                pass_test("index write compile");
            }
        }

        {
            const Bytecode bc = compile("x = [1, 2]\nx.append(3)");
            bool found = false;
            for (const auto& ins : bc.instructions) {
                if (ins.opcode == Opcode::CallMethod) {
                    found = true;
                    break;
                }
            }
            if (!found || bc.calls.empty() || bc.calls[0].name != "append") {
                fail_test("method call compile", "expected CALL_METHOD append");
            } else {
                pass_test("method call compile");
            }
        }

        {
            const Bytecode bc = compile("x = 0\nwhile x < 1:\n    x = 1");
            bool has_jump = false;
            bool has_false = false;
            for (const auto& ins : bc.instructions) {
                if (ins.opcode == Opcode::Jump) {
                    has_jump = true;
                }
                if (ins.opcode == Opcode::JumpIfFalse) {
                    has_false = true;
                }
            }
            if (!has_jump || !has_false) {
                fail_test("while compile", "expected JUMP and JUMP_IF_FALSE");
            } else {
                pass_test("while compile");
            }
        }

        {
            const Bytecode bc = compile("for x in [1]:\n    print(x)");
            bool has_get_iter = false;
            bool has_iter_next = false;
            for (const auto& ins : bc.instructions) {
                if (ins.opcode == Opcode::GetIter) {
                    has_get_iter = true;
                }
                if (ins.opcode == Opcode::IterNext) {
                    has_iter_next = true;
                }
            }
            if (!has_get_iter || !has_iter_next) {
                fail_test("for compile", "expected GET_ITER and ITER_NEXT");
            } else {
                pass_test("for compile");
            }
        }

        {
            const Bytecode bc = compile("x = range(10)");
            bool found = false;
            for (const auto& ins : bc.instructions) {
                if (ins.opcode == Opcode::CallBuiltin) {
                    found = true;
                    break;
                }
            }
            if (!found || bc.calls.empty() || bc.calls[0].name != "range") {
                fail_test("range compile", "expected CALL_BUILTIN range");
            } else {
                pass_test("range compile");
            }
        }

    expect_error("undefined variable", "print x", "undefined variable", 1, 7);
    expect_error("duplicate declaration", "int x int x", "duplicate declaration", 1, 7);
    expect_error("duplicate declaration inferred then typed", "x = 1\nint x = 2\n", "duplicate declaration", 2, 1);
    expect_error("type error string to int", "int x = \"hello\"", "Type error: expected int, got string", 1, 9);
    expect_error("type error bool to float", "float x = true", "Type error: expected float, got bool", 1, 11);
    expect_error("type error int to string", "string x = 10", "Type error: expected string, got int", 1, 12);
    expect_error("type error string to bool", "bool x = \"true\"", "Type error: expected bool, got string", 1, 10);
    expect_error("typed reassignment", "int x = 10 x = \"hello\"", "Type error: expected int, got string", 1, 16);
    expect_error("uninitialized then wrong type", "int score score = \"hello\"", "Type error: expected int, got string",
                 1, 19);
    expect_error("list to int", "int x = [1]", "Type error: expected int, got list", 1, 9);
    expect_error("dict to string", "string name = {}", "Type error: expected string, got map", 1, 15);
    expect_error("unknown type", "foo x = 1", "Type error: unknown type", 1, 1);
    expect_error("float to int rejection", "int x = 1.5", "Type error: expected int, got float", 1, 9);
    expect_error("generic list mismatch", "list<int> x = [\"a\"]", "Type error: expected list<int>, got list<string>",
                 1, 15);
    expect_error("generic map value mismatch", "map<string, int> x = {\"a\": \"1\"}",
                 "Type error: expected map<string,int>, got map<string,string>", 1, 22);

        {
            const Bytecode bc = compile("list<int> values = [1, 2]");
            if (bc.variables.size() != 1 || !bc.variables[0].type_name || *bc.variables[0].type_name != "list<int>") {
                fail_test("generic list variable metadata", "unexpected type name");
            } else {
                pass_test("generic list variable metadata");
            }
        }

        {
            const Bytecode bc = compile("float y = 10\n");
            bool saw_coerce = false;
            for (const Instruction& ins : bc.instructions) {
                if (ins.opcode == Opcode::CoerceToFloat) {
                    saw_coerce = true;
                }
            }
            if (!saw_coerce) {
                fail_test("int to float coerce bytecode", "expected COERCE_TO_FLOAT");
            } else {
                pass_test("int to float coerce bytecode");
            }
        }
    expect_error("break outside loop", "break", "break outside loop", 1, 1);
    expect_error("continue outside loop", "continue", "continue outside loop", 1, 1);
    expect_error("unknown function", "foo()", "unknown function", 1, 1);
    expect_error("catch binding scope", "try:\n    throw 1\ncatch error:\n    pass\nprint(error)\n",
                 "undefined variable", 5, 7);
    {
        const Bytecode bc = compile("try:\n    throw \"x\"\ncatch error:\n    print(error)\n");
        bool saw_enter = false;
        bool saw_throw = false;
        bool saw_leave = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::EnterTry) {
                saw_enter = true;
            }
            if (ins.opcode == Opcode::Throw) {
                saw_throw = true;
            }
            if (ins.opcode == Opcode::LeaveTry) {
                saw_leave = true;
            }
        }
        if (!saw_enter || !saw_throw || !saw_leave || bc.handlers.empty()) {
            fail_test("try catch bytecode", "missing exception opcodes");
        } else {
            pass_test("try catch bytecode");
        }
    }
    {
        const Bytecode bc = compile_with_builtins(
            "class ChildError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
            "try:\n    throw ChildError(\"x\", null)\ncatch ChildError error:\n    pass\ncatch Exception error:\n    pass\n");
        if (bc.handlers.empty() || bc.handlers[0].catches.size() != 2 ||
            bc.handlers[0].catches[0].catch_class_index == 0xFFFFFFFFu ||
            bc.handlers[0].catches[1].catch_class_index == 0xFFFFFFFFu) {
            fail_test("typed catch bytecode", "missing catch class indices");
        } else {
            pass_test("typed catch bytecode");
        }
    }
    expect_error("unknown catch type", "try:\n    throw 1\ncatch Missing error:\n    pass\n", "unknown catch type", 3,
                 1);
    {
        try {
            compile_with_builtins(
                "class ChildError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                "try:\n    throw ChildError(\"x\", null)\ncatch Exception error:\n    pass\ncatch ChildError error:\n    pass\n");
            fail_test("unreachable catch clause", "expected compiler error");
        } catch (const CompilerError& err) {
            if (err.message().find("unreachable catch clause") == std::string::npos) {
                fail_test("unreachable catch clause", err.message());
            } else {
                pass_test("unreachable catch clause");
            }
        } catch (const std::exception& ex) {
            fail_test("unreachable catch clause", ex.what());
        }
    }
    expect_error("unreachable match case",
                 "match 1:\n    _:\n        print(1)\n    2:\n        print(2)\n", "unreachable match case", 4, 5);
    expect_error("match binding does not leak",
                 "match 1:\n    value if value == 1:\n        print(value)\nprint(value)\n", "undefined variable", 4,
                 7);

        {
            const Bytecode bc = compile("x = 10\nx += 2\n");
            bool saw_add = false;
            bool saw_store = false;
            for (const Instruction& ins : bc.instructions) {
                if (ins.opcode == Opcode::Add) {
                    saw_add = true;
                }
                if (ins.opcode == Opcode::StoreVar) {
                    saw_store = true;
                }
            }
            if (!saw_add || !saw_store) {
                fail_test("compound assignment bytecode", "expected ADD and STORE_VAR");
            } else {
                pass_test("compound assignment bytecode");
            }
        }

        {
            const Bytecode bc = compile("x = 1\nx++\n");
            bool saw_add = false;
            for (const Instruction& ins : bc.instructions) {
                if (ins.opcode == Opcode::Add) {
                    saw_add = true;
                }
            }
            if (!saw_add) {
                fail_test("increment bytecode", "expected ADD");
            } else {
                pass_test("increment bytecode");
            }
        }

        {
            const Bytecode bc = compile("x = 5 & 3");
            bool saw_and = false;
            for (const Instruction& ins : bc.instructions) {
                if (ins.opcode == Opcode::BitAnd) {
                    saw_and = true;
                }
            }
            if (!saw_and) {
                fail_test("bitwise bytecode", "expected BIT_AND");
            } else {
                pass_test("bitwise bytecode");
            }
        }

        {
            const Bytecode bc = compile("x = 1 in [1, 2]");
            bool saw = false;
            for (const Instruction& ins : bc.instructions) {
                if (ins.opcode == Opcode::Contains) {
                    saw = true;
                }
            }
            if (!saw) {
                fail_test("membership bytecode", "expected CONTAINS");
            } else {
                pass_test("membership bytecode");
            }
        }

        {
            const Bytecode bc = compile("x = true ? 1 : 2");
            bool saw_false = false;
            bool saw_jump = false;
            for (const Instruction& ins : bc.instructions) {
                if (ins.opcode == Opcode::JumpIfFalse) {
                    saw_false = true;
                }
                if (ins.opcode == Opcode::Jump) {
                    saw_jump = true;
                }
            }
            if (!saw_false || !saw_jump) {
                fail_test("ternary bytecode", "expected JUMP_IF_FALSE and JUMP");
            } else {
                pass_test("ternary bytecode");
            }
        }

    expect_error("local does not leak from if", "if true:\n    y = 1\nprint(y)\n", "undefined variable", 3, 7);
    expect_error("no variable hoisting", "print(x)\nx = 10\n", "undefined variable", 1, 7);
    expect_error("local does not leak from while", "while false:\n    z = 1\nprint(z)\n", "undefined variable", 3, 7);
    expect_error("for item does not leak", "for i in [1]:\n    print(i)\nprint(i)\n", "undefined variable", 3, 7);
    expect_error("match local does not leak",
                 "match 1:\n    1:\n        hidden = 1\nprint(hidden)\n", "undefined variable", 4, 7);
    expect_error("duplicate inner declaration",
                 "int x = 1\nif true:\n    int y = 1\n    int y = 2\n", "duplicate declaration", 4, 5);
    expect_error("wrong argument count",
                 "func add(a, b):\n    return a + b\nadd(10)\n", "'add' expects 2 arguments", 3, 1);
    expect_error("too many arguments",
                 "func add(a, b):\n    return a + b\nadd(10, 20, 30)\n", "'add' expects 2 arguments", 3, 1);
    expect_error("duplicate function",
                 "func test():\n    return 1\nfunc test():\n    return 2\n", "duplicate function signature", 3, 1);
    expect_error("local does not leak from function",
                 "func test():\n    x = 10\n    print(x)\ntest()\nprint(x)\n", "undefined variable", 5, 7);
    expect_error("no variable hoisting still", "print(x)\nx = 10\n", "undefined variable", 1, 7);
    expect_error("wrong argument count still",
                 "func add(a, b):\n    a + b\nadd(1, 2, 3)\n", "'add' expects 2 arguments", 3, 1);

    {
        const Bytecode bc = compile("func add(a, b):\n    return a + b\nresult = add(10, 20)\n");
        bool saw_call = false;
        bool saw_return = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::Call) {
                saw_call = true;
            }
            if (ins.opcode == Opcode::Return) {
                saw_return = true;
            }
        }
        const FunctionInfo* add_fn = find_user_function(bc, "add");
        if (!saw_call || !saw_return || count_user_functions(bc) != 1 || !add_fn || add_fn->arity != 2) {
            fail_test("function bytecode", "expected CALL/RETURN and FunctionInfo");
        } else {
            pass_test("function bytecode");
        }
    }

    {
        const Bytecode bc = compile("result = add(10, 20)\nfunc add(a, b):\n    return a + b\n");
        const FunctionInfo* add_fn = find_user_function(bc, "add");
        if (count_user_functions(bc) != 1 || !add_fn || add_fn->entry == 0) {
            fail_test("function hoist bytecode", "expected hoisted function entry after main");
        } else {
            pass_test("function hoist bytecode");
        }
    }

    {
        const Bytecode bc = compile("func add(a, b):\n    a + b\nprint(add(1, 2))\n");
        bool saw_add = false;
        bool saw_return = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::Add) {
                saw_add = true;
            }
            if (ins.opcode == Opcode::Return) {
                saw_return = true;
            }
        }
        if (!saw_add || !saw_return) {
            fail_test("automatic return bytecode", "expected ADD then RETURN");
        } else {
            pass_test("automatic return bytecode");
        }
    }

    expect_error("duplicate nested function",
                 "func outer():\n    func inner():\n        return 1\n    func inner():\n        return 2\n",
                 "duplicate function signature", 4, 5);
    expect_error("nested undefined variable",
                 "func outer():\n    func inner():\n        return x\n    inner()\n", "undefined variable", 3, 16);
    expect_error("no variable hoisting in nested",
                 "func outer():\n    print(x)\n    x = 10\n", "undefined variable", 2, 11);

    {
        const Bytecode bc =
            compile("func outer():\n    func inner():\n        print(\"hello\")\n    inner()\nouter()\n");
        const FunctionInfo* outer_fn = find_user_function(bc, "outer");
        const FunctionInfo* inner_fn = find_user_function(bc, "inner");
        if (count_user_functions(bc) != 2 || !outer_fn || outer_fn->entry == 0 || outer_fn->local_count < 1 ||
            !inner_fn) {
            fail_test("nested function bytecode", "expected nested function entries and locals");
        } else {
            pass_test("nested function bytecode");
        }
    }

    expect_error("tuple index assign", "t = (1, 2)\nt[0] = 3\n", "tuple is immutable", 2, 1);
    expect_error("set index", "s = {1, 2}\nprint(s[0])\n", "cannot index set", 2, 7);

    {
        const Bytecode bc = compile("struct Player:\n    name\np = Player()\nprint(p.name)\n");
        bool has_make_struct = false;
        bool has_get_field = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::MakeStruct) {
                has_make_struct = true;
            }
            if (ins.opcode == Opcode::GetField) {
                has_get_field = true;
            }
        }
        if (!has_make_struct || !has_get_field || bc.structs.size() != 1 || bc.structs[0].name != "Player") {
            fail_test("struct compile", "expected struct bytecode");
        } else {
            pass_test("struct compile");
        }
    }
    {
        const Bytecode bc = compile("enum Color:\n    RED\nprint(Color.RED)\n");
        bool has_load_enum = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::LoadEnum) {
                has_load_enum = true;
            }
        }
        if (!has_load_enum || bc.enums.size() != 1 || bc.enums[0].members[0] != "RED") {
            fail_test("enum compile", "expected enum bytecode");
        } else {
            pass_test("enum compile");
        }
    }
    expect_error("unknown struct field", "struct Player:\n    name\np = Player()\nprint(p.missing)\n",
                 "unknown field", 4, 7);
    expect_error("duplicate struct type", "struct Player:\n    name\nstruct Player:\n    score\n",
                 "duplicate declaration", 3, 1);
    expect_error("struct constructor args", "struct Player:\n    name\np = Player(1, 2)\n", "'Player' expects 1 arguments", 3, 5);

    {
        const Bytecode bc = compile("struct Player:\n    pub name\n    private secret\np = Player()\nprint(p.name)\n");
        if (bc.structs.empty() || bc.structs[0].fields.size() != 2 ||
            bc.structs[0].fields[0].access != pscript::MemberAccess::Public ||
            bc.structs[0].fields[1].access != pscript::MemberAccess::Private) {
            fail_test("struct access metadata", "expected access in StructFieldInfo");
        } else {
            pass_test("struct access metadata");
        }
    }
    expect_error("private field read", "struct Player:\n    private secret\np = Player()\nprint(p.secret)\n",
                 "cannot access private field", 4, 7);
    expect_error("protected field read", "struct Player:\n    prot secret\np = Player()\nprint(p.secret)\n",
                 "cannot access protected field", 4, 7);
    {
        const Bytecode bc = compile("func noop():\n    pass\n");
        const FunctionInfo* noop_fn = find_user_function(bc, "noop");
        if (!noop_fn || noop_fn->entry == 0) {
            fail_test("pass compiles to no-op", "expected function entry");
        } else {
            pass_test("pass compiles to no-op");
        }
    }

    {
        try {
            compile_with_builtins(
                "async func main():\n    s = AsyncStream([\"a\"])\n    async for c in s:\n        print(c)\n");
            pass_test("async for stream compile");
        } catch (const std::exception& ex) {
            fail_test("async for stream compile", ex.what());
        }
    }

    {
        ModuleTempDir dir;
        dir.write("math.phs", "func add(a, b):\n    return a + b\n");
        CompileOptions options;
        options.source_file = dir.main_path();
        const Bytecode bc = compile(parse("import math\n"), options);
        bool has_import_module = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::ImportModule) {
                has_import_module = true;
            }
        }
        if (!has_import_module || bc.modules.empty()) {
            fail_test("import module bytecode", "expected ImportModule and module descriptor");
        } else {
            pass_test("import module bytecode");
        }
    }
    {
        ModuleTempDir dir;
        dir.write("math.phs", "func add(a, b):\n    return a + b\n");
        CompileOptions options;
        options.source_file = dir.main_path();
        const Bytecode bc = compile(parse("from math import add\nprint(add(1, 2))\n"), options);
        bool has_import_name = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::ImportName) {
                has_import_name = true;
            }
        }
        if (!has_import_name || bc.modules.empty()) {
            fail_test("from import bytecode", "expected ImportName");
        } else {
            pass_test("from import bytecode");
        }
    }
    {
        ModuleTempDir dir;
        dir.write("math.phs", "func add(a, b):\n    return a + b\n");
        CompileOptions options;
        options.source_file = dir.main_path();
        try {
            compile(parse("import math as m\nimport other as m\n"), options);
            fail_test("duplicate module alias", "expected compiler error");
        } catch (const CompilerError& err) {
            if (err.message().find("duplicate declaration") == std::string::npos) {
                fail_test("duplicate module alias", err.message());
            } else {
                pass_test("duplicate module alias");
            }
        }
    }
    {
        ModuleTempDir dir;
        dir.write("math.phs", "func add(a, b):\n    return a + b\n");
        dir.write("other.phs", "func add(a, b):\n    return a - b\n");
        CompileOptions options;
        options.source_file = dir.main_path();
        try {
            compile(parse("from math import add\nfrom other import add\n"), options);
            fail_test("duplicate from import", "expected compiler error");
        } catch (const CompilerError& err) {
            if (err.message().find("duplicate declaration") == std::string::npos) {
                fail_test("duplicate from import", err.message());
            } else {
                pass_test("duplicate from import");
            }
        }
    }
    {
        ModuleTempDir dir;
        dir.write("math.phs", "func add(a, b):\n    return a + b\n");
        CompileOptions options;
        options.source_file = dir.main_path();
        try {
            compile(parse("from math import missing\n"), options);
            fail_test("missing module export", "expected compiler error");
        } catch (const CompilerError& err) {
            if (err.message().find("has no export") == std::string::npos) {
                fail_test("missing module export", err.message());
            } else {
                pass_test("missing module export");
            }
        }
    }
    {
        ModuleTempDir dir;
        dir.write("math.phs", "pub value = 1\npriv secret = 2\n");
        CompileOptions options;
        options.source_file = dir.main_path();
        try {
            compile(parse("from math import secret\n"), options);
            fail_test("private from import", "expected compiler error");
        } catch (const CompilerError& err) {
            if (err.message().find("has no export") == std::string::npos) {
                fail_test("private from import", err.message());
            } else {
                pass_test("private from import");
            }
        }
    }
    {
        ModuleTempDir dir;
        dir.write("math.phs", "prot value = 1\n");
        CompileOptions options;
        options.source_file = dir.main_path();
        try {
            compile(parse("import math\n"), options);
            fail_test("protected module declaration", "expected compiler error");
        } catch (const CompilerError& err) {
            if (err.message().find("protected is not allowed at module level") == std::string::npos) {
                fail_test("protected module declaration", err.message());
            } else {
                pass_test("protected module declaration");
            }
        }
    }

    {
        const Bytecode bc = compile("async func foo():\n    return await delay(1ms)\n");
        const FunctionInfo* foo = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "foo") {
                foo = &fn;
                break;
            }
        }
        if (!foo || !foo->is_async || !foo->returns_value) {
            fail_test("async function metadata", "unexpected function info");
        } else {
            pass_test("async function metadata");
        }
        bool has_await = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::Await) {
                has_await = true;
                break;
            }
        }
        if (!has_await) {
            fail_test("await compilation", "missing AWAIT opcode");
        } else {
            pass_test("await compilation");
        }
    }
    expect_error("await in sync function", "func main():\n    await delay(1ms)\n", "await outside async function", 2, 5);
    {
        const Bytecode bc = compile("await delay(1ms)\n");
        if (bc.async_main_function_index == 0xFFFFFFFFu) {
            fail_test("top-level await compile", "missing async module entry");
        } else {
            pass_test("top-level await compile");
        }
    }
    {
        const Bytecode bc = compile("async func main():\n    return 1\n");
        if (bc.async_main_function_index == 0xFFFFFFFFu) {
            fail_test("async main detection", "async_main_function_index not set");
        } else {
            pass_test("async main detection");
        }
    }
    {
        const Bytecode bc = compile("async func foo():\n    return 1\nasync func main():\n    f = foo\n    t = f()\n    return await t\n");
        const FunctionInfo* foo = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "foo") {
                foo = &fn;
                break;
            }
        }
        if (!foo || !foo->is_async) {
            fail_test("async function value metadata", "missing async flag");
        } else {
            pass_test("async function value metadata");
        }
    }
    expect_error("async constructor compile", "class Foo:\n    async constructor():\n        pass\n",
                  "async constructor is not supported", 2, 5);

    {
        const Bytecode bc = compile("func add(a: int, b: int) -> int:\n    return a + b\n");
        const FunctionInfo* add = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "add") {
                add = &fn;
                break;
            }
        }
        if (!add || !add->return_type_name || *add->return_type_name != "int") {
            fail_test("function return type metadata", "missing -> int on add");
        } else {
            pass_test("function return type metadata");
        }
    }
    expect_error_prefix("invalid function return type",
                        "func bad() -> int:\n    return \"x\"\n",
                        "Type error: expected int, got string");
    expect_error_prefix("function missing return value",
                        "func bad() -> int:\n    pass\n",
                        "function requires a return value");
    {
        const Bytecode bc = compile("class Box:\n    func value() -> int:\n        return 1\n");
        const FunctionInfo* value = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "Box.value") {
                value = &fn;
                break;
            }
        }
        if (!value || !value->return_type_name || *value->return_type_name != "int") {
            fail_test("method return type metadata", "missing method return type");
        } else {
            pass_test("method return type metadata");
        }
    }
    {
        const Bytecode bc = compile("x: int | string = 1\ny: int | string = \"a\"\n");
        if (bc.variables.size() < 2) {
            fail_test("union variable metadata", "expected variables");
        } else {
            pass_test("union variable metadata");
        }
    }
    expect_error_prefix("invalid union assignment compile",
                        "x: int | string = 1\nx = true\n",
                        "Type error: expected int|string, got bool");
    {
        const Bytecode bc = compile("a = true || false\nb: int | string = 1\n");
        if (bc.variables.size() < 2) {
            fail_test("pipe vs logical or", "compile failed");
        } else {
            pass_test("pipe vs logical or");
        }
    }
    expect_error_prefix("const reassignment compile", "const x = 10\nx = 20\n",
                        "cannot assign to const 'x'");
    expect_error_prefix("const compound assign compile", "const x = 10\nx += 1\n",
                        "cannot assign to const 'x'");
    {
        const Bytecode bc = compile("func identity<T>(value: T) -> T:\n    return value\n");
        const FunctionInfo* identity = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "identity") {
                identity = &fn;
                break;
            }
        }
        if (!identity || identity->generic_type_params.empty()) {
            fail_test("generic function metadata", "missing type params");
        } else {
            pass_test("generic function metadata");
        }
    }
    expect_error_prefix("overload ambiguous",
                        "func value<T>(x: T):\n    pass\nfunc value(x: int):\n    pass\nvalue(1)\n",
                        "ambiguous overload");
    {
        compile("func greet(name: string, prefix: string = \"Hello\"):\n    pass\n");
        pass_test("default parameter compile");
    }
    {
        compile("func sum(values: int...):\n    pass\n");
        pass_test("variadic parameter compile");
    }
    {
        const Bytecode bc = compile("interface Printable:\n    func print()\n");
        if (bc.interfaces.empty() || bc.interfaces[0].name != "Printable") {
            fail_test("interface declaration metadata", "missing interface info");
        } else {
            pass_test("interface declaration metadata");
        }
    }
    {
        compile("interface Printable:\n    func print()\nclass Player : Printable:\n    func print():\n        pass\n");
        pass_test("class implements interface compile");
    }
    expect_error_prefix("missing interface method",
                        "interface Printable:\n    func print()\nclass Player : Printable:\n    pass\n",
                        "class 'Player' does not implement interface method");
    expect_error_prefix("generic constraint compile",
                        "interface Printable:\n    func print()\nclass Player : Printable:\n    func print():\n        pass\n"
                        "func show<T: Printable>(value: T):\n    pass\nshow(10)\n",
                        "type argument does not satisfy generic constraint");
    {
        compile("class Box<T>:\n    value: T\n");
        pass_test("generic class compile");
    }
    {
        const Bytecode bc = compile("@deprecated\nfunc old():\n    pass\n");
        const FunctionInfo* old_fn = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "old") {
                old_fn = &fn;
                break;
            }
        }
        if (!old_fn || old_fn->annotations.empty() || old_fn->annotations[0].name != "deprecated") {
            fail_test("annotation metadata", "missing annotation");
        } else {
            pass_test("annotation metadata");
        }
    }
    {
        const Bytecode bc = compile("@deprecated(\"use_new\")\nfunc old():\n    pass\n");
        const FunctionInfo* old_fn = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "old") {
                old_fn = &fn;
                break;
            }
        }
        if (!old_fn || old_fn->annotations.empty() || old_fn->annotations[0].name != "deprecated" ||
            old_fn->annotations[0].args.empty() || old_fn->annotations[0].args[0].second != "\"use_new\"") {
            fail_test("annotation argument metadata", "missing annotation args");
        } else {
            pass_test("annotation argument metadata");
        }
    }
    {
        const Bytecode bc = compile("func f(@deprecated x: int):\n    pass\n");
        const FunctionInfo* f = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "f") {
                f = &fn;
                break;
            }
        }
        if (!f || f->parameter_annotations.size() != 1 || f->parameter_annotations[0].empty() ||
            f->parameter_annotations[0][0].name != "deprecated") {
            fail_test("parameter annotation metadata", "missing parameter annotation");
        } else {
            pass_test("parameter annotation metadata");
        }
    }
    {
        const Bytecode bc = compile("async func get_n() -> int:\n    return 10\n");
        const FunctionInfo* get_n = nullptr;
        for (const FunctionInfo& fn : bc.functions) {
            if (fn.name == "get_n") {
                get_n = &fn;
                break;
            }
        }
        if (!get_n || !get_n->return_type_name || *get_n->return_type_name != "int" || !get_n->is_async) {
            fail_test("async return type metadata", "unexpected async metadata");
        } else {
            pass_test("async return type metadata");
        }
    }

    expect_error("delay bare integer", "async func main():\n    await delay(10)\n", "expected a duration literal", 2, 17);
    expect_error("delay must be awaited stmt", "async func main():\n    delay(1ms)\n", "'delay' must be awaited", 2, 5);
    expect_error("duration literal assignment", "async func main():\n    x = 1s\n", "duration literal not allowed", 2, 9);
    {
        const Bytecode bc = compile("async func main():\n    await delay(1.5s)\n    await delay(500ms)\n");
        bool has_delay = false;
        for (const CallInfo& call : bc.calls) {
            if (call.name == "delay") {
                has_delay = true;
            }
        }
        if (!has_delay) {
            fail_test("duration delay compile", "missing delay call metadata");
        } else {
            pass_test("duration delay compile");
        }
    }
    {
        const Bytecode bc = compile("str name = \"x\"\nstring title = \"y\"\n");
        (void)bc;
        pass_test("str string type alias");
    }
    {
        const Bytecode bc = compile("typealias UserId = int\nfunc id() -> UserId:\n    return 1\n");
        if (bc.type_aliases.size() != 1 || bc.type_aliases[0].name != "UserId") {
            fail_test("typealias declaration", "missing alias metadata");
        } else {
            pass_test("typealias declaration");
        }
    }
    {
        const Bytecode bc = compile("enum Status:\n    @deprecated(\"legacy\")\n    Ok\n    Err\n");
        if (bc.enums.empty() || bc.enums[0].member_annotations.size() < 1 ||
            bc.enums[0].member_annotations[0].empty()) {
            fail_test("enum member annotations", "missing member annotation metadata");
        } else {
            pass_test("enum member annotations");
        }
    }
    {
        const Bytecode bc = compile("@deprecated(\"old\")\nenum Status:\n    Ok\n    Err\n");
        if (bc.enums.empty() || bc.enums[0].annotations.empty()) {
            fail_test("enum annotations", "missing annotation metadata");
        } else {
            pass_test("enum annotations");
        }
    }
    {
        const Bytecode bc = compile("@deprecated(\"use new_api\")\nfunc old_api():\n    return 1\nfunc main():\n    return old_api()\n");
        if (bc.warnings.empty()) {
            fail_test("deprecated usage warning", "no compile warning");
        } else {
            pass_test("deprecated usage warning");
        }
    }
    {
        bool threw = false;
        try {
            (void)compile("x ??= 1\n");
        } catch (const ParserError&) {
            threw = true;
        } catch (const LexerError&) {
            threw = true;
        }
        if (!threw) {
            fail_test("reject null coalesce assign", "compiled");
        } else {
            pass_test("reject null coalesce assign");
        }
    }

    {
        const char* source =
            "class Mgr:\n    func enter():\n        return this\n    func exit(exc):\n        return null\n"
            "async func main():\n    async with Mgr():\n        pass\n";
        const Bytecode bc = compile_with_builtins(source);
        bool pending_load = false;
        for (const Instruction& ins : bc.instructions) {
            if (ins.opcode == Opcode::LoadPendingException) {
                pending_load = true;
                break;
            }
        }
        if (!pending_load) {
            fail_test("async with pending exception load", "missing LOAD_PENDING_EXCEPTION");
        } else {
            pass_test("async with pending exception load");
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All compiler tests passed\n";
    return 0;
    } catch (const std::exception& ex) {
        std::cerr << "FAIL  compiler tests: " << ex.what() << '\n';
        return 1;
    }
}
