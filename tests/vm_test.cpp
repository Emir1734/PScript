#include "compiler/compiler.h"
#include "parser/parser.h"
#include "runtime/runtime.h"
#include "stdlib/native_boundary.h"
#include "vm/vm.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
using pscript::CompileOptions;
using pscript::CompilerError;
using pscript::compile;
using pscript::compile_with_builtins;
using pscript::parse;
using pscript::Constant;
using pscript::ConstantKind;
using pscript::Instruction;
using pscript::Opcode;
using pscript::Runtime;
using pscript::Vm;
using pscript::VmError;

namespace {

int g_failed = 0;

std::uint32_t test_process_id() {
#ifdef _WIN32
    return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(getpid());
#endif
}

std::filesystem::path unique_test_run_dir(std::string_view prefix, std::atomic<int>& counter) {
    const int run = ++counter;
    return std::filesystem::temp_directory_path() / (std::string(prefix) + "-pid" +
                                                      std::to_string(test_process_id()) + "-run" + std::to_string(run));
}

void write_test_file(const std::filesystem::path& file, std::string_view contents) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create test file: " + file.string());
    }
    out << contents;
    out.flush();
    if (!out) {
        throw std::runtime_error("failed to write test file: " + file.string());
    }
}

class CwdScope {
public:
    explicit CwdScope(const std::filesystem::path& target) {
        previous_ = std::filesystem::current_path();
        std::error_code ec;
        std::filesystem::current_path(target, ec);
        if (ec) {
            throw std::runtime_error("failed to set test cwd to " + target.string() + ": " + ec.message());
        }
        active_ = true;
    }

    ~CwdScope() {
        restore();
    }

    CwdScope(const CwdScope&) = delete;
    CwdScope& operator=(const CwdScope&) = delete;

private:
    void restore() {
        if (!active_) {
            return;
        }
        std::error_code ec;
        std::filesystem::current_path(previous_, ec);
        active_ = false;
    }

    std::filesystem::path previous_;
    bool active_ = false;
};

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
    std::cout.flush();
}

class ModuleTempDir {
public:
    ModuleTempDir() {
        path_ = unique_test_run_dir("pscript_vm_module_tests", counter_);
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            throw std::runtime_error("failed to create module test directory: " + path_.string() + ": " + ec.message());
        }
    }

    ~ModuleTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    void write(const std::string& name, std::string_view contents) const {
        write_test_file(path_ / name, contents);
    }

    std::string main_path() const { return (path_ / "main.phs").string(); }

    const std::filesystem::path& path() const { return path_; }

private:
    static inline std::atomic<int> counter_{0};
    std::filesystem::path path_;
};

std::string run_source(const std::string& source) {
    std::ostringstream out;
    Vm vm(compile(source), out);
    vm.run();
    return out.str();
}

std::string run_modules(const std::string& main_source,
                        const std::vector<std::pair<std::string, std::string>>& files) {
    ModuleTempDir dir;
    for (const auto& [name, contents] : files) {
        dir.write(name, contents);
    }
    dir.write("main.phs", main_source);
    CompileOptions options;
    options.source_file = dir.main_path();
    CwdScope cwd(dir.path());
    std::ostringstream out;
    Vm vm(compile(parse(main_source), options), out);
    vm.run();
    return out.str();
}

void expect_module_output(std::string_view name, const std::string& main_source,
                          const std::vector<std::pair<std::string, std::string>>& files,
                          const std::string& expected) {
    try {
        const std::string got = run_modules(main_source, files);
        if (got != expected) {
            fail_test(name, "got \"" + got + "\" expected \"" + expected + "\"");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

std::string run_builtins(const std::string& source) {
    std::ostringstream out;
    Vm vm(compile_with_builtins(source), out);
    vm.run();
    return out.str();
}

std::pair<std::string, std::string> run_builtins_with_stderr(const std::string& source,
                                                             const std::string& source_file = "<stdin>") {
    std::ostringstream out;
    std::ostringstream err;
    Runtime runtime(out, std::cin, err);
    Vm vm(compile_with_builtins(source, source_file), runtime);
    vm.run();
    return {out.str(), err.str()};
}

void expect_log_stderr(std::string_view name, const std::string& source, const std::string& source_file,
                       const std::function<void(const std::string&, const std::string&)>& check) {
    try {
        const auto [stdout_text, stderr_text] = run_builtins_with_stderr(source, source_file);
        check(stdout_text, stderr_text);
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

std::string run_source_with_input(const std::string& source, const std::string& input) {
    std::istringstream in(input);
    std::ostringstream out;
    Runtime runtime(out, in);
    Vm vm(compile(source), runtime);
    vm.run();
    return out.str();
}

std::string run_async_source_with_input(const std::string& source, const std::string& input) {
    std::istringstream in(input);
    std::ostringstream out;
    Runtime runtime(out, in);
    Vm vm(compile_with_builtins(source), runtime);
    vm.run();
    return out.str();
}

void expect_output(std::string_view name, const std::string& source, const std::string& expected) {
    try {
        const std::string got = run_source(source);
        if (got != expected) {
            fail_test(name, "got \"" + got + "\" expected \"" + expected + "\"");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_builtin_output(std::string_view name, const std::string& source, const std::string& expected) {
    try {
        const std::string got = run_builtins(source);
        if (got != expected) {
            fail_test(name, "got \"" + got + "\" expected \"" + expected + "\"");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_vm_error(std::string_view name, Bytecode bytecode, std::string_view message) {
    try {
        std::ostringstream out;
        Vm vm(std::move(bytecode), out);
        vm.run();
        fail_test(name, "expected VM error");
    } catch (const VmError& err) {
        if (err.message() != message) {
            fail_test(name, "got \"" + err.message() + "\"");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_unhandled(std::string_view name, Bytecode bytecode, std::string_view substring) {
    try {
        std::ostringstream out;
        Vm vm(std::move(bytecode), out);
        vm.run();
        fail_test(name, "expected unhandled exception");
    } catch (const VmError& err) {
        const std::string& message = err.message();
        if (message.find("Unhandled exception:") == std::string::npos) {
            fail_test(name, "missing unhandled prefix: \"" + message + "\"");
            return;
        }
        if (message.find(substring) == std::string::npos) {
            fail_test(name, "got \"" + message + "\"");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_unhandled_builtins(std::string_view name, const std::string& source, std::string_view substring,
                               const std::string& source_file = "test.phs") {
    expect_unhandled(name, compile_with_builtins(source, source_file), substring);
}

class FsTempDir {
public:
    FsTempDir() {
        path_ = unique_test_run_dir("pscript_vm_fs_tests", counter_);
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            throw std::runtime_error("failed to create fs test directory: " + path_.string() + ": " + ec.message());
        }
        cwd_.emplace(path_);
    }

    ~FsTempDir() {
        cwd_.reset();
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    std::filesystem::path path() const { return path_; }

private:
    static inline std::atomic<int> counter_{0};
    std::filesystem::path path_;
    std::optional<CwdScope> cwd_;
};

std::string phs_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        if (ch == '\\' || ch == '"') {
            out += '\\';
        }
        out += ch;
    }
    return out;
}

std::string run_source_in_fs(const std::string& source) {
    FsTempDir dir;
    std::ostringstream out;
    Vm vm(compile_with_builtins(source), out);
    vm.run();
    return out.str();
}

void expect_fs_output(std::string_view name, const std::string& source, const std::string& expected) {
    try {
        const std::string got = run_source_in_fs(source);
        if (got != expected) {
            fail_test(name, "got \"" + got + "\" expected \"" + expected + "\"");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_fs_unhandled(std::string_view name, const std::string& source, std::string_view substring) {
    FsTempDir dir;
    expect_unhandled(name, compile_with_builtins(source), substring);
}

std::string run_source_with_args(const std::string& source, const std::vector<std::string>& program_args) {
    std::ostringstream out;
    Runtime runtime(out);
    runtime.set_program_args(program_args);
    Vm vm(compile_with_builtins(source), runtime);
    vm.run();
    return out.str();
}

void expect_source_with_args(std::string_view name, const std::string& source,
                             const std::vector<std::string>& program_args, const std::string& expected) {
    try {
        const std::string got = run_source_with_args(source, program_args);
        if (got != expected) {
            fail_test(name, "got \"" + got + "\" expected \"" + expected + "\"");
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_process_exit(std::string_view name, const std::string& source, int expected_code) {
    try {
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(source), runtime);
        vm.run();
        if (!vm.process_exit_requested()) {
            fail_test(name, "expected process exit");
            return;
        }
        if (vm.process_exit_code() != expected_code) {
            fail_test(name, "got exit code " + std::to_string(vm.process_exit_code()));
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

}  // namespace

int main() {
    const CwdScope process_cwd_guard(std::filesystem::current_path());
    expect_output("LOAD_CONST integer", "print 10", "10\n");
    expect_output("LOAD_CONST float", "print 1.5", "1.5\n");
    expect_output("LOAD_CONST string", "print \"hi\"", "hi\n");
    expect_output("utf-8 merhaba", "print(\"Merhaba D\xC3\xBCnya\")", "Merhaba D\xC3\xBCnya\n");
    expect_output("utf-8 turkish letters", "print(\"\xC3\xA7 \xC4\x9F \xC4\xB1 \xC4\xB0 \xC3\xB6 \xC5\x9F \xC3\xBC\")",
                  "\xC3\xA7 \xC4\x9F \xC4\xB1 \xC4\xB0 \xC3\xB6 \xC5\x9F \xC3\xBC\n");
    expect_output("utf-8 grinning emoji", "print(\"\xF0\x9F\x98\x80\")", "\xF0\x9F\x98\x80\n");
    expect_output("utf-8 rocket emoji", "print(\"\xF0\x9F\x9A\x80\")", "\xF0\x9F\x9A\x80\n");
    expect_output("utf-8 red heart", "print(\"\xE2\x9D\xA4\xEF\xB8\x8F\")", "\xE2\x9D\xA4\xEF\xB8\x8F\n");
    expect_output("utf-8 mixed", "print(\"PScript \xC3\xA7" "al\xC4\xB1\xC5\x9F\xC4\xB1yor \xF0\x9F\x9A\x80\")",
                  "PScript \xC3\xA7" "al\xC4\xB1\xC5\x9F\xC4\xB1yor \xF0\x9F\x9A\x80\n");
    expect_output("utf-8 through variable", "name = \"Emir Y\xC4\xB1ld\xC4\xB1z\"\nprint(name)\n",
                  "Emir Y\xC4\xB1ld\xC4\xB1z\n");
    expect_output("LOAD_CONST bool/null", "print true print false print null", "true\nfalse\nnull\n");

    expect_output("STORE_VAR / LOAD_VAR", "x = 10 print x", "10\n");
    expect_output("unary minus", "a = 5 print -a", "-5\n");
    expect_output("unary plus", "a = 5 print +a", "5\n");

    expect_output("add", "print 1 + 2", "3\n");
    expect_output("sub", "print 5 - 2", "3\n");
    expect_output("mul", "print 3 * 4", "12\n");
    expect_output("div", "print 8 / 2", "4\n");
    expect_output("mod", "print 7 % 3", "1\n");
    expect_output("float add", "print 1 + 2.5", "3.5\n");

    expect_output("eq", "print 1 == 1", "true\n");
    expect_output("string eq same contents", "print(\"ab\" == \"ab\")", "true\n");
    expect_output("string neq length", "print(\"ab\" == \"abc\")", "false\n");
    expect_output("string neq same length", "print(\"abc\" == \"abd\")", "false\n");
    expect_output("string eq utf-8", "print(\"\xC3\xA7\" == \"\xC3\xA7\")", "true\n");
    expect_output("string vs int eq", "print(\"1\" == 1)", "false\n");
    expect_output("string empty eq", "print(\"\" == \"\")", "true\n");
    expect_output("ne", "print 1 != 2", "true\n");
    expect_output("lt", "print 1 < 2", "true\n");
    expect_output("le", "print 2 <= 2", "true\n");
    expect_output("gt", "print 3 > 1", "true\n");
    expect_output("ge", "print 3 >= 4", "false\n");

    expect_output("PRINT forms", "print(\"Hello\") print \"World\"", "Hello\nWorld\n");
    expect_output("PRINT multiple", "print(\"X:\", 10, \"Y:\", 20)", "X: 10 Y: 20\n");
    expect_output("PRINT multiple bare", "score = 5 print \"Score:\", score", "Score: 5\n");
    expect_output("bare print grouped div", "x = 10\ny = 4\nprint (x * y) / 2\n", "20\n");
    expect_output("paren print grouped div", "x = 10\ny = 4\nprint((x * y) / 2)\n", "20\n");
    expect_output("bare print mul", "x = 10\ny = 4\nprint x * y\n", "40\n");
    expect_output("bare print add mul", "x = 10\ny = 4\nprint x + y * 2\n", "18\n");
    expect_output("bare print grouped add mul", "x = 10\ny = 4\nprint (x + y) * 2\n", "28\n");
    expect_output("bare print multi full exprs", "x = 10\ny = 4\nprint x + 10, y * 2, (x + y) / 2\n", "20 8 7\n");
    expect_output("bare print comparison", "x = 10\nprint x > 10\nprint x == 10\n", "false\ntrue\n");
    expect_output("instruction order", "x = 1 + 2 * 3 print x print x == 7", "7\ntrue\n");
    expect_output("HALT", "print 1", "1\n");

    expect_output("simple if", "if true:\n    print(\"yes\")", "yes\n");
    expect_output("false if", "if false:\n    print(\"no\")", "");
    expect_output("if else", "if false:\n    print(\"no\")\nelse:\n    print(\"yes\")", "yes\n");
    expect_output("if elif", "if false:\n    print(\"a\")\nelif true:\n    print(\"b\")", "b\n");
    expect_output("if elseif", "if false:\n    print(\"a\")\nelseif true:\n    print(\"b\")", "b\n");
    expect_output("if elif else", "if false:\n    print(\"a\")\nelif false:\n    print(\"b\")\nelse:\n    print(\"c\")", "c\n");
    expect_output("truthy values",
                  "if 10 {\n    print(\"a\")\n}\nif 0 {\n    print(\"b\")\n}\nif \"hello\" {\n    print(\"c\")\n}\nif \"\" {\n    print(\"d\")"
                  "\n}\nif null {\n    print(\"e\")\n}",
                  "a\nc\n");
    expect_output("comparison if", "x = 20\nif x > 10:\n    print(\"big\")", "big\n");
    expect_output("nested if", "x = 20\nif x > 10:\n    if x > 15:\n        print(\"very big\")\n    else:\n        print(\"big\")",
                  "very big\n");
    expect_output("nested else", "if true:\n    if false:\n        print(\"a\")\n    else:\n        print(\"b\")", "b\n");
    expect_output("colon block", "if true:\n    print(\"colon\")", "colon\n");
    expect_output("colon after block", "if true:\n    print(\"a\")\nprint(\"b\")", "a\nb\n");
    expect_output("nested indent",
                  "if true:\n    print(\"outer\")\n    if true:\n        print(\"inner\")\n    print(\"outer again\")\nprint(\"done\")",
                  "outer\ninner\nouter again\ndone\n");
    expect_output("brace block", "if true {\n    print(\"brace\")\n}", "brace\n");
    expect_output("brace elif else",
                  "if false {\n    print(\"a\")\n} elif true {\n    print(\"b\")\n} else {\n    print(\"c\")\n}", "b\n");
    expect_output("if parenthesized condition", "a = 2\nb = 1\nif (a > b):\n    print(\"if\")", "if\n");
    expect_output("elif parenthesized condition",
                  "a = 1\nif false:\n    print(\"if\")\nelif ((a - 5) / 3):\n    print(\"elif\")", "elif\n");
    expect_output("elseif parenthesized condition",
                  "a = 10\nb = 1\nif false:\n    print(\"if\")\nelseif (a + b * 2) > 10:\n    print(\"elseif\")",
                  "elseif\n");
    expect_output("if math condition", "if 1 + 2:\n    print(\"if\")", "if\n");
    expect_output("elif math condition", "if 0:\n    print(\"if\")\nelif 3 * 4:\n    print(\"elif\")", "elif\n");
    expect_output("elseif math condition", "if 0:\n    print(\"if\")\nelseif -2:\n    print(\"elseif\")", "elseif\n");
    expect_output("condition operator precedence",
                  "a = 4\nb = 2\nif false:\n    print(\"no\")\nelseif (a + b * 2) > 10:\n    print(\"yes\")\nelse:\n    "
                  "print(\"nope\")",
                  "nope\n");
    expect_output("condition nested parentheses",
                  "a = 4\nb = 2\nif false:\n    print(\"no\")\nelseif ((a + b) * 2) > 10:\n    print(\"yes\")",
                  "yes\n");
    expect_output("if elif elseif else indent",
                  "a = 3\nb = 1\nif a > b:\n    print(\"if\")\nelif (a + 2) > b:\n    print(\"elif\")\nelseif (a * 2) > "
                  "b:\n    print(\"elseif\")\nelse:\n    print(\"else\")",
                  "if\n");
    expect_output("if elif elseif else brace",
                  "a = 1\nb = 10\nif a > b {\n    print(\"if\")\n} elif (a + 2) > b {\n    print(\"elif\")\n} elseif (a "
                  "* 2) > b {\n    print(\"elseif\")\n} else {\n    print(\"else\")\n}",
                  "else\n");

    expect_output("list read write",
                  "numbers = [10, 20, 30]\nprint(numbers[0])\nprint(numbers[1])\nnumbers[1] = 50\nprint(numbers[1])",
                  "10\n20\n50\n");
    expect_output("mixed list", "items = [10, \"hello\", true]\nprint(items[0])\nprint(items[1])\nprint(items[2])",
                  "10\nhello\ntrue\n");
    expect_output("dictionary",
                  "player = {\n    \"name\": \"Emir\",\n    \"score\": 100\n}\nprint(player[\"name\"])\nprint(player["
                  "\"score\"])\nplayer[\"score\"] = 150\nplayer[\"level\"] = 5\nprint(player[\"score\"])\nprint(player["
                  "\"level\"])",
                  "Emir\n100\n150\n5\n");
    expect_output("collection truthiness",
                  "if []:\n    print(\"wrong\")\nif [1]:\n    print(\"list true\")\nif {}:\n    print(\"wrong\")\nif "
                  "{\"a\": 1}:\n    print(\"dict true\")",
                  "list true\ndict true\n");
    expect_output("uninitialized typed variables",
                  "int score\nstring name\nscore = 100\nname = \"Emir\"\nprint(score)\nprint(name)", "100\nEmir\n");
    expect_output("typed variable starts null", "int score\nprint(score)", "null\n");
    expect_output("nested list", "grid = [[1, 2], [3, 4]]\nprint(grid[1][0])", "3\n");
    expect_output("print list", "print([10, 20])", "[10, 20]\n");
    expect_output("empty collections print", "print([])\nprint({})", "[]\n{}\n");

    expect_output("empty tuple", "print(())", "()\n");
    expect_output("single-element tuple", "t = (10,)\nprint(t)\nprint(t[0])", "(10,)\n10\n");
    expect_output("normal tuple", "t = (10, 20, 30)\nprint(t[0])\nprint(t[2])", "10\n30\n");
    expect_output("parenthesized expression not tuple", "print((10))", "10\n");
    expect_output("mixed tuple", "t = (10, \"hello\", true)\nprint(t[1])\nprint(t[2])", "hello\ntrue\n");
    expect_output("nested tuples", "t = ((1, 2), (3, 4))\nprint(t[1][0])", "3\n");
    expect_output("tuple methods",
                  "t = (10, 20, 30)\nprint(t.length())\nprint(t.contains(20))\nprint(t.find(30))\nprint(t.first())\nprint(t.last())\nprint(t.find(99))",
                  "3\ntrue\n2\n10\n30\n-1\n");
    expect_output("tuple alias", "a = (1, 2)\nb = a\nprint(b[1])", "2\n");
    expect_output("tuple as return",
                  "func pair():\n    return (1, 2)\nt = pair()\nprint(t[0])\nprint(t[1])\n", "1\n2\n");
    expect_output("tuple membership", "print(2 in (1, 2, 3))", "true\n");
    expect_output("empty tuple truthiness",
                  "if ():\n    print(\"wrong\")\nif (1,):\n    print(\"tuple true\")", "tuple true\n");

    expect_output("set duplicates",
                  "s = {1, 2, 2, 3}\nprint(s.length())\nprint(s.contains(2))\nprint(s.contains(9))", "3\ntrue\nfalse\n");
    expect_output("set mixed", "s = {1, \"a\", true}\nprint(s.contains(\"a\"))\nprint(s.contains(true))", "true\ntrue\n");
    expect_output("set add remove",
                  "s = {1, 2}\ns.add(3)\ns.add(2)\nprint(s.length())\nprint(s.contains(3))\ns.remove(1)\nprint(s.contains(1))\nprint(s.is_empty())",
                  "3\ntrue\nfalse\nfalse\n");
    expect_output("empty set via clear", "s = {1}\ns.clear()\nprint(s.is_empty())\nprint(s.length())", "true\n0\n");
    expect_output("set operations",
                  "a = {1, 2, 3}\nb = {2, 3, 4}\nc = a.union(b)\nd = a.intersection(b)\ne = a.difference(b)\n"
                  "print(c.length())\nprint(c.contains(1))\nprint(c.contains(4))\n"
                  "print(d.length())\nprint(d.contains(2))\nprint(d.contains(1))\n"
                  "print(e.length())\nprint(e.contains(1))\nprint(e.contains(2))\n"
                  "print(a.length())\nprint(b.length())",
                  "4\ntrue\ntrue\n2\ntrue\nfalse\n1\ntrue\nfalse\n3\n3\n");
    expect_output("set alias mutation", "a = {1}\nb = a\nb.add(2)\nprint(a.contains(2))", "true\n");
    expect_output("set in list", "x = [{1, 2}]\nprint(x[0].contains(1))", "true\n");
    expect_output("set membership op", "print(2 in {1, 2, 3})", "true\n");
    expect_output("empty set difference",
                  "e = {1}.difference({1})\nprint(e.is_empty())", "true\n");
    expect_output("if brace block still", "if true {\n    print(1)\n}", "1\n");
    expect_output("empty dict still", "d = {}\nprint(d.length())", "0\n");

    expect_output("list api",
                  "items = [10, 20, 30]\nitems.append(40)\nitems.prepend(5)\nprint(items.first())\nprint(items.last())\n"
                  "print(items.length())\nprint(items.find(20))\nprint(items.contains(30))\nprint(items.count(10))\n"
                  "items.remove(20)\nitems.remove_at(0)\nprint(items.length())\nitems.reverse()\nitems.sort()\n"
                  "items.clear()\nprint(items.is_empty())",
                  "5\n40\n5\n2\ntrue\n1\n3\ntrue\n");
    expect_output("mixed list api",
                  "items = [10, \"hello\", true]\nprint(items.find(\"hello\"))\nprint(items.contains(true))\nprint(items.count(10))",
                  "1\ntrue\n1\n");
    expect_output("list sort strings",
                  "names = [\"Mehmet\", \"Ali\", \"Emir\"]\nnames.sort()\nprint(names)", "[Ali, Emir, Mehmet]\n");
    expect_output("list sort numbers", "numbers = [30, 10, 20]\nnumbers.sort()\nprint(numbers)", "[10, 20, 30]\n");
    expect_output("collection filter map take",
                  "func is_even(x):\n    return x % 2 == 0\nfunc double(x):\n    return x * 2\n"
                  "nums = [1, 2, 3, 4]\nprint(filter(nums, is_even))\nprint(map(nums, double))\nprint(take(nums, 2))\n",
                  "[2, 4]\n[2, 4, 6, 8]\n[1, 2]\n");
    expect_output("parallel map ordered",
                  "func double(x):\n    return x * 2\nprint(parallel_map([1, 2, 3], double))\n",
                  "[2, 4, 6]\n");
    expect_output("parallel dot map ordered",
                  "func double(x):\n    return x * 2\nprint(parallel.map([1, 2, 3], double))\n",
                  "[2, 4, 6]\n");
    expect_output("collection sort_by group_by",
                  "func player_score(p):\n    return p[\"score\"]\nfunc player_team(p):\n    return p[\"team\"]\n"
                  "players = [{\"name\": \"Ada\", \"team\": \"A\", \"score\": 10}, {\"name\": \"Bob\", \"team\": \"B\", \"score\": 20}, {\"name\": \"Cara\", \"team\": \"A\", \"score\": 15}]\n"
                  "ranked = sort_by(players, player_score, true)\nprint(ranked[0][\"name\"])\n"
                  "teams = group_by(players, player_team)\nprint(teams[\"A\"].length())\n",
                  "Bob\n2\n");
    expect_output("pipeline operator",
                  "func identity(x):\n    return x\nnums = [3, 1, 2]\nprint(nums |> sort_by(identity) |> take(2))\n",
                  "[1, 2]\n");
    expect_output("dictionary api",
                  "player = {\n    \"name\": \"Emir\",\n    \"score\": 100\n}\nprint(player.get(\"name\"))\n"
                  "print(player.contains_key(\"score\"))\nprint(player.length())\nkeys = player.keys()\n"
                  "values = player.values()\nprint(keys)\nprint(values)\nplayer.remove(\"score\")\n"
                  "print(player.contains_key(\"score\"))\nplayer.clear()\nprint(player.is_empty())",
                  "Emir\ntrue\n2\n[name, score]\n[Emir, 100]\nfalse\ntrue\n");
    expect_output("dict lookup hit",
                  "key = \"k\"\nbox = {}\nbox[key] = 9\nprint(box[key])\nprint(box.contains_key(key))",
                  "9\ntrue\n");
    expect_output("dict lookup miss contains",
                  "box = {\"a\": 1}\nprint(box.contains_key(\"z\"))",
                  "false\n");
    expect_output("dict lookup overwrite",
                  "box = {\"a\": 1}\nbox[\"a\"] = 2\nprint(box[\"a\"])",
                  "2\n");
    expect_output("string api",
                  "text = \"Hello World\"\nprint(text.find(\"World\"))\nprint(text.contains(\"Hello\"))\n"
                  "print(text.replace(\"World\", \"PScript\"))\nprint(text.starts_with(\"Hello\"))\n"
                  "print(text.ends_with(\"World\"))\nprint(text.first())\nprint(text.last())\nprint(text.length())\n"
                  "print(text.is_empty())",
                  "6\ntrue\nHello PScript\ntrue\ntrue\nH\nd\n11\nfalse\n");
    expect_output("method call expression", "x = [10, 20, 30].length()\ny = \"Hello\".contains(\"ell\")\nprint(x)\nprint(y)",
                  "3\ntrue\n");
    expect_output("method in if", "if [1, 2].is_empty():\n    print(\"empty\")\nelse:\n    print(\"ok\")", "ok\n");
    expect_output("find missing", "print([1].find(2))\nprint(\"ab\".find(\"c\"))", "-1\n-1\n");

    expect_output("integer compare operators",
                  "print(3 < 5)\nprint(5 <= 5)\nprint(7 > 2)\nprint(4 >= 4)\nprint(10 > 10)\n",
                  "true\ntrue\ntrue\ntrue\nfalse\n");
    expect_output("integer divmod loop",
                  "total = 0\ni = 1\nwhile i < 5:\n    total = total + (i / 2) + (i % 3)\n    i = i + 1\nprint(total)",
                  "8\n");

    expect_unhandled("division by zero", compile("print 1 / 0"), "division by zero");
    expect_unhandled("modulo by zero", compile("print 1 % 0"), "modulo by zero");
    expect_unhandled("float division by zero", compile("print(1.0 / 0)"), "division by zero");
    expect_unhandled("float modulo rejected", compile("print(1.0 % 0)"), "unsupported operand type");
    expect_unhandled("unsupported operand type", compile("print true + 1"), "unsupported operand type");

    {
        Bytecode bc;
        bc.register_count = 1;
        bc.instructions.push_back(Instruction{Opcode::Print, 3, 0, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("invalid register", bc, "invalid register");
    }

    {
        Bytecode bc;
        bc.register_count = 1;
        bc.instructions.push_back(Instruction{Opcode::LoadConst, 0, 0, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("invalid constant", bc, "invalid constant index");
    }

    {
        Bytecode bc;
        bc.register_count = 1;
        bc.instructions.push_back(Instruction{Opcode::LoadVar, 0, 0, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("invalid variable", bc, "invalid variable index");
    }

    {
        Bytecode bc;
        bc.register_count = 1;
        bc.instructions.push_back(Instruction{Opcode::Print, 0, 0, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("uninitialized register", bc, "uninitialized register");
    }

    {
        Bytecode bc;
        bc.register_count = 1;
        bc.constants.push_back(Constant{ConstantKind::Integer, "1", false});
        bc.instructions.push_back(Instruction{Opcode::LoadConst, 0, 0, 0});
        bc.instructions.push_back(Instruction{static_cast<Opcode>(255), 0, 0, 0});
        expect_vm_error("invalid opcode", bc, "invalid instruction");
    }

    {
        Bytecode bc;
        bc.instructions.push_back(Instruction{Opcode::Jump, 5, 0, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("invalid jump", bc, "invalid jump");
    }

    {
        Bytecode bc;
        bc.register_count = 1;
        bc.instructions.push_back(Instruction{Opcode::LoadFunction, 0, 3, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("invalid function index", bc, "invalid function index");
    }

    {
        Bytecode bc;
        bc.register_count = 1;
        bc.instructions.push_back(Instruction{Opcode::LoadLocal, 0, 4, 0});
        bc.instructions.push_back(Instruction{Opcode::Halt, 0, 0, 0});
        expect_vm_error("invalid local index", bc, "invalid local index");
    }

    expect_unhandled("index on int", compile("x = 1\nprint(x[0])"), "not a list");
    expect_unhandled("key on int", compile("x = 1\nprint(x[\"a\"])"), "not a dictionary");
    expect_unhandled("list index out of range", compile("x = [1]\nprint(x[3])"), "list index out of range");
    expect_unhandled("tuple index out of range", compile("t = (1, 2)\nprint(t[5])"), "tuple index out of range");
    expect_unhandled("empty tuple first", compile("print(().first())"), "tuple is empty");
    expect_unhandled("empty tuple last", compile("print(().last())"), "tuple is empty");
    expect_unhandled("tuple unknown method", compile("t = (1,)\nt.append(2)\n"), "unknown tuple method 'append'");
    expect_unhandled("set remove missing", compile("s = {1}\ns.remove(2)\n"), "undefined set value");
    expect_unhandled("union type", compile("s = {1}\ns.union([1])\n"), "'union' expects a set");
    expect_unhandled("list index type", compile("x = [1]\nprint(x[\"a\"])"), "list index must be an integer");
    expect_unhandled("dict key type", compile("x = {\"a\": 1}\nprint(x[0])"), "dictionary key must be a string");
    expect_unhandled("undefined dict key", compile("x = {}\nprint(x[\"a\"])"), "undefined dictionary key");
    expect_unhandled("index assign on int", compile("x = 1\nx[0] = 2"), "not a list");
    expect_unhandled("list method on int", compile("x = 1\nx.append(2)"), "cannot call method 'append' on int");
    expect_unhandled("dict method on list", compile("x = []\nprint(x.get(\"a\"))"), "unknown list method 'get'");
    expect_unhandled("string method on list", compile("x = []\nprint(x.starts_with(\"a\"))"),
                      "unknown list method 'starts_with'");
    expect_unhandled("append argc", compile("x = []\nx.append()"), "'append' expects 1 argument");
    expect_unhandled("length argc", compile("x = []\nprint(x.length(1))"), "'length' expects no arguments");
    expect_unhandled("empty first", compile("x = []\nprint(x.first())"), "list is empty");
    expect_unhandled("empty last", compile("x = []\nprint(x.last())"), "list is empty");
    expect_unhandled("empty string first", compile("print(\"\".first())"), "string is empty");
    expect_unhandled("remove_at range", compile("x = [1]\nx.remove_at(3)"), "list index out of range");
    expect_unhandled("remove_at type", compile("x = [1]\nx.remove_at(\"a\")"), "'remove_at' expects an integer index");
    expect_unhandled("sort mixed", compile("x = [1, \"a\"]\nx.sort()"), "cannot sort incomparable values");
    expect_unhandled("dict get key type", compile("x = {}\nprint(x.get(1))"), "'get' expects a string key");
    expect_unhandled("string find type", compile("print(\"a\".find(1))"), "'find' expects a string argument");
    expect_unhandled("remove missing", compile("x = [1]\nx.remove(2)"), "value not found");
    expect_unhandled("dict remove missing", compile("x = {}\nx.remove(\"a\")"), "undefined dictionary key");

    expect_output("while loop", "x = 0\nwhile x < 3:\n    print(x)\n    x = x + 1\n", "0\n1\n2\n");
    expect_output("while false start", "while false:\n    print(1)\nprint(2)\n", "2\n");
    expect_output("nested while",
                  "x = 0\nwhile x < 2:\n    y = 0\n    while y < 2:\n        print(x, y)\n        y = y + 1\n    x = x + 1\n",
                  "0 0\n0 1\n1 0\n1 1\n");
    expect_output("while expression condition", "x = 1\ny = 3\nwhile ((x + 2) * y) < 12:\n    print(x)\n    x = x + 1\n",
                  "1\n");
    expect_output("while parentheses", "x = 0\nwhile (x < 2):\n    print(x)\n    x = x + 1\n", "0\n1\n");

    expect_output("for list", "numbers = [1, 2, 3]\nfor number in numbers:\n    print(number)\n", "1\n2\n3\n");
    expect_output("for empty list", "for number in []:\n    print(number)\nprint(9)\n", "9\n");
    expect_output("nested for",
                  "for a in [1, 2]:\n    for b in [3]:\n        print(a, b)\n", "1 3\n2 3\n");
    expect_output("for and while",
                  "for n in [1, 2]:\n    x = 0\n    while x < n:\n        print(n)\n        x = x + 1\n",
                  "1\n2\n2\n");
    expect_output("for loop variable assign",
                  "item = 0\nfor item in [7, 8]:\n    print(item)\nprint(item)\n", "7\n8\n8\n");

    expect_output("range stop", "for i in range(3):\n    print(i)\n", "0\n1\n2\n");
    expect_output("range start stop", "for i in range(2, 5):\n    print(i)\n", "2\n3\n4\n");
    expect_output("range step", "for i in range(2, 10, 2):\n    print(i)\n", "2\n4\n6\n8\n");
    expect_output("range negative step", "for i in range(10, 0, -2):\n    print(i)\n", "10\n8\n6\n4\n2\n");
    expect_output("empty range", "for i in range(0):\n    print(i)\nprint(1)\n", "1\n");
    expect_output("print range", "print(range(2, 10, 2))\n", "range(2, 10, 2)\n");

    expect_output("while break",
                  "x = 0\nwhile x < 10:\n    if x == 2:\n        break\n    print(x)\n    x = x + 1\n", "0\n1\n");
    expect_output("for break", "for i in range(10):\n    if i == 2:\n        break\n    print(i)\n", "0\n1\n");
    expect_output("nested break",
                  "x = 0\nwhile x < 2:\n    y = 0\n    while y < 5:\n        if y == 1:\n            break\n        print(x, y)\n        y = y + 1\n    x = x + 1\n",
                  "0 0\n1 0\n");
    expect_output("while continue",
                  "x = 0\nwhile x < 4:\n    x = x + 1\n    if x == 2:\n        continue\n    print(x)\n", "1\n3\n4\n");
    expect_output("for continue", "for i in range(4):\n    if i == 1:\n        continue\n    print(i)\n", "0\n2\n3\n");
    expect_output("nested continue",
                  "for i in [1, 2]:\n    x = 0\n    while x < 3:\n        x = x + 1\n        if x == 2:\n            continue\n        print(i, x)\n",
                  "1 1\n1 3\n2 1\n2 3\n");
    expect_output("combined continue break",
                  "for i in range(10):\n    if i == 3:\n        continue\n    if i == 7:\n        break\n    print(i)\n",
                  "0\n1\n2\n4\n5\n6\n");
    expect_output("for brace", "for i in [1] {\n    print(i)\n}\n", "1\n");

    expect_builtin_output("iterator null value",
                  "class NullIterator:\n    int i\n    constructor():\n        this.i = 0\n    func next():\n        if this.i == 0:\n            this.i = 1\n            return null\n        if this.i == 1:\n            this.i = 2\n            return 20\n        throw StopIteration(\"\", null)\nclass NullIterable:\n    func iter():\n        return NullIterator()\nfor x in NullIterable():\n    print(x)\n",
                  "null\n20\n");
    expect_builtin_output("custom counter iterable",
                  "class CounterIterator:\n    int current\n    int limit\n    constructor(limit):\n        this.current = 0\n        this.limit = limit\n    func next():\n        if this.current >= this.limit:\n            throw StopIteration(\"\", null)\n        value = this.current\n        this.current = this.current + 1\n        return value\nclass Counter:\n    int limit\n    constructor(limit):\n        this.limit = limit\n    func iter():\n        return CounterIterator(this.limit)\nfor x in Counter(3):\n    print(x)\n",
                  "0\n1\n2\n");
    expect_output("iterator manual next",
                  "it = [10, 20].iter()\nprint(it.next())\nprint(it.next())\nfor x in it:\n    print(x)\n",
                  "10\n20\n");
    expect_builtin_output("iterator inheritance",
                  "class BaseIterator:\n    int n\n    constructor():\n        this.n = 0\n    func next():\n        if this.n >= 2:\n            throw StopIteration(\"\", null)\n        value = this.n\n        this.n = this.n + 1\n        return value\nclass ChildIterator : BaseIterator:\n    pass\nclass BaseIterable:\n    func iter():\n        return ChildIterator()\nfor x in BaseIterable():\n    print(x)\n",
                  "0\n1\n");
    expect_builtin_output("iterator factory iterable",
                  "class RangeIterator:\n    int n\n    int end\n    constructor(start, end):\n        this.n = start\n        this.end = end\n    func next():\n        if this.n >= this.end:\n            throw StopIteration(\"\", null)\n        value = this.n\n        this.n = this.n + 1\n        return value\nclass RangeIterable:\n    int start\n    int end\n    constructor(start, end):\n        this.start = start\n        this.end = end\n    func iter():\n        return RangeIterator(this.start, this.end)\nfor x in RangeIterable(5, 7):\n    print(x)\n",
                  "5\n6\n");
    expect_unhandled("for body stopiteration",
                     compile_with_builtins("for x in [1]:\n    throw StopIteration(\"\", null)\n"), "StopIteration");
    expect_builtin_output("iterator other exception",
                  "class BadIterator:\n    func next():\n        throw ValueException(\"boom\", null)\nclass BadIterable:\n    func iter():\n        return BadIterator()\ntry:\n    for x in BadIterable():\n        print(x)\ncatch ValueException error:\n    print(error.message)\n",
                  "boom\n");
    expect_output("for finally cleanup",
                  "done = 0\nfor x in [1, 2]:\n    try:\n        if x == 2:\n            break\n    finally:\n        done = done + 1\nprint(done)\n",
                  "2\n");
    expect_unhandled("missing iter", compile_with_builtins("class X:\n    pass\nfor x in X():\n    print(x)\n"),
                     "object is not iterable");
    expect_unhandled("missing next", compile_with_builtins("class X:\n    func iter():\n        return X()\nfor x in X():\n    print(x)\n"),
                     "object has no method 'next'");

    expect_builtin_output("iterator nested loops",
                  "class CountIterator:\n    int n\n    int end\n    constructor(end):\n        this.n = 0\n        this.end = end\n    func next():\n        if this.n >= this.end:\n            throw StopIteration(\"\", null)\n        value = this.n\n        this.n = this.n + 1\n        return value\nclass Counter:\n    int end\n    constructor(end):\n        this.end = end\n    func iter():\n        return CountIterator(this.end)\nfor a in Counter(2):\n    for b in Counter(2):\n        print(a, b)\n",
                  "0 0\n0 1\n1 0\n1 1\n");
    expect_builtin_output("iterator break continue",
                  "class CountIterator:\n    int n\n    int limit\n    constructor(limit):\n        this.n = 0\n        this.limit = limit\n    func next():\n        if this.n >= this.limit:\n            throw StopIteration(\"\", null)\n        value = this.n\n        this.n = this.n + 1\n        return value\nclass Counter:\n    int limit\n    constructor(limit):\n        this.limit = limit\n    func iter():\n        return CountIterator(this.limit)\nfor x in Counter(5):\n    if x == 3:\n        continue\n    if x == 4:\n        break\n    print(x)\n",
                  "0\n1\n2\n");
    expect_builtin_output("iterator super override",
                  "class BaseIterator:\n    protected int n\n    constructor():\n        this.n = 0\n    func next():\n        if this.n >= 1:\n            throw StopIteration(\"\", null)\n        value = this.n\n        this.n = this.n + 1\n        return value\nclass ChildIterator : BaseIterator:\n    func next():\n        if this.n >= 2:\n            throw StopIteration(\"\", null)\n        value = this.n\n        this.n = this.n + 1\n        return value\nclass Wrap:\n    func iter():\n        return ChildIterator()\nfor x in Wrap():\n    print(x)\n",
                  "0\n1\n");
    expect_builtin_output("iterator resumed state",
                  "class CountIterator:\n    int n\n    int limit\n    constructor(limit):\n        this.n = 0\n        this.limit = limit\n    func next():\n        if this.n >= this.limit:\n            throw StopIteration(\"\", null)\n        value = this.n\n        this.n = this.n + 1\n        return value\nclass Counter:\n    int limit\n    constructor(limit):\n        this.limit = limit\n    func iter():\n        return CountIterator(this.limit)\nit = Counter(3).iter()\nprint(it.next())\nfor x in it:\n    print(x)\n",
                  "0\n1\n2\n");
    expect_builtin_output("iterator try finally outer",
                  "class CountIterator:\n    int n\n    int limit\n    constructor(limit):\n        this.n = 0\n        this.limit = limit\n    func next():\n        if this.n >= this.limit:\n            throw StopIteration(\"\", null)\n        value = this.n\n        this.n = this.n + 1\n        return value\nclass Counter:\n    int limit\n    constructor(limit):\n        this.limit = limit\n    func iter():\n        return CountIterator(this.limit)\ndone = 0\nfor x in Counter(3):\n    try:\n        if x == 1:\n            continue\n        print(x)\n    finally:\n        done = done + 1\nprint(done)\n",
                  "0\n2\n3\n");

    expect_output("while brace", "x = 1\nwhile x {\n    print(x)\n    x = 0\n}\n", "1\n");

    expect_output("logical and", "print(true and true)\nprint(true and false)\nprint(false and true)\n", "true\nfalse\nfalse\n");
    expect_output("logical amp", "print(true && false)\n", "false\n");
    expect_output("logical or", "print(false or true)\nprint(false or false)\nprint(true or false)\n", "true\nfalse\ntrue\n");
    expect_output("logical pipe", "print(false || true)\n", "true\n");
    expect_output("logical not", "print(not false)\nprint(!true)\n", "true\nfalse\n");
    expect_output("logical precedence", "print(false or true and false)\nprint((false or true) and false)\n", "false\nfalse\n");
    expect_output("logical with comparison", "age = 20\nscore = 80\nif age >= 18 and score >= 50:\n    print(\"Passed\")\n", "Passed\n");
    expect_output("short-circuit and", "items = []\nx = false and items.append(1)\nprint(items)\nprint(x)\n", "[]\nfalse\n");
    expect_output("short-circuit or", "items = []\nx = true or items.append(1)\nprint(items)\nprint(x)\n", "[]\ntrue\n");
    expect_output("and evaluates right", "items = []\nx = true and items.append(1)\nprint(items)\n", "[1]\n");
    expect_output("nested logical", "print(not false and true or false)\n", "true\n");

    expect_output("match first wins",
                  "score = 100\nmatch score {\n    100:\n        print(\"Perfect\")\n    _:\n        print(\"Other\")\n}\n",
                  "Perfect\n");
    expect_output("match multi pattern",
                  "score = 95\nmatch score:\n    90, 95:\n        print(\"Excellent\")\n    _:\n        print(\"Other\")\n",
                  "Excellent\n");
    expect_output("match wildcard",
                  "score = 10\nmatch score:\n    100:\n        print(\"Perfect\")\n    _:\n        print(\"Other\")\n",
                  "Other\n");
    expect_output("match guard",
                  "score = 70\nmatch score:\n    value if value >= 90:\n        print(\"Excellent\")\n    value if value >= 50:\n        print(\"Passed\")\n    _:\n        print(\"Failed\")\n",
                  "Passed\n");
    expect_output("match nested",
                  "value = 2\nmatch value {\n    1:\n        print(\"one\")\n    2, 3:\n        if value == 2 {\n            print(\"two\")\n        } else {\n            print(\"three\")\n        }\n    _:\n        print(\"other\")\n}\n",
                  "two\n");
    expect_output("match no fallthrough",
                  "match 1:\n    1:\n        print(\"a\")\n    2:\n        print(\"b\")\n",
                  "a\n");
    expect_output("match list pattern",
                  "pair = [1, 2]\nmatch pair:\n    [1, 2]:\n        print(\"ok\")\n    _:\n        print(\"no\")\n",
                  "ok\n");
    expect_output("class property get set",
                  "class Player:\n    private int _score\n    public get score():\n        return this._score\n"
                  "    public set score(value):\n        this._score = value\n"
                  "p = Player()\np.score = 10\nprint(p.score)\n",
                  "10\n");
    expect_output("conversions",
                  "print(int(\"10\"))\nprint(float(\"1.5\"))\nprint(str(10))\nprint(bool(\"true\"))\nprint(bool(\"false\"))\n",
                  "10\n1.5\n10\ntrue\nfalse\n");
    expect_output("typed conversion", "int age = int(\"21\")\nprint(age)\n", "21\n");

    {
        try {
            const std::string got = run_source_with_input(
                "name = input()\nprint(name)\n", "Emir\n");
            if (got != "Emir\n") {
                fail_test("input no prompt", "got \"" + got + "\"");
            } else {
                pass_test("input no prompt");
            }
        } catch (const std::exception& ex) {
            fail_test("input no prompt", ex.what());
        }
    }
    {
        try {
            const std::string got = run_source_with_input(
                "name = input(\"Name: \")\nage = int(input(\"Age: \"))\nprint(name, age)\n", "Ada\n30\n");
            if (got != "Name: Age: Ada 30\n") {
                fail_test("input prompt convert", "got \"" + got + "\"");
            } else {
                pass_test("input prompt convert");
            }
        } catch (const std::exception& ex) {
            fail_test("input prompt convert", ex.what());
        }
    }

    expect_unhandled("range zero step", compile("print(range(1, 2, 0))"), "range step cannot be zero");
    expect_output("for dict keys", "d = {\"a\": 1, \"b\": 2}\nfor key in d:\n    print(key)\n", "a\nb\n");
    expect_output("for tuple", "for x in (1, 2):\n    print(x)\n", "1\n2\n");
    expect_output("for set", "for x in {3, 1, 2}:\n    print(x)\n", "3\n1\n2\n");
    expect_unhandled("cannot iterate int", compile("for x in 1:\n    print(x)\n"), "cannot iterate int");
    expect_unhandled("range arg type", compile("print(range(1.5))"), "'range' expects integer arguments");

    expect_output("global from nested", "x = 10\nif true:\n    print(x)\n", "10\n");
    expect_output("nested scope reads parents",
                  "x = 10\nif true:\n    y = 20\n    if true:\n        print(x)\n        print(y)\n", "10\n20\n");
    expect_output("assignment updates outer", "x = 10\nif true:\n    x = 20\nprint(x)\n", "20\n");
    expect_output("shadowing declaration",
                  "x = 10\nif true {\n    int x = 20\n    print(x)\n}\nprint(x)\n", "20\n10\n");
    expect_output("typed variable in nested scope",
                  "int x = 10\nif true:\n    x = 20\nprint(x)\n", "20\n");
    expect_output("nested shadowing",
                  "x = 1\nif true:\n    int x = 2\n    if true:\n        int x = 3\n        print(x)\n    print(x)\nprint(x)\n",
                  "3\n2\n1\n");
    expect_output("elif else scopes",
                  "x = 0\nif false:\n    y = 1\nelif true:\n    x = 4\nelse:\n    x = 5\nprint(x)\n", "4\n");
    expect_output("while uses outer", "x = 0\nwhile x < 2:\n    x = x + 1\nprint(x)\n", "2\n");
    expect_output("for updates outer item",
                  "item = 0\nfor item in [7, 8]:\n    print(item)\nprint(item)\n", "7\n8\n8\n");
    expect_output("match case scope",
                  "x = 0\nmatch 1:\n    1:\n        x = 9\n        y = 1\nprint(x)\n", "9\n");
    expect_output("collections still work with scopes",
                  "xs = [1]\nif true:\n    xs.append(2)\nprint(xs)\n", "[1, 2]\n");

    expect_output("compound assignment",
                  "x = 10\nx += 5\nx -= 2\nx *= 3\nx /= 2\nx %= 2\nprint(x)\n", "1\n");
    expect_output("postfix increment value", "x = 10\ny = x++\nprint(x)\nprint(y)\n", "11\n10\n");
    expect_output("prefix increment value", "x = 10\ny = ++x\nprint(x)\nprint(y)\n", "11\n11\n");
    expect_output("decrement", "x = 5\nx--\nprint(x)\n--x\nprint(x)\n", "4\n3\n");
    expect_output("bitwise ops",
                  "print(5 & 3)\nprint(5 | 3)\nprint(5 ^ 3)\nprint(~0)\nprint(5 << 2)\nprint(20 >> 1)\n",
                  "1\n7\n6\n-1\n20\n10\n");
    expect_output("membership list dict string range",
                  "print(2 in [1, 2, 3])\nprint(4 not in [1, 2, 3])\nprint(\"a\" in {\"a\": 1})\nprint(\"b\" not in {\"a\": 1})\nprint(\"pp\" in \"apple\")\nprint(2 in range(5))\nprint(5 in range(5))\n",
                  "true\ntrue\ntrue\ntrue\ntrue\ntrue\nfalse\n");
    expect_output("ternary branches",
                  "print(true ? \"A\" : \"B\")\nprint(false ? \"A\" : \"B\")\nprint(1 + (true ? 10 : 20))\n",
                  "A\nB\n11\n");
    expect_output("ternary conditional eval",
                  "xs = []\nx = false ? xs.append(1) : 9\nprint(xs)\nprint(x)\nys = []\ny = true ? 8 : ys.append(1)\nprint(ys)\nprint(y)\n",
                  "[]\n9\n[]\n8\n");
    expect_output("nested ternary", "print(false ? 1 : true ? 2 : 3)\n", "2\n");
    expect_output("null coalesce non-null", "username = \"Ada\"\nprint(username ?? \"Guest\")\n", "Ada\n");
    expect_output("null coalesce null", "username = null\nprint(username ?? \"Guest\")\n", "Guest\n");
    expect_output("null coalesce short circuit",
                  "xs = []\nprint(1 ?? xs.append(1))\nprint(xs)\n", "1\n[]\n");
    expect_output("null coalesce chain", "print(null ?? null ?? \"ok\")\n", "ok\n");
    expect_output("and still short circuit",
                  "xs = []\nprint(false and xs.append(1))\nprint(xs)\n", "false\n[]\n");

    expect_output("function add return",
                  "func add(a, b):\n    return a + b\nprint(add(10, 20))\n", "30\n");
    expect_output("function brace body",
                  "func add(a, b) {\n    return a + b\n}\nprint(add(5, 7))\n", "12\n");
    expect_output("zero argument call",
                  "func hello():\n    print(\"hi\")\nhello()\n", "hi\n");
    expect_output("defer nested call does not run outer defer early",
                  "func inner():\n    defer print(\"inner\")\n    return\n"
                  "func outer():\n    defer print(\"outer\")\n    inner()\n    print(\"mid\")\n    return\n"
                  "outer()\n",
                  "inner\nmid\nouter\n");
    expect_output("defer runs on return",
                  "func demo():\n    defer print(\"cleanup\")\n    print(\"body\")\n    return\n"
                  "demo()\n",
                  "body\ncleanup\n");
    expect_output("void function call no extra null",
                  "func hello():\n    print(\"Hello\")\nhello()\nprint(\"done\")\n", "Hello\ndone\n");
    expect_output("one argument call",
                  "func ident(x):\n    return x\nprint(ident(9))\n", "9\n");
    expect_output("nested expression call",
                  "func add(a, b):\n    return a + b\nprint(add(1, 2) * 3)\n", "9\n");
    expect_output("function result assignment",
                  "func add(a, b):\n    return a + b\nresult = add(10, 20)\nprint(result)\n", "30\n");
    expect_output("return stops execution",
                  "func test():\n    return 10\n    print(\"never\")\ntest()\n", "");
    expect_output("no explicit return is null",
                  "func test():\n    print(\"hello\")\nx = test()\nprint(x)\n", "hello\nnull\n");
    expect_output("bare return is null",
                  "func test():\n    return\nx = test()\nprint(x)\n", "null\n");
    expect_output("parameter local scope",
                  "func test(x):\n    print(x)\ntest(50)\n", "50\n");
    expect_output("function local variable",
                  "func test():\n    x = 10\n    print(x)\ntest()\n", "10\n");
    expect_output("global visible in function",
                  "x = 10\nfunc test():\n    print(x)\ntest()\n", "10\n");
    expect_output("assignment updates global",
                  "x = 10\nfunc test():\n    x = 20\ntest()\nprint(x)\n", "20\n");
    expect_output("local declaration shadows",
                  "x = 10\nfunc test():\n    int x = 20\ntest()\nprint(x)\n", "10\n");
    expect_output("parameter shadowing",
                  "x = 10\nfunc test(x):\n    print(x)\ntest(50)\nprint(x)\n", "50\n10\n");
    expect_output("function hoisting",
                  "result = add(10, 20)\nfunc add(a, b):\n    return a + b\nprint(result)\n", "30\n");
    expect_output("print of function call",
                  "func add(a, b):\n    return a + b\nprint(add(5, 7))\n", "12\n");
    expect_output("multiple functions",
                  "func one():\n    return 1\nfunc two():\n    return 2\nprint(one() + two())\n", "3\n");

    expect_output("automatic return arithmetic",
                  "func square(x) {\n    x * x\n}\nresult = square(5)\nprint(result)\n", "25\n");
    expect_output("automatic return colon",
                  "func add(a, b):\n    a + b\nprint(add(10, 20))\n", "30\n");
    expect_output("automatic return identifier",
                  "func test():\n    x = 10\n    x\nprint(test())\n", "10\n");
    expect_output("automatic return of call",
                  "func add(a, b):\n    a + b\nfunc twice(x):\n    add(x, x)\nprint(twice(4))\n", "8\n");
    expect_output("last statement not expression is null",
                  "func test():\n    x = 10\nprint(test())\n", "null\n");
    expect_output("print last still null",
                  "func test():\n    print(\"hello\")\nx = test()\nprint(x)\n", "hello\nnull\n");
    expect_output("if body expression is not automatic return",
                  "func test():\n    if true:\n        10\nprint(test())\n", "null\n");
    expect_output("explicit return beats trailing expression",
                  "func test():\n    return 10\n    20\nprint(test())\n", "10\n");
    expect_output("return from if true",
                  "func test(x):\n    if x > 10:\n        return 100\n    return 0\nprint(test(20))\n", "100\n");
    expect_output("return from if false",
                  "func test(x):\n    if x > 10:\n        return 100\n    return 0\nprint(test(5))\n", "0\n");
    expect_output("return from loop",
                  "func find():\n    for item in [1, 2, 3]:\n        return item\n    return null\nprint(find())\n",
                  "1\n");
    expect_output("factorial",
                  "func factorial(n):\n    if n <= 1:\n        return 1\n    return n * factorial(n - 1)\nprint(factorial(5))\n",
                  "120\n");
    expect_output("recursive countdown",
                  "func down(n):\n    if n <= 0:\n        return 0\n    print(n)\n    return down(n - 1)\nprint(down(3))\n",
                  "3\n2\n1\n0\n");
    expect_output("recursive locals",
                  "func count(n):\n    x = n\n    if n <= 0:\n        return x\n    return count(n - 1)\nprint(count(3))\n",
                  "0\n");
    expect_output("nested recursive calls",
                  "func fib(n):\n    if n <= 1:\n        return n\n    return fib(n - 1) + fib(n - 2)\nprint(fib(6))\n",
                  "8\n");
    expect_output("recursive expression add",
                  "func factorial(n):\n    if n <= 1:\n        return 1\n    return n * factorial(n - 1)\nresult = factorial(5) + 10\nprint(result)\n",
                  "130\n");
    expect_output("two recursive calls in expression",
                  "func factorial(n):\n    if n <= 1:\n        return 1\n    return n * factorial(n - 1)\nresult = factorial(3) * factorial(4)\nprint(result)\n",
                  "144\n");

    expect_output("nested function call",
                  "func outer():\n    func inner():\n        print(\"hello\")\n    inner()\nouter()\n", "hello\n");
    expect_output("nested capture print",
                  "func outer():\n    x = 10\n    func inner():\n        print(x)\n    inner()\nouter()\n", "10\n");
    expect_output("nested function parameters",
                  "func outer():\n    func add(a, b):\n        return a + b\n    print(add(2, 3))\nouter()\n", "5\n");
    expect_output("nested function locals",
                  "func outer():\n    func inner():\n        y = 7\n        return y\n    print(inner())\nouter()\n", "7\n");
    expect_output("nested function hoisting",
                  "func outer():\n    result = inner()\n    func inner():\n        return 10\n    return result\nprint(outer())\n",
                  "10\n");
    expect_output("capture one variable",
                  "func outer():\n    x = 10\n    y = 20\n    func inner():\n        return x\n    print(inner())\nouter()\n",
                  "10\n");
    expect_output("capture multiple variables",
                  "func outer():\n    x = 10\n    y = 20\n    func inner():\n        return x + y\n    print(inner())\nouter()\n",
                  "30\n");
    expect_output("nested capture",
                  "func outer():\n    x = 3\n    func mid():\n        func inner():\n            return x\n        return inner()\n    print(mid())\nouter()\n",
                  "3\n");
    expect_output("global without capture",
                  "x = 99\nfunc outer():\n    func inner():\n        return x\n    print(inner())\nouter()\n", "99\n");
    expect_output("capture mutation shared",
                  "func outer():\n    x = 10\n    func inner():\n        return x\n    x = 20\n    print(inner())\nouter()\n",
                  "20\n");
    expect_output("return closure",
                  "func outer():\n    x = 10\n    func inner():\n        return x\n    x = 20\n    return inner\nf = outer()\nprint(f())\n",
                  "20\n");
    expect_unhandled("call stack depth",
                     compile("func f():\n    f()\nf()\n"),
                     "maximum call stack depth exceeded");
    expect_output("make_counter",
                  "func make_counter():\n    count = 0\n    func increment():\n        count += 1\n        return count\n    return increment\ncounter = make_counter()\nprint(counter())\nprint(counter())\n",
                  "1\n2\n");
    expect_output("independent closures",
                  "func make_counter():\n    count = 0\n    func increment():\n        count += 1\n        return count\n    return increment\na = make_counter()\nb = make_counter()\nprint(a())\nprint(a())\nprint(b())\n",
                  "1\n2\n1\n");
    expect_output("function value call",
                  "func add(a, b):\n    return a + b\nf = add\nprint(f(10, 20))\n", "30\n");
    expect_output("closure mutation operators",
                  "func outer():\n    x = 10\n    func inner():\n        x += 1\n        return x\n    return inner\nf = outer()\nprint(f())\nprint(f())\nprint(f())\n",
                  "11\n12\n13\n");
    expect_output("independent make_value",
                  "func make_value(x):\n    func get():\n        return x\n    return get\na = make_value(10)\nb = make_value(20)\nprint(a())\nprint(b())\n",
                  "10\n20\n");
    expect_output("nested recursive function",
                  "func outer():\n    func count(n):\n        if n <= 0:\n            return 0\n        return count(n - 1)\n    return count(5)\nprint(outer())\n",
                  "0\n");
    expect_output("nested automatic return still expr",
                  "func outer():\n    func inner():\n        10\n    inner()\nprint(outer())\n", "10\n");
    expect_output("nested func not automatic return",
                  "func outer():\n    func inner():\n        10\nprint(outer())\n", "null\n");

    expect_output("struct declaration and fields",
                  "struct Player:\n    name\n    number\np = Player()\np.name = \"Emir\"\np.number = 10\n"
                  "print(p.name)\nprint(p.number)\nprint(p)",
                  "Emir\n10\nPlayer{name: Emir, number: 10}\n");
    expect_output("record same as struct",
                  "record Team:\n    name\n    score\nt = Team()\nt.name = \"Home\"\nt.score = 3\nprint(t.name)\nprint(t)",
                  "Home\nTeam{name: Home, score: 3}\n");
    expect_output("struct brace syntax",
                  "struct Point {\n    x\n    y\n}\np = Point()\np.x = 1\np.y = 2\nprint(p.x)\nprint(p.y)",
                  "1\n2\n");
    expect_output("struct typed fields",
                  "struct Player:\n    string name\n    int number\np = Player()\np.name = \"A\"\np.number = 7\nprint(p.number)",
                  "7\n");
    expect_output("nested struct",
                  "struct Address:\n    city\nstruct Player:\n    Address home\np = Player()\np.home = Address()\np.home.city = \"Istanbul\"\nprint(p.home.city)",
                  "Istanbul\n");
    expect_output("struct in list",
                  "struct Item:\n    value\na = Item()\na.value = 1\nitems = [a]\nitem = items[0]\nprint(item.value)",
                  "1\n");
    expect_output("struct in dict",
                  "struct Item:\n    value\na = Item()\na.value = 5\nd = {\"item\": a}\nitem = d[\"item\"]\nprint(item.value)",
                  "5\n");
    expect_output("struct in tuple",
                  "struct Item:\n    value\na = Item()\na.value = 9\nt = (a,)\nitem = t[0]\nprint(item.value)",
                  "9\n");
    expect_output("struct in set",
                  "struct Item:\n    value\na = Item()\na.value = 2\ns = {a}\nprint(s.contains(a))",
                  "true\n");
    expect_output("struct type identity",
                  "struct Player:\n    name\nstruct Team:\n    name\np = Player()\nt = Team()\np.name = \"A\"\nt.name = \"A\"\nprint(p == t)",
                  "false\n");
    expect_output("struct equality",
                  "struct Player:\n    name\n    number\na = Player()\nb = Player()\na.name = \"Emir\"\na.number = 10\nb.name = \"Emir\"\nb.number = 10\nprint(a == b)",
                  "true\n");
    expect_output("struct function param return",
                  "struct Player:\n    name\nfunc greet(Player p):\n    return p.name\nx = Player()\nx.name = \"Emir\"\nprint(greet(x))",
                  "Emir\n");
    expect_output("struct scope",
                  "struct Player:\n    name\nfunc make():\n    Player p = Player()\n    p.name = \"local\"\n    return p\nPlayer x = make()\nprint(x.name)",
                  "local\n");

    expect_output("enum declaration and access",
                  "enum Color:\n    RED\n    GREEN\nc = Color.RED\nprint(c)\nprint(c == Color.RED)\nprint(c == Color.GREEN)",
                  "Color.RED\ntrue\nfalse\n");
    expect_output("enum different type inequality",
                  "enum Color:\n    RED\nenum Status:\n    RED\nprint(Color.RED == Status.RED)",
                  "false\n");
    expect_output("enum match",
                  "enum Color:\n    RED\n    GREEN\nc = Color.GREEN\nmatch c:\n    Color.RED:\n        print(\"red\")\n    Color.GREEN:\n        print(\"green\")",
                  "green\n");
    expect_output("enum function arg return",
                  "enum Color:\n    RED\nfunc label(Color c):\n    return c\nprint(label(Color.RED))",
                  "Color.RED\n");
    expect_output("enum in list",
                  "enum Color:\n    RED\n    BLUE\nprint([Color.RED, Color.BLUE][1])",
                  "Color.BLUE\n");
    expect_output("enum scope",
                  "enum Color:\n    RED\nfunc pick():\n    return Color.RED\nprint(pick())",
                  "Color.RED\n");

    {
        try {
            compile("struct Player:\n    name\np = Player()\nprint(p.missing)\n");
            fail_test("struct unknown field", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("unknown field") == std::string_view::npos) {
                fail_test("struct unknown field", err.message());
            } else {
                pass_test("struct unknown field");
            }
        }
    }
    {
        try {
            run_source("enum Color:\n    RED\nprint(Color.MISSING)\n");
            fail_test("enum unknown member", "expected error");
        } catch (const std::exception& ex) {
            if (std::string_view(ex.what()).find("unknown enum member") == std::string_view::npos) {
                fail_test("enum unknown member", ex.what());
            } else {
                pass_test("enum unknown member");
            }
        }
    }
    {
        try {
            run_source("enum Color:\n    RED\nprint(Color.RED + Color.RED)\n");
            fail_test("enum invalid arithmetic", "expected error");
        } catch (const std::exception& ex) {
            if (std::string_view(ex.what()).find("unsupported operand type") == std::string_view::npos) {
                fail_test("enum invalid arithmetic", ex.what());
            } else {
                pass_test("enum invalid arithmetic");
            }
        }
    }
    {
        try {
            compile("x = 1\nprint(x.name)\n");
            fail_test("invalid field access non-struct", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("cannot resolve field") == std::string_view::npos) {
                fail_test("invalid field access non-struct", err.message());
            } else {
                pass_test("invalid field access non-struct");
            }
        }
    }

    expect_output("pass standalone", "pass\nprint(1)", "1\n");
    expect_output("pass in if", "if false:\n    pass\nprint(2)", "2\n");
    expect_output("pass in while", "x = 0\nwhile x < 1:\n    pass\n    x += 1\nprint(x)", "1\n");
    expect_output("pass in for", "for i in [1]:\n    pass\nprint(3)", "3\n");
    expect_output("pass function returns null", "func noop():\n    pass\nprint(noop())", "null\n");
    expect_output("public struct field", "struct Player:\n    pub name\np = Player()\np.name = \"Emir\"\nprint(p.name)",
                  "Emir\n");
    expect_output("default public struct field", "struct Player:\n    name\np = Player()\np.name = \"A\"\nprint(p.name)",
                  "A\n");

    expect_output("class declaration and fields",
                  "class Player:\n    public string name\np = Player()\np.name = \"Emir\"\nprint(p.name)\n",
                  "Emir\n");
    expect_output("class constructor",
                  "class Player:\n    public string name\n    constructor(name):\n        this.name = name\n"
                  "p = Player(\"Emir\")\nprint(p.name)\n",
                  "Emir\n");
    expect_output("class method",
                  "class Player:\n    int score\n    constructor():\n        this.score = 0\n"
                  "    func add(value):\n        this.score += value\n    func get():\n        this.score\n"
                  "p = Player()\np.add(10)\nprint(p.get())\n",
                  "10\n");
    expect_output("class constructor with args",
                  "class Player:\n    public string name\n    public int score\n    constructor(name, score):\n"
                  "        this.name = name\n        this.score = score\np = Player(\"Emir\", 10)\n"
                  "print(p.score)\n",
                  "10\n");
    expect_output("class brace syntax",
                  "class Point {\n    public x\n    public y\n}\np = Point()\np.x = 1\np.y = 2\nprint(p.x)\n",
                  "1\n");
    expect_output("class access public external",
                  "class Player:\n    pub name\np = Player()\np.name = \"A\"\nprint(p.name)\n",
                  "A\n");
    expect_output("class access private internal",
                  "class Player:\n    private int secret\n    constructor():\n        this.secret = 7\n"
                  "    func get_secret():\n        this.secret\np = Player()\nprint(p.get_secret())\n",
                  "7\n");
    expect_output("class access protected internal",
                  "class Player:\n    protected int score\n    constructor():\n        this.score = 9\n"
                  "    func get_score():\n        this.score\np = Player()\nprint(p.get_score())\n",
                  "9\n");
    expect_output("class distinct from struct",
                  "struct S:\n    value\nclass C:\n    public value\ns = S()\nc = C()\ns.value = 1\nc.value = 2\n"
                  "print(s == c)\n",
                  "false\n");
    expect_output("class reference alias",
                  "class Player:\n    public int score\na = Player()\nb = a\na.score = 5\nprint(b.score)\n",
                  "5\n");
    expect_output("class nested function in method",
                  "class Player:\n    public int score\n    constructor():\n        this.score = 0\n    func bump():\n"
                  "        func add(n):\n            return n + 1\n        this.score += add(2)\n"
                  "p = Player()\np.bump()\nprint(p.score)\n",
                  "3\n");

    {
        try {
            compile("class Player:\n    private int secret\np = Player()\nprint(p.secret)\n");
            fail_test("class private external read", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("cannot access private field") == std::string_view::npos) {
                fail_test("class private external read", err.message());
            } else {
                pass_test("class private external read");
            }
        }
    }
    {
        try {
            compile("class Player:\n    protected int score\np = Player()\nprint(p.score)\n");
            fail_test("class protected external read", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("cannot access protected field") == std::string_view::npos) {
                fail_test("class protected external read", err.message());
            } else {
                pass_test("class protected external read");
            }
        }
    }
    {
        try {
            compile("class Player:\n    int score\n    constructor():\n        return 1\n");
            fail_test("constructor return value", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("constructor cannot return a value") == std::string_view::npos) {
                fail_test("constructor return value", err.message());
            } else {
                pass_test("constructor return value");
            }
        }
    }
    {
        try {
            compile("print(this)\n");
            fail_test("this outside class", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("invalid use of 'this'") == std::string_view::npos) {
                fail_test("this outside class", err.message());
            } else {
                pass_test("this outside class");
            }
        }
    }
    {
        try {
            compile("func outer():\n    print(this)\n");
            fail_test("this in global function", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("invalid use of 'this'") == std::string_view::npos) {
                fail_test("this in global function", err.message());
            } else {
                pass_test("this in global function");
            }
        }
    }
    {
        try {
            compile("class Player:\n    int score\np = Player(1, 2)\n");
            fail_test("class wrong constructor arity", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("expects 0 arguments") == std::string_view::npos) {
                fail_test("class wrong constructor arity", err.message());
            } else {
                pass_test("class wrong constructor arity");
            }
        }
    }

    expect_output("inheritance basic fields",
                  "class Player:\n    public string name\nclass FootballPlayer : Player:\n    public int number\n"
                  "p = FootballPlayer()\np.name = \"Emir\"\np.number = 10\nprint(p.name)\nprint(p.number)\n",
                  "Emir\n10\n");
    expect_output("inheritance method",
                  "class Player:\n    func greet():\n        print(\"Player\")\n"
                  "class FootballPlayer : Player:\n    pass\np = FootballPlayer()\np.greet()\n",
                  "Player\n");
    expect_output("inheritance override",
                  "class Player:\n    func greet():\n        print(\"Player\")\n"
                  "class FootballPlayer : Player:\n    func greet():\n        print(\"FootballPlayer\")\n"
                  "p = FootballPlayer()\np.greet()\n",
                  "FootballPlayer\n");
    expect_output("inheritance super method",
                  "class Player:\n    func greet():\n        print(\"Player\")\n"
                  "class FootballPlayer : Player:\n    func greet():\n        super.greet()\n        print(\"FootballPlayer\")\n"
                  "p = FootballPlayer()\np.greet()\n",
                  "Player\nFootballPlayer\n");
    expect_output("inheritance parent method alias",
                  "class Player:\n    func greet():\n        print(\"Player\")\n"
                  "class FootballPlayer : Player:\n    func greet():\n        parent.greet()\n        print(\"FootballPlayer\")\n"
                  "p = FootballPlayer()\np.greet()\n",
                  "Player\nFootballPlayer\n");
    expect_output("inheritance constructor chaining",
                  "class Player:\n    public string name\n    constructor(name):\n        this.name = name\n"
                  "class FootballPlayer : Player:\n    public int number\n    constructor(name, number):\n"
                  "        super(name)\n        this.number = number\n"
                  "p = FootballPlayer(\"Emir\", 10)\nprint(p.name)\nprint(p.number)\n",
                  "Emir\n10\n");
    expect_output("inheritance parent constructor alias",
                  "class Player:\n    public string name\n    constructor(name):\n        this.name = name\n"
                  "class FootballPlayer : Player:\n    public int number\n    constructor(name, number):\n"
                  "        parent(name)\n        this.number = number\n"
                  "p = FootballPlayer(\"Emir\", 10)\nprint(p.name)\n",
                  "Emir\n");
    expect_output("inheritance forward parent",
                  "class FootballPlayer : Player:\n    public int number\nclass Player:\n    public string name\n"
                  "p = FootballPlayer()\np.name = \"Emir\"\nprint(p.name)\n",
                  "Emir\n");
    expect_output("inheritance protected child access",
                  "class Player:\n    protected int score\n    constructor():\n        this.score = 1\n"
                  "class FootballPlayer : Player:\n    func bump():\n        this.score += 1\n    func get():\n"
                  "        this.score\np = FootballPlayer()\np.bump()\nprint(p.get())\n",
                  "2\n");
    expect_output("inheritance three level chain",
                  "class A:\n    public int a\nclass B : A:\n    public int b\nclass C : B:\n    public int c\n"
                  "o = C()\no.a = 1\no.b = 2\no.c = 3\nprint(o.a)\nprint(o.b)\nprint(o.c)\n",
                  "1\n2\n3\n");
    expect_output("inheritance this parent field in child method",
                  "class Player:\n    public string name\nclass FootballPlayer : Player:\n    func setup():\n"
                  "        this.name = \"Emir\"\np = FootballPlayer()\np.setup()\nprint(p.name)\n",
                  "Emir\n");

    {
        try {
            compile("class Player:\n    protected int score\np = Player()\nprint(p.score)\n");
            fail_test("inheritance protected external read", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("cannot access protected field") == std::string_view::npos) {
                fail_test("inheritance protected external read", err.message());
            } else {
                pass_test("inheritance protected external read");
            }
        }
    }
    {
        try {
            compile("class Player:\n    private int secret\nclass FootballPlayer : Player:\n    func test():\n"
                      "        this.secret\n");
            fail_test("inheritance private child read", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("cannot access private field") == std::string_view::npos) {
                fail_test("inheritance private child read", err.message());
            } else {
                pass_test("inheritance private child read");
            }
        }
    }
    {
        try {
            compile("class FootballPlayer : Player:\n    pass\n");
            fail_test("inheritance unknown parent", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("unknown base type 'Player'") == std::string_view::npos) {
                fail_test("inheritance unknown parent", err.message());
            } else {
                pass_test("inheritance unknown parent");
            }
        }
    }
    {
        try {
            compile("class A : B:\n    int x\nclass B : A:\n    int y\n");
            fail_test("inheritance circular", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("circular inheritance") == std::string_view::npos) {
                fail_test("inheritance circular", err.message());
            } else {
                pass_test("inheritance circular");
            }
        }
    }
    {
        try {
            compile("class A:\n    pass\nclass B:\n    pass\nclass Child : A, B:\n    pass\n");
            fail_test("inheritance multiple", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("multiple inheritance is not supported") == std::string_view::npos) {
                fail_test("inheritance multiple", err.message());
            } else {
                pass_test("inheritance multiple");
            }
        }
    }
    {
        try {
            compile("super.greet()\n");
            fail_test("inheritance invalid super outside class", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("invalid use of 'super'") == std::string_view::npos) {
                fail_test("inheritance invalid super outside class", err.message());
            } else {
                pass_test("inheritance invalid super outside class");
            }
        }
    }
    {
        try {
            compile("class Player:\n    func greet():\n        pass\nclass FootballPlayer : Player:\n    func greet():\n"
                      "        super()\n");
            fail_test("inheritance invalid super constructor in method", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("invalid use of 'super'") == std::string_view::npos) {
                fail_test("inheritance invalid super constructor in method", err.message());
            } else {
                pass_test("inheritance invalid super constructor in method");
            }
        }
    }
    {
        try {
            compile("class Player:\n    string name\n    constructor(name):\n        this.name = name\n"
                      "class FootballPlayer : Player:\n    int number\n    constructor(name, number):\n"
                      "        this.number = number\n");
            fail_test("inheritance missing parent constructor call", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("missing required parent constructor call") ==
                std::string_view::npos) {
                fail_test("inheritance missing parent constructor call", err.message());
            } else {
                pass_test("inheritance missing parent constructor call");
            }
        }
    }
    {
        try {
            compile("class Player:\n    string name\nclass FootballPlayer : Player:\n    int number\n"
                      "p = FootballPlayer()\nprint(p.unknown)\n");
            fail_test("inheritance unknown field", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("unknown field 'unknown'") == std::string_view::npos) {
                fail_test("inheritance unknown field", err.message());
            } else {
                pass_test("inheritance unknown field");
            }
        }
    }
    {
        try {
            compile("class Player:\n    func greet():\n        pass\nclass FootballPlayer : Player:\n    pass\n"
                      "p = FootballPlayer()\np.missing()\n");
            fail_test("inheritance unknown method", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("unknown method 'missing'") == std::string_view::npos) {
                fail_test("inheritance unknown method", err.message());
            } else {
                pass_test("inheritance unknown method");
            }
        }
    }

    {
        try {
            compile("struct Player:\n    private secret\np = Player()\np.secret = 1\n");
            fail_test("private field write", "expected compile error");
        } catch (const pscript::CompilerError& err) {
            if (std::string_view(err.message()).find("cannot access private field") == std::string_view::npos) {
                fail_test("private field write", err.message());
            } else {
                pass_test("private field write");
            }
        }
    }

    expect_output("destructure list literal",
                  "[a, b, c] = [10, 20, 30]\nprint(a)\nprint(b)\nprint(c)\n", "10\n20\n30\n");
    expect_output("destructure list runtime",
                  "[a, b] = get_pair()\nprint(a)\nprint(b)\nfunc get_pair():\n    return [1, 2]\n",
                  "1\n2\n");
    expect_output("destructure tuple literal",
                  "(a, b) = (10, 20)\nprint(a)\nprint(b)\n", "10\n20\n");
    expect_output("destructure tuple three",
                  "(a, b, c) = (1, 2, 3)\nprint(a)\nprint(b)\nprint(c)\n", "1\n2\n3\n");
    expect_output("destructure nested",
                  "[a, (b, c)] = [10, (20, 30)]\nprint(a)\nprint(b)\nprint(c)\n", "10\n20\n30\n");
    expect_output("destructure rhs evaluated once",
                  "calls = 0\nfunc get_values():\n    calls = calls + 1\n    return [calls, calls + 1]\n"
                  "func run():\n    [a, b] = get_values()\n    print(calls)\n    print(a)\n    print(b)\n"
                  "run()\n",
                  "1\n1\n2\n");
    expect_output("destructure scope",
                  "outer = 0\nif true:\n    [x, y] = [3, 4]\n    outer = x + y\nprint(outer)\n", "7\n");
    expect_output("function add return regression",
                  "func add(a, b):\n    a + b\nprint(add(2, 3))\n", "5\n");
    expect_output("void function auto return regression",
                  "func hello():\n    print(\"hello\")\nfunc wrapper():\n    hello()\nwrapper()\nprint(\"ok\")\n",
                  "hello\nok\n");

    {
        try {
            compile("[a, b] = [1, 2, 3]\n");
            fail_test("destructure list mismatch compile", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("destructuring mismatch") == std::string_view::npos) {
                fail_test("destructure list mismatch compile", err.message());
            } else {
                pass_test("destructure list mismatch compile");
            }
        }
    }
    {
        try {
            compile("(a, b) = (1,)\n");
            fail_test("destructure tuple mismatch compile", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("destructuring mismatch") == std::string_view::npos) {
                fail_test("destructure tuple mismatch compile", err.message());
            } else {
                pass_test("destructure tuple mismatch compile");
            }
        }
    }
    {
        expect_unhandled("destructure runtime mismatch",
                         compile("[a, b] = one()\nfunc one():\n    return [1]\n"),
                         "destructuring mismatch");
    }
    {
        expect_unhandled("destructure invalid value", compile("[a] = 1\n"), "cannot destructure value");
    }

    expect_output("destructure wildcard list",
                  "[_, b, _] = [10, 20, 30]\nprint(b)\n", "20\n");
    expect_output("destructure wildcard tuple",
                  "(_, b) = (10, 20)\nprint(b)\n", "20\n");
    expect_output("destructure nested wildcard",
                  "[a, (_, c)] = [10, (20, 30)]\nprint(a)\nprint(c)\n", "10\n30\n");
    expect_output("destructure list rest basic",
                  "[a, *rest] = [1, 2, 3, 4]\nprint(a)\nprint(rest)\n", "1\n[2, 3, 4]\n");
    expect_output("destructure list rest two fixed",
                  "[a, b, *rest] = [1, 2, 3, 4, 5]\nprint(a)\nprint(b)\nprint(rest)\n",
                  "1\n2\n[3, 4, 5]\n");
    expect_output("destructure list rest empty",
                  "[a, *rest] = [1]\nprint(a)\nprint(rest)\nprint(rest.length())\n", "1\n[]\n0\n");
    expect_output("destructure tuple rest",
                  "(a, *rest) = (1, 2, 3, 4)\nprint(a)\nprint(rest)\n", "1\n[2, 3, 4]\n");
    expect_output("destructure nested tuple rest",
                  "[a, (b, *rest)] = [10, (20, 30, 40)]\nprint(a)\nprint(b)\nprint(rest)\n",
                  "10\n20\n[30, 40]\n");
    expect_output("destructure rest rhs evaluated once",
                  "calls = 0\nfunc get_values():\n    calls = calls + 1\n    return [calls, calls + 1, calls + 2]\n"
                  "func run():\n    [a, *rest] = get_values()\n    print(calls)\n    print(a)\n    print(rest)\n"
                  "run()\n",
                  "1\n1\n[2, 3]\n");
    expect_output("destructure rest with wildcard",
                  "[_, *rest] = [1, 2, 3]\nprint(rest)\n", "[2, 3]\n");
    {
        try {
            compile("[a, b] = [1, 2, 3]\n");
            fail_test("destructure exact mismatch regression", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("destructuring mismatch") == std::string_view::npos) {
                fail_test("destructure exact mismatch regression", err.message());
            } else {
                pass_test("destructure exact mismatch regression");
            }
        }
    }
    {
        try {
            compile("[*a, b] = [1, 2]\n");
            fail_test("destructure rest first compile", "expected compile error");
        } catch (const std::exception& err) {
            if (std::string_view(err.what()).find("rest pattern must be last") == std::string_view::npos) {
                fail_test("destructure rest first compile", err.what());
            } else {
                pass_test("destructure rest first compile");
            }
        }
    }
    {
        try {
            compile("[a, *r, *s] = [1, 2, 3]\n");
            fail_test("destructure multiple rest compile", "expected compile error");
        } catch (const std::exception& err) {
            if (std::string_view(err.what()).find("rest pattern must be last") == std::string_view::npos) {
                fail_test("destructure multiple rest compile", err.what());
            } else {
                pass_test("destructure multiple rest compile");
            }
        }
    }
    {
        try {
            compile("[_, b] = [1, 2]\nprint(_)\n");
            fail_test("destructure wildcard no slot", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("undefined variable") == std::string_view::npos) {
                fail_test("destructure wildcard no slot", err.message());
            } else {
                pass_test("destructure wildcard no slot");
            }
        }
    }

    expect_output("destructure dict basic",
                  "{\"name\": name, \"age\": age} = {\"name\": \"Emir\", \"age\": 30}\nprint(name)\nprint(age)\n",
                  "Emir\n30\n");
    expect_output("destructure dict wildcard",
                  "{\"name\": _, \"age\": age} = {\"name\": \"Emir\", \"age\": 30}\nprint(age)\n", "30\n");
    expect_output("destructure dict rest",
                  "{\"name\": name, *rest} = {\"name\": \"Emir\", \"age\": 30}\nprint(name)\nprint(rest)\n",
                  "Emir\n{\"age\": 30}\n");
    expect_output("destructure nested dict",
                  "{\"user\": {\"name\": name}} = {\"user\": {\"name\": \"Bob\"}}\nprint(name)\n", "Bob\n");
    expect_output("destructure dict rhs evaluated once",
                  "calls = 0\nfunc get_data():\n    calls = calls + 1\n    return {\"x\": 1}\n"
                  "{\"x\": x} = get_data()\nprint(calls)\nprint(x)\n",
                  "1\n1\n");
    expect_output("destructure struct basic",
                  "struct Player:\n    string name\n    int age\np = Player()\np.name = \"Emir\"\np.age = 30\n"
                  "Player{name, age} = p\nprint(name)\nprint(age)\n",
                  "Emir\n30\n");
    expect_output("destructure struct nested field",
                  "struct Address:\n    string city\nstruct Player:\n    string name\n    Address address\n"
                  "p = Player()\np.name = \"Emir\"\np.address = Address()\np.address.city = \"Istanbul\"\n"
                  "Player{name, address: Address{city}} = p\nprint(name)\nprint(city)\n",
                  "Emir\nIstanbul\n");
    expect_output("destructure param tuple",
                  "func add((a, b)):\n    a + b\nprint(add((10, 20)))\n", "30\n");
    expect_output("destructure param list",
                  "func first([a, b]):\n    return a\nprint(first([3, 4]))\n", "3\n");
    expect_output("destructure param dict",
                  "func greet({\"name\": name}):\n    print(name)\nfunc run():\n    greet({\"name\": \"hi\"})\n"
                  "run()\n",
                  "hi\n");
    expect_output("destructure param struct",
                  "struct Point:\n    int x\nfunc read_x(Point{x}):\n    return x\np = Point()\np.x = 5\n"
                  "print(read_x(p))\n",
                  "5\n");
    expect_output("destructure param nested",
                  "func pair([a, (b, c)]):\n    print(a)\n    print(b)\n    print(c)\n"
                  "pair([1, (2, 3)])\n",
                  "1\n2\n3\n");
    expect_output("destructure param rest",
                  "func tail([a, *rest]):\n    print(a)\n    print(rest)\nfunc run():\n    tail([1, 2, 3])\n"
                  "run()\n",
                  "1\n[2, 3]\n");
    expect_output("destructure param recursion",
                  "func walk([h, *rest]):\n    if rest.length() == 0:\n        return h\n    return h + walk(rest)\n"
                  "print(walk([1, 2, 3]))\n",
                  "6\n");
    {
        try {
            compile("{\"a\": x} = {\"b\": 1}\n");
            fail_test("destructure dict missing key compile", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("undefined dictionary key") == std::string_view::npos) {
                fail_test("destructure dict missing key compile", err.message());
            } else {
                pass_test("destructure dict missing key compile");
            }
        }
    }
    {
        try {
            compile("struct Player:\n    private int secret\np = Player()\nPlayer{secret} = p\n");
            fail_test("destructure struct private field", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("cannot access private field") == std::string_view::npos) {
                fail_test("destructure struct private field", err.message());
            } else {
                pass_test("destructure struct private field");
            }
        }
    }
    {
        try {
            compile("struct Player:\n    int x\nPlayer{missing} = Player()\n");
            fail_test("destructure struct unknown field", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("unknown field") == std::string_view::npos) {
                fail_test("destructure struct unknown field", err.message());
            } else {
                pass_test("destructure struct unknown field");
            }
        }
    }
    {
        try {
            compile("func bad([*rest, a]):\n    pass\n");
            fail_test("destructure param rest first", "expected compile error");
        } catch (const std::exception& err) {
            if (std::string_view(err.what()).find("rest pattern must be last") == std::string_view::npos) {
                fail_test("destructure param rest first", err.what());
            } else {
                pass_test("destructure param rest first");
            }
        }
    }
    {
        try {
            compile("func dup((a, a)):\n    pass\n");
            fail_test("destructure param duplicate binding", "expected compile error");
        } catch (const std::exception& err) {
            if (std::string_view(err.what()).find("duplicate binding") == std::string_view::npos) {
                fail_test("destructure param duplicate binding", err.what());
            } else {
                pass_test("destructure param duplicate binding");
            }
        }
    }

    expect_unhandled("bitwise float", compile("print(1.5 & 1)"), "unsupported operand type");
    expect_unhandled("bitwise string", compile("print(\"a\" | 1)"), "unsupported operand type");
    expect_unhandled("bitwise bool", compile("print(true ^ 1)"), "unsupported operand type");
    expect_unhandled("invalid shift", compile("print(1 << -1)"), "invalid shift");
    expect_unhandled("membership int", compile("print(1 in 1)"), "cannot use 'in' with int");
    expect_unhandled("invalid function value call", compile("x = 1\nx()\n"), "invalid function value call");
    expect_unhandled("wrong argument count through value",
                     compile("func add(a, b):\n    return a + b\nf = add\nf(1)\n"), "'add' expects 2 arguments");

    expect_output("any basic int", "any x = 10\nprint(x)\n", "10\n");
    expect_output("any reassignment types", "any x = 10\nx = \"hello\"\nprint(x)\n", "hello\n");
    expect_output("any null", "any x = null\nx = 10\nprint(x)\n", "10\n");
    expect_output("inferred int stays int", "x = 10\nx = 20\nprint(x)\n", "20\n");
    expect_output("int to float assignment", "float x = 10\nprint(x / 3)\n", "3.33333\n");
    expect_output("int to float param", "func test(float x):\n    print(x / 3)\ntest(10)\n", "3.33333\n");
    expect_output("any to int runtime", "any x = 10\nint y = x\nprint(y)\n", "10\n");
    expect_output("class child to parent assign",
                  "class Animal:\n    pass\nclass Dog : Animal:\n    pass\nAnimal a = Dog()\nprint(a)\n", "Dog{}\n");
    expect_output("class child to parent param",
                  "class Animal:\n    pass\nclass Dog : Animal:\n    pass\n"
                  "func take(Animal a):\n    print(\"ok\")\ntake(Dog())\n",
                  "ok\n");
    expect_output("class multilevel inheritance",
                  "class Animal:\n    pass\nclass Mammal : Animal:\n    pass\nclass Dog : Mammal:\n    pass\n"
                  "Mammal m = Dog()\nprint(m)\n",
                  "Dog{}\n");
    expect_output("any to class runtime",
                  "class Animal:\n    pass\nclass Dog : Animal:\n    pass\nany x = Dog()\nAnimal a = x\nprint(a)\n",
                  "Dog{}\n");
    expect_unhandled("any to class mismatch",
                     compile("class Animal:\n    pass\nany x = 10\nAnimal a = x\n"),
                     "Type error: expected Animal, got int");
    expect_unhandled("any operator mismatch", compile("any x = 10\nprint(x + \"hello\")\n"), "unsupported operand type");
    {
        try {
            compile("class Animal:\n    pass\nclass Dog : Animal:\n    pass\nDog d = Animal()\n");
            fail_test("parent to child assign", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("Type error") == std::string_view::npos) {
                fail_test("parent to child assign", err.message());
            } else {
                pass_test("parent to child assign");
            }
        }
    }
    {
        try {
            compile("int y = 10\ny = \"hello\"\n");
            fail_test("inferred wrong reassign", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("Type error") == std::string_view::npos) {
                fail_test("inferred wrong reassign", err.message());
            } else {
                pass_test("inferred wrong reassign");
            }
        }
    }

    expect_output("generic list int assign", "list<int> scores = [10, 20, 30]\nprint(scores[0])\n", "10\n");
    expect_output("generic list string assign", "list<string> names = [\"a\", \"b\"]\nprint(names[1])\n", "b\n");
    expect_output("generic map assign", "map<string, int> scores = {\"Emir\": 100}\nprint(scores[\"Emir\"])\n", "100\n");
    expect_output("generic empty list typed", "list<int> values = []\nvalues.append(1)\nprint(values)\n", "[1]\n");
    expect_output("generic empty map typed", "map<string, int> scores = {}\nscores[\"x\"] = 5\nprint(scores[\"x\"])\n",
                  "5\n");
    expect_output("generic list inference numeric", "values = [1, 2.5]\nprint(values)\n", "[1, 2.5]\n");
    expect_output("generic nested list", "list<list<int>> matrix = [[1, 2]]\nprint(matrix[0][0])\n", "1\n");
    expect_output("generic map nested value", "map<string, list<int>> data = {\"row\": [1, 2]}\nprint(data[\"row\"][1])\n",
                  "2\n");
    expect_output("generic function param", "func sum(list<int> values):\n    print(values[0] + values[1])\n"
                                            "sum([1, 2])\n",
                  "3\n");
    expect_output("any to generic list runtime", "any x = [1, 2, 3]\nlist<int> numbers = x\nprint(numbers[2])\n", "3\n");
    expect_unhandled("any to generic list mismatch", compile("any x = [\"hello\"]\nlist<int> numbers = x\n"),
                      "Type error: expected int, got string");
    {
        try {
            compile("list<int> scores = [\"a\", \"b\"]\n");
            fail_test("generic list wrong element", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("Type error") == std::string_view::npos) {
                fail_test("generic list wrong element", err.message());
            } else {
                pass_test("generic list wrong element");
            }
        }
    }
    {
        try {
            compile("map<string, int> scores = {\"Emir\": \"100\"}\n");
            fail_test("generic map wrong value", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("Type error") == std::string_view::npos) {
                fail_test("generic map wrong value", err.message());
            } else {
                pass_test("generic map wrong value");
            }
        }
    }
    {
        try {
            compile("list<int> values = [1, 2]\nvalues.append(\"hello\")\n");
            fail_test("generic list append mismatch", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("Type error") == std::string_view::npos) {
                fail_test("generic list append mismatch", err.message());
            } else {
                pass_test("generic list append mismatch");
            }
        }
    }
    {
        try {
            compile("func sum(list<int> values):\n    pass\nsum([\"a\", \"b\"])\n");
            fail_test("generic function wrong call", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("Type error") == std::string_view::npos) {
                fail_test("generic function wrong call", err.message());
            } else {
                pass_test("generic function wrong call");
            }
        }
    }
    {
        try {
            compile("list<list<int>> x = [[\"a\"]]\n");
            fail_test("generic nested mismatch", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("Type error") == std::string_view::npos) {
                fail_test("generic nested mismatch", err.message());
            } else {
                pass_test("generic nested mismatch");
            }
        }
    }

    expect_output("type primitives",
                  "print(type(10))\nprint(type(3.14))\nprint(type(\"hello\"))\nprint(type(true))\nprint(type(null))\n",
                  "int\nfloat\nstring\nbool\nnull\n");
    expect_output("type collections",
                  "print(type([1, 2, 3]))\nprint(type({\"score\": 100}))\nprint(type((1,)))\nprint(type({1}))\n",
                  "list\ndict\ntuple\nset\n");
    expect_output("typed list type runtime",
                  "list<int> values = [1, 2, 3]\nprint(type(values))\n",
                  "list\n");
    expect_output("conversion string numeric",
                  "print(int(\"42\"))\nprint(float(\"3.14\"))\nprint(str(123))\nprint(bool(1))\n",
                  "42\n3.14\n123\ntrue\n");
    expect_output("any conversion int", "any x = \"42\"\nint y = int(x)\nprint(y)\n", "42\n");
    expect_output("any conversion str", "any x = 42\nstring y = str(x)\nprint(y)\n", "42\n");
    expect_output("generic conversion list",
                  "list<int> values = [int(\"1\"), int(\"2\")]\nprint(values)\n",
                  "[1, 2]\n");
    expect_output("type struct enum function",
                  "struct Item:\n    value\nenum Color:\n    RED\nfunc noop():\n    pass\n"
                  "print(type(Item()))\nprint(type(Color.RED))\nprint(type(noop))\n",
                  "struct\nenum\nfunction\n");
    expect_unhandled("invalid int string", compile("print(int(\"hello\"))\n"), "cannot convert string to int");
    expect_unhandled("unsupported list to float", compile("print(float([1]))\n"), "cannot convert list to float");

    expect_output("throw string caught", "try:\n    throw \"error\"\ncatch error:\n    print(error)\n", "error\n");
    expect_output("throw int caught", "try:\n    throw 42\ncatch error:\n    print(error)\n", "42\n");
    expect_output("throw float caught", "try:\n    throw 3.14\ncatch error:\n    print(error)\n", "3.14\n");
    expect_output("throw bool caught", "try:\n    throw true\ncatch error:\n    print(error)\n", "true\n");
    expect_output("throw null caught", "try:\n    throw null\ncatch error:\n    print(error)\n", "null\n");
    expect_output("throw variable caught", "msg = \"boom\"\ntry:\n    throw msg\ncatch error:\n    print(error)\n",
                  "boom\n");
    expect_output("throw expression caught", "try:\n    throw 1 + 2\ncatch error:\n    print(error)\n", "3\n");
    expect_unhandled("throw propagation same function",
                     compile("func f():\n    throw \"error\"\nf()\nprint(\"after\")\n"),
                     "error");
    expect_output("try finally normal", "try:\n    print(\"try\")\nfinally:\n    print(\"finally\")\nprint(\"after\")\n",
                  "try\nfinally\nafter\n");
    expect_output("finally runs once",
                  "n = 0\ntry:\n    n = 1\nfinally:\n    n = n + 1\nprint(n)\n", "2\n");
    expect_output("throw catch finally",
                  "try:\n    throw \"error\"\ncatch error:\n    print(error)\nfinally:\n    print(\"finally\")\n"
                  "print(\"after\")\n",
                  "error\nfinally\nafter\n");
    {
        std::ostringstream out;
        try {
            Vm vm(compile("try:\n    throw \"error\"\nfinally:\n    print(\"finally\")\n"), out);
            vm.run();
            fail_test("unhandled throw finally", "expected VmError");
        } catch (const VmError& err) {
            if (out.str() != "finally\n") {
                fail_test("unhandled throw finally", "output mismatch");
            } else if (err.message().find("Unhandled exception:") == std::string::npos ||
                       err.message().find("error") == std::string::npos) {
                fail_test("unhandled throw finally", err.message());
            } else {
                pass_test("unhandled throw finally");
            }
        } catch (const std::exception& ex) {
            fail_test("unhandled throw finally", ex.what());
        }
    }
    expect_output("throw caller catch",
                  "func inner():\n    throw \"error\"\ntry:\n    inner()\ncatch error:\n    print(error)\n",
                  "error\n");
    expect_output("throw multi frame catch",
                  "func inner():\n    throw \"error\"\nfunc outer():\n    inner()\ntry:\n    outer()\ncatch error:\n"
                  "    print(error)\n",
                  "error\n");
    expect_output("nested try outer catch",
                  "try:\n    try:\n        throw \"inner\"\n    catch error:\n        throw \"outer\"\ncatch error:\n"
                  "    print(error)\n",
                  "outer\n");
    expect_output("try catch brace syntax",
                  "try {\n    throw \"error\"\n} catch error {\n    print(error)\n}\n",
                  "error\n");
    expect_output("catch continues after",
                  "try:\n    throw \"x\"\ncatch error:\n    print(error)\nprint(\"done\")\n", "x\ndone\n");
    expect_builtin_output("runtime error caught TypeException",
                          "try:\n    int(\"hello\")\ncatch TypeException error:\n    print(error.message)\n",
                          "cannot convert string to int\n");
    {
        try {
            compile("try:\n    throw \"error\"\ncatch error:\n    print(error)\nprint(error)\n");
            fail_test("catch scope violation", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("undefined variable") == std::string_view::npos) {
                fail_test("catch scope violation", err.message());
            } else {
                pass_test("catch scope violation");
            }
        }
    }
    {
        try {
            compile("x = throw \"error\"\n");
            fail_test("throw not expression", "expected parse or compile error");
        } catch (const std::exception& err) {
            const std::string msg = err.what();
            if (msg.find("expected expression") != std::string::npos ||
                msg.find("invalid") != std::string::npos) {
                pass_test("throw not expression");
            } else {
                fail_test("throw not expression", msg);
            }
        }
    }

    expect_builtin_output("builtin exception throw catch",
                          "try:\n    throw ValueException(\"bad\", null)\ncatch ValueException error:\n"
                          "    print(error.message)\n",
                          "bad\n");
    expect_builtin_output("typed catch parent match",
                          "class ChildError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                          "try:\n    throw ChildError(\"child\", null)\ncatch Exception error:\n    print(error.message)\n",
                          "child\n");
    expect_builtin_output("typed catch exact match",
                          "class ChildError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                          "try:\n    throw ChildError(\"child\", null)\ncatch ChildError error:\n    print(error.message)\n",
                          "child\n");
    expect_builtin_output("typed catch ordering",
                          "class ChildError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                          "try:\n    throw ChildError(\"child\", null)\ncatch ChildError error:\n    print(\"child\")\n"
                          "catch Exception error:\n    print(\"parent\")\n",
                          "child\n");
    {
        try {
            run_builtins("class ChildError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                         "try:\n    throw ChildError(\"child\", null)\ncatch TypeException error:\n    print(error)\n");
            fail_test("typed catch unrelated", "expected VmError");
        } catch (const VmError& err) {
            if (err.message().find("Unhandled exception:") == std::string::npos) {
                fail_test("typed catch unrelated", err.message());
            } else {
                pass_test("typed catch unrelated");
            }
        } catch (const std::exception& ex) {
            fail_test("typed catch unrelated", ex.what());
        }
    }
    expect_builtin_output("rethrow outer catch",
                          "class MyError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                          "try:\n    throw MyError(\"boom\", null)\ncatch MyError error:\n    throw error\ncatch Exception error:\n"
                          "    print(error.message)\n",
                          "boom\n");
    expect_builtin_output("exception chaining cause",
                          "class MyError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                          "try:\n    throw ValueException(\"inner\", null)\ncatch Exception error:\n"
                          "    wrapped = MyError(\"outer\", error)\n    print(wrapped.message)\n    print(wrapped.cause.message)\n",
                          "outer\ninner\n");
    expect_builtin_output("user exception fields",
                          "class MyError : Exception:\n    public int code\n    constructor(message, cause, code):\n        parent(message, cause)\n"
                          "        this.code = code\nerr = MyError(\"fail\", null, 42)\nprint(err.code)\n",
                          "42\n");
    expect_builtin_output("finally return preserves value",
                          "func test():\n    try:\n        return 10\n    finally:\n        print(\"cleanup\")\n"
                          "print(test())\n",
                          "cleanup\n10\n");
    expect_builtin_output("finally break in loop",
                          "while true:\n    try:\n        break\n    finally:\n        print(\"cleanup\")\n    break\n"
                          "print(\"after\")\n",
                          "cleanup\nafter\n");
    expect_builtin_output("finally continue in loop",
                          "count = 0\nwhile count < 2:\n    try:\n        count = count + 1\n        continue\n    finally:\n"
                          "        print(\"cleanup\")\n    print(\"body\")\nprint(count)\n",
                          "cleanup\ncleanup\n2\n");
    expect_builtin_output("nested typed catch inner consumes",
                          "class MyError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                          "try:\n    try:\n        throw MyError(\"inner\", null)\n    catch MyError error:\n        print(\"inner\")\n"
                          "catch Exception error:\n    print(\"outer\")\n",
                          "inner\n");
    expect_builtin_output("exception through function",
                          "func inner():\n    throw ValueException(\"boom\", null)\n"
                          "try:\n    inner()\ncatch ValueException error:\n    print(error.message)\n",
                          "boom\n");
    expect_builtin_output("exception through closure",
                          "msg = \"boom\"\nfunc make():\n    func inner():\n        throw ValueException(msg, null)\n    return inner\n"
                          "f = make()\ntry:\n    f()\ncatch ValueException error:\n    print(error.message)\n",
                          "boom\n");
    expect_builtin_output("constructor throw unwind",
                          "class MyError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                          "class Bad : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n        throw MyError(\"ctor\", null)\n"
                          "try:\n    Bad(\"x\", null)\ncatch MyError error:\n    print(error.message)\n",
                          "ctor\n");
    expect_builtin_output("finally return overrides defer",
                          "func test():\n    try:\n        return 1\n    finally:\n        return 2\nprint(test())\n",
                          "2\n");

    expect_builtin_output("runtime error propagation caller catch",
                          "func test():\n    return int(\"hello\")\n"
                          "try:\n    test()\ncatch TypeException error:\n    print(error.message)\n",
                          "cannot convert string to int\n");
    expect_builtin_output("runtime error parent catch",
                          "try:\n    int(\"hello\")\ncatch RuntimeException error:\n    print(error.message)\n",
                          "cannot convert string to int\n");
    expect_builtin_output("runtime error NameException",
                          "try:\n    print({}[\"missing\"])\ncatch NameException error:\n    print(error.message)\n",
                          "undefined dictionary key\n");
    expect_builtin_output("runtime error IndexException",
                          "try:\n    print([1][5])\ncatch IndexException error:\n    print(error.message)\n",
                          "list index out of range\n");
    expect_builtin_output("runtime error ValueException",
                          "try:\n    print(1 / 0)\ncatch ValueException error:\n    print(error.message)\n",
                          "division by zero\n");
    {
        std::ostringstream out;
        try {
            Vm vm(compile_with_builtins("try:\n    int(\"hello\")\nfinally:\n    print(\"cleanup\")\n"), out);
            vm.run();
            fail_test("runtime error finally cleanup", "expected VmError");
        } catch (const VmError& err) {
            if (out.str() != "cleanup\n") {
                fail_test("runtime error finally cleanup", "output mismatch");
            } else if (err.message().find("Unhandled exception:") == std::string::npos) {
                fail_test("runtime error finally cleanup", err.message());
            } else {
                pass_test("runtime error finally cleanup");
            }
        } catch (const std::exception& ex) {
            fail_test("runtime error finally cleanup", ex.what());
        }
    }
    {
        std::ostringstream out;
        try {
            Vm vm(compile_with_builtins("try:\n    int(\"hello\")\nfinally:\n    print(\"cleanup\")\n", "test.phs"),
                  out);
            vm.run();
            fail_test("runtime error finally unhandled", "expected VmError");
        } catch (const VmError& err) {
            if (out.str() != "cleanup\n") {
                fail_test("runtime error finally unhandled", "output mismatch");
            } else if (err.message().find("Unhandled exception:") == std::string::npos ||
                       err.message().find("TypeException") == std::string::npos) {
                fail_test("runtime error finally unhandled", err.message());
            } else {
                pass_test("runtime error finally unhandled");
            }
        } catch (const std::exception& ex) {
            fail_test("runtime error finally unhandled", ex.what());
        }
    }
    {
        std::ostringstream out;
        try {
            Vm vm(compile_with_builtins("func inner():\n    int(\"x\")\nfunc outer():\n    inner()\nouter()\n",
                                        "trace.phs"),
                  out);
            vm.run();
            fail_test("runtime stack trace nested", "expected VmError");
        } catch (const VmError& err) {
            const std::string& msg = err.message();
            if (msg.find("at inner (trace.phs:") == std::string::npos ||
                msg.find("at outer (trace.phs:") == std::string::npos ||
                msg.find("at main (trace.phs:") == std::string::npos) {
                fail_test("runtime stack trace nested", msg);
            } else {
                pass_test("runtime stack trace nested");
            }
        } catch (const std::exception& ex) {
            fail_test("runtime stack trace nested", ex.what());
        }
    }
    {
        std::ostringstream out;
        try {
            Vm vm(compile_with_builtins(
                      "class MyError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                      "func inner():\n    throw MyError(\"failure\", null)\nfunc outer():\n    inner()\nouter()\n",
                      "user.phs"),
                  out);
            vm.run();
            fail_test("user exception stack trace", "expected VmError");
        } catch (const VmError& err) {
            const std::string& msg = err.message();
            if (msg.find("Unhandled exception: MyError: failure") == std::string::npos ||
                msg.find("at inner (user.phs:") == std::string::npos ||
                msg.find("at outer (user.phs:") == std::string::npos) {
                fail_test("user exception stack trace", msg);
            } else {
                pass_test("user exception stack trace");
            }
        } catch (const std::exception& ex) {
            fail_test("user exception stack trace", ex.what());
        }
    }
    {
        std::ostringstream out;
        try {
            Vm vm(compile_with_builtins(
                      "class MyError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                      "try:\n    throw ValueException(\"inner\", null)\n"
                      "catch Exception error:\n    throw MyError(\"outer\", error)\n",
                      "chain.phs"),
                  out);
            vm.run();
            fail_test("chained unhandled exception", "expected VmError");
        } catch (const VmError& err) {
            const std::string& msg = err.message();
            if (msg.find("MyError: outer") == std::string::npos || msg.find("caused by:") == std::string::npos ||
                msg.find("ValueException: inner") == std::string::npos) {
                fail_test("chained unhandled exception", msg);
            } else {
                pass_test("chained unhandled exception");
            }
        } catch (const std::exception& ex) {
            fail_test("chained unhandled exception", ex.what());
        }
    }
    expect_unhandled_builtins("primitive throw unhandled", "throw \"oops\"\n", "oops");
    expect_unhandled_builtins("primitive int throw unhandled", "throw 42\n", "42");
    expect_builtin_output("throw inside catch propagates",
                          "class MyError : Exception:\n    constructor(message, cause):\n        parent(message, cause)\n"
                          "try:\n    try:\n        throw ValueException(\"inner\", null)\n    catch Exception error:\n"
                          "        throw MyError(\"outer\", error)\ncatch MyError error:\n    print(error.message)\n"
                          "    print(error.cause.message)\n",
                          "outer\ninner\n");

    expect_module_output("basic module namespace access",
                         "import math\nprint(math.add(2, 3))\nprint(math.value)\n",
                         {{"math.phs", "func add(a, b):\n    return a + b\nvalue = 42\n"}},
                         "5\n42\n");
    expect_module_output("module executes on first import",
                         "import config\nprint(config.value)\n",
                         {{"config.phs", "print(\"config loaded\")\nvalue = 42\n"}},
                         "config loaded\n42\n");
    expect_module_output("module state preserved",
                         "import counter\ncounter.increment()\ncounter.increment()\nprint(counter.value)\n",
                         {{"counter.phs", "value = 0\nfunc increment():\n    value += 1\n"}},
                         "2\n");
    expect_module_output("from import function",
                         "from math import add\nprint(add(2, 3))\n",
                         {{"math.phs", "func add(a, b):\n    return a + b\n"}},
                         "5\n");
    expect_module_output("module alias",
                         "import math as m\nprint(m.add(1, 2))\n",
                         {{"math.phs", "func add(a, b):\n    return a + b\n"}},
                         "3\n");
    expect_module_output("from import alias",
                         "from math import add as plus\nprint(plus(4, 5))\n",
                         {{"math.phs", "func add(a, b):\n    return a + b\n"}},
                         "9\n");
    expect_module_output("module scope isolation",
                         "import a\nimport b\nprint(a.secret)\nprint(b.secret)\n",
                         {{"a.phs", "secret = 10\n"}, {"b.phs", "secret = 20\n"}},
                         "10\n20\n");
    expect_module_output("nested module import",
                         "import outer\nprint(outer.greet())\n",
                         {{"outer.phs", "import inner\nfunc greet():\n    return inner.message\n"},
                          {"inner.phs", "message = \"hello\"\n"}},
                         "hello\n");
    expect_module_output("class import from module",
                         "from shapes import Box\nb = Box(3)\nprint(b.size)\n",
                         {{"shapes.phs",
                           "class Box:\n    public int size\n    constructor(size):\n        this.size = size\n"}},
                         "3\n");
    expect_module_output("module public export access",
                         "import math\nprint(math.value)\n",
                         {{"math.phs", "pub value = 42\npriv secret = 99\n"}},
                         "42\n");
    expect_module_output("module live namespace state",
                         "import counter\nfrom counter import value\ncounter.increment()\nprint(counter.value)\nprint(value)\n",
                         {{"counter.phs", "value = 0\nfunc increment():\n    value += 1\n"}},
                         "1\n0\n");
    expect_module_output("module public mutation",
                         "import config\nconfig.value = 20\nprint(config.value)\n",
                         {{"config.phs", "pub value = 10\n"}},
                         "20\n");
    expect_module_output("circular import partial access",
                         "import a\nprint(a.ready)\n",
                         {{"a.phs", "import b\nready = true\n"},
                          {"b.phs", "import a\n"}},
                         "true\n");
    expect_module_output("inheritance across modules",
                         "from base import Animal\nclass Dog : Animal:\n    func speak():\n        return \"woof\"\n"
                         "d = Dog()\nprint(d.speak())\n",
                         {{"base.phs", "pub class Animal:\n    func speak():\n        return \"...\"\n"}},
                         "woof\n");
    expect_module_output("enum import from module",
                         "from types import Color\nprint(Color.RED)\n",
                         {{"types.phs", "pub enum Color:\n    RED\n    GREEN\n"}},
                         "Color.RED\n");
    expect_module_output("struct import from module",
                         "from data import Player\np = Player()\np.name = \"A\"\nprint(p.name)\n",
                         {{"data.phs", "pub struct Player:\n    name\n"}},
                         "A\n");
    expect_module_output("module function exception caught in importer",
                         "from math import divide\ntry:\n    divide(1, 0)\ncatch error:\n    print(\"caught\")\n"
                         "print(\"after\")\n",
                         {{"math.phs", "func divide(a, b):\n    return a / b\n"}},
                         "caught\nafter\n");
    expect_module_output("module function finally return to importer",
                         "from math import compute\nprint(compute())\n",
                         {{"math.phs", "func compute():\n    try:\n        return 42\n    finally:\n        print(\"cleanup\")\n"}},
                         "cleanup\n42\n");
    expect_module_output("nested module calls exception to importer",
                         "import chain\ntry:\n    chain.run()\ncatch error:\n    print(\"caught\")\nprint(\"done\")\n",
                         {{"chain.phs", "import leaf\nfunc run():\n    return leaf.fail()\n"},
                          {"leaf.phs", "func fail():\n    throw \"boom\"\n"}},
                         "caught\ndone\n");
    expect_module_output("nested module calls finally on unwind",
                         "import chain\ntry:\n    chain.run()\ncatch error:\n    print(\"caught\")\nprint(\"done\")\n",
                         {{"chain.phs", "import leaf\nfunc run():\n    try:\n        return leaf.fail()\n    finally:\n        print(\"chain cleanup\")\n"},
                          {"leaf.phs", "func fail():\n    throw \"boom\"\n"}},
                         "chain cleanup\ncaught\ndone\n");
    {
        try {
            run_modules("import math\nprint(math.secret)\n", {{"math.phs", "pub value = 1\npriv secret = 2\n"}});
            fail_test("private module export runtime", "expected VmError");
        } catch (const VmError& err) {
            if (err.message().find("has no export 'secret'") == std::string::npos) {
                fail_test("private module export runtime", err.message());
            } else {
                pass_test("private module export runtime");
            }
        } catch (const std::exception& ex) {
            fail_test("private module export runtime", ex.what());
        }
    }
    {
        try {
            run_modules("import config\nconfig.secret = 5\n", {{"config.phs", "priv secret = 1\n"}});
            fail_test("private module mutation", "expected VmError");
        } catch (const VmError& err) {
            if (err.message().find("has no export 'secret'") == std::string::npos) {
                fail_test("private module mutation", err.message());
            } else {
                pass_test("private module mutation");
            }
        } catch (const std::exception& ex) {
            fail_test("private module mutation", ex.what());
        }
    }
    {
        try {
            run_modules("import missing\nprint(1)\n", {});
            fail_test("missing module runtime", "expected VmError");
        } catch (const VmError& err) {
            if (err.message().find("cannot find module 'missing'") == std::string::npos) {
                fail_test("missing module runtime", err.message());
            } else {
                pass_test("missing module runtime");
            }
        } catch (const std::exception& ex) {
            fail_test("missing module runtime", ex.what());
        }
    }
    {
        ModuleTempDir dir;
        dir.write("bad.phs", "print(1 / 0)\n");
        CompileOptions options;
        options.source_file = dir.main_path();
        std::ostringstream out;
        try {
            Vm vm(compile(parse("import bad\n"), options), out);
            vm.run();
            fail_test("module runtime error propagation", "expected VmError");
        } catch (const VmError& err) {
            if (err.message().find("division by zero") == std::string::npos) {
                fail_test("module runtime error propagation", err.message());
            } else {
                pass_test("module runtime error propagation");
            }
        } catch (const std::exception& ex) {
            fail_test("module runtime error propagation", ex.what());
        }
    }

    expect_output("top-level await async call",
                  "async func test():\n    print(1)\nawait test()\n",
                  "1\n");
    expect_output("async fire-and-forget after sync top-level",
                  "async func test():\n    print(\"a\")\nprint(\"before\")\ntest()\n",
                  "before\na\n");
    expect_output("top-level await async print a",
                  "async func test():\n    print(\"a\")\nawait test()\n",
                  "a\n");
    expect_output("top-level await async delay sequence",
                  "async func test():\n    await delay(1ms)\n    print(\"a\")\nawait test()\nprint(\"done\")\n",
                  "a\ndone\n");
    expect_output("top-level await return value",
                  "async func get_value():\n    return 42\nresult = await get_value()\nprint(result)\n",
                  "42\n");
    expect_output("top-level await delay",
                  "await delay(1ms)\nprint(\"done\")\n",
                  "done\n");
    expect_output("top-level delay task variable",
                  "task = delay(1ms)\nawait task\nprint(\"done\")\n",
                  "done\n");
    expect_output("top-level nested async",
                  "async func inner():\n    return 10\nasync func outer():\n    value = await inner()\n    return value + 5\n"
                  "print(await outer())\n",
                  "15\n");
    expect_output("async main basic", "async func main():\n    print(1)\n", "1\n");
    expect_output("await delay", "async func main():\n    await delay(1ms)\n    print(2)\n", "2\n");
    expect_output("await result", "async func value():\n    return 42\nasync func main():\n    x = await value()\n    print(x)\n",
                  "42\n");
    expect_output("async annotated return type",
                  "async func value() -> int:\n    return 42\nasync func main():\n    print(await value())\n",
                  "42\n");
    expect_output("function annotated return type",
                  "func add(a: int, b: int) -> int:\n    return a + b\nprint(add(2, 3))\n",
                  "5\n");
    expect_output("generic function inference",
                  "func identity<T>(value: T) -> T:\n    return value\nprint(identity(10))\nprint(identity(\"hi\"))\n",
                  "10\nhi\n");
    expect_output("generic struct inference",
                  "struct Pair<T, U>:\n    first: T\n    second: U\np = Pair(10, \"hi\")\nprint(p.first)\nprint(p.second)\n",
                  "10\nhi\n");
    expect_output("generic class explicit type",
                  "class Box<T>:\n    value: T\n    func get() -> T:\n        return value\n"
                  "b = Box<int>(10)\nprint(b.get())\n",
                  "10\n");
    expect_output("generic class inference",
                  "class Box<T>:\n    value: T\n    func get() -> T:\n        return value\n"
                  "print(Box(10).get())\n",
                  "10\n");
    expect_output("generic interface implementation",
                  "interface Container<T>:\n    func get() -> T\n"
                  "class IntBox : Container<int>:\n    value: int\n    func get() -> int:\n        return value\n"
                  "print(IntBox(7).get())\n",
                  "7\n");
    expect_output("multiple interface constraints",
                  "interface A:\n    func a()\ninterface B:\n    func b()\n"
                  "class C : A, B:\n    func a():\n        pass\n    func b():\n        pass\n"
                  "func use<T: A & B>(value: T):\n    value.a()\nuse(C())\n",
                  "");
    expect_output("generic match type bind",
                  "class Box<T>:\n    value: T\n"
                  "b = Box<int>(3)\nmatch b:\n    Box<int> x:\n        print(x.value)\n",
                  "3\n");
    expect_output("generic struct destructure",
                  "struct Pair<T, U>:\n    first: T\n    second: U\n"
                  "p = Pair(1, \"a\")\nPair<int, string> { first: a, second: b } = p\nprint(a)\nprint(b)\n",
                  "1\na\n");
    expect_module_output("generic module export",
                         "from shapes import Box\nb = Box(4)\nprint(b.value)\n",
                         {{"shapes.phs",
                           "class Box<T>:\n    pub value: T\n"}},
                         "4\n");
    expect_output("reflection annotations function",
                  "@deprecated(\"old\")\nfunc old():\n    pass\nprint(annotations(old).length())\n",
                  "1\n");
    expect_output("interface printable runtime",
                  "interface Printable:\n    func print()\nclass Player : Printable:\n    func print():\n        print(\"ok\")\n"
                  "Player().print()\n",
                  "ok\n");
    expect_output("generic constraint printable",
                  "interface Printable:\n    func print()\nclass Player : Printable:\n    func print():\n        print(\"p\")\n"
                  "func show<T: Printable>(value: T):\n    value.print()\nshow(Player())\n",
                  "p\n");
    expect_output("generic function value inference",
                  "func identity<T>(value: T) -> T:\n    return value\nf = identity\nprint(f(10))\n",
                  "10\n");
    expect_output("reflection type_of",
                  "class Box:\n    pass\nprint(type_of(Box()))\n",
                  "Box\n");
    expect_output("function overload int string",
                  "func show(x: int):\n    print(\"i\")\nfunc show(x: string):\n    print(\"s\")\nshow(1)\nshow(\"a\")\n",
                  "i\ns\n");
    expect_output("default parameter runtime",
                  "func greet(name: string, prefix: string = \"Hello\"):\n    print(prefix + \" \" + name)\n"
                  "greet(\"Emir\")\ngreet(\"Emir\", \"Hi\")\n",
                  "Hello Emir\nHi Emir\n");
    expect_output("variadic sum runtime",
                  "func sum(values: int...):\n    total = 0\n    for v in values:\n        total = total + v\n    print(total)\n"
                  "sum()\nsum(1, 2, 3)\n",
                  "0\n6\n");
    expect_output("union typed variable runtime",
                  "x: int | string = 10\nprint(x)\ny: int | string = \"hi\"\nprint(y)\n",
                  "10\nhi\n");
    expect_output("nested async calls",
                  "async func inner():\n    return 7\nasync func outer():\n    return await inner()\nasync func main():\n    print(await outer())\n",
                  "7\n");
    expect_output("cooperative timer tasks",
                  "async func main():\n    a = delay(1ms)\n    b = delay(1ms)\n    await a\n    await b\n    print(1)\n",
                  "1\n");
    expect_output("async closure capture",
                  "async func main():\n    n = 5\n    async func worker():\n        return n + 1\n    print(await worker())\n",
                  "6\n");
    expect_builtin_output("async exception propagation",
                          "async func fail():\n    throw ValueException(\"boom\", null)\nasync func main():\n    try:\n        await fail()\n    catch e:\n        print(e.message)\n",
                          "boom\n");

    expect_output("await completed task twice",
                  "async func once():\n    return 1\nasync func main():\n    t = once()\n    await t\n    print(await t)\n",
                  "1\n");
    expect_output("async double suspend resume",
                  "async func main():\n    await delay(1ms)\n    await delay(1ms)\n    print(1)\n",
                  "1\n");
    expect_output("scheduler interleaved tasks",
                  "async func a():\n    print(\"A1\")\n    await delay(2ms)\n    print(\"A2\")\nasync func b():\n    "
                  "print(\"B1\")\n    await delay(1ms)\n    print(\"B2\")\nasync func main():\n    ta = a()\n    tb = "
                  "b()\n    await tb\n    await ta\n",
                  "A1\nB1\nB2\nA2\n");
    expect_output("spawned task runs after async main body",
                  "async func bg():\n    print(\"bg\")\nasync func main():\n    bg()\n    print(\"main\")\n",
                  "main\nbg\n");
    expect_output("async recursive await",
                  "async func fact(n):\n    if n <= 1:\n        return 1\n    return n * await fact(n - 1)\nasync func "
                  "main():\n    print(await fact(5))\n",
                  "120\n");
    expect_output("async function value call",
                  "async func add(a, b):\n    return a + b\nasync func main():\n    f = add\n    print(await f(2, 3))\n",
                  "5\n");
    expect_output("nested async await chain",
                  "async func c():\n    return 3\nasync func b():\n    return await c()\nasync func a():\n    return "
                  "await b()\nasync func main():\n    print(await a())\n",
                  "3\n");
    expect_builtin_output("async finally await",
                          "async func main():\n    try:\n        await delay(1ms)\n        print(\"body\")\n    finally:\n"
                          "        print(\"finally\")\n    print(\"after\")\n",
                          "body\nfinally\nafter\n");
    expect_builtin_output("async return finally await",
                          "async func work():\n    try:\n        await delay(1ms)\n        return 99\n    finally:\n"
                          "        print(\"cleanup\")\nasync func main():\n    print(await work())\n",
                          "cleanup\n99\n");
    expect_builtin_output("async finally with inner await",
                          "async func main():\n    try:\n        await delay(1ms)\n    finally:\n        await delay(1ms)\n"
                          "        print(\"done\")\n",
                          "done\n");
    expect_builtin_output("async nested exception with cause",
                          "async func inner():\n    throw ValueException(\"inner\", null)\nasync func outer():\n    "
                          "await inner()\nasync func main():\n    try:\n        await outer()\n    catch e:\n        "
                          "print(e.message)\n",
                          "inner\n");
    expect_output("async class method",
                  "class Worker:\n    public int n\n    async func run():\n        await delay(1ms)\n        return this.n\n"
                  "async func main():\n    w = Worker()\n    w.n = 4\n    print(await w.run())\n",
                  "4\n");
    expect_output("async overridden method",
                  "class Base:\n    async func run():\n        return 1\nclass Derived : Base:\n    async func run():\n"
                  "        await delay(1ms)\n        return 2\nasync func main():\n    d = Derived()\n    print(await "
                  "d.run())\n",
                  "2\n");
    expect_module_output("module async function",
                         "import math\nasync func main():\n    print(await math.compute())\n",
                         {{"math.phs", "async func compute():\n    await delay(1ms)\n    return 10\n"}},
                         "10\n");
    expect_module_output("module async exception propagation",
                         "import math\nasync func main():\n    try:\n        await math.fail()\n    catch e:\n        "
                         "print(e)\n",
                         {{"math.phs", "async func fail():\n    throw \"mod\"\n"}},
                         "mod\n");
    expect_builtin_output("async nested closure",
                          "async func main():\n    base = 10\n    async func inner():\n        async func leaf():\n"
                          "            return base + 1\n        return await leaf()\n    print(await inner())\n",
                          "11\n");
    {
        std::ostringstream out;
        try {
            Vm vm(compile_with_builtins("async func fail():\n    throw ValueException(\"bg\", null)\nasync func main():\n"
                                        "    fail()\n    print(\"done\")\n"),
                  out);
            vm.run();
            fail_test("unhandled async background failure", "expected VmError");
        } catch (const VmError& err) {
            if (out.str() != "done\n" || err.message().find("Unhandled exception:") == std::string::npos ||
                err.message().find("bg") == std::string::npos) {
                fail_test("unhandled async background failure", err.message());
            } else {
                pass_test("unhandled async background failure");
            }
        } catch (const std::exception& ex) {
            fail_test("unhandled async background failure", ex.what());
        }
    }

    // Bölüm 3: Full Integration & Finalization
    expect_output("async nested concurrent regression",
                  "async func inner():\n    await delay(1ms)\n    async func nested():\n        await delay(1ms)\n        "
                  "return 1\n    return await nested()\nasync func outer():\n    await delay(1ms)\n    return await "
                  "inner()\nasync func main():\n    a = outer()\n    b = outer()\n    await a\n    await b\n    "
                  "print(1)\n",
                  "1\n");
    expect_output("async zero delay", "async func main():\n    await delay(0ms)\n    print(1)\n", "1\n");
    expect_output("delay duration seconds", "async func main():\n    await delay(1.5s)\n    print(1)\n", "1\n");
    expect_output("delay duration units zero",
                  "async func main():\n    await delay(0m)\n    await delay(0h)\n    print(1)\n", "1\n");
    expect_output("async calls sync inside",
                  "func sync_val():\n    return 10\nasync func main():\n    await delay(0ms)\n    print(sync_val())\n",
                  "10\n");
    expect_output("async while await break continue",
                  "async func main():\n    i = 0\n    while i < 5:\n        await delay(0ms)\n        i += 1\n        "
                  "if i == 2:\n            continue\n        if i == 4:\n            break\n        print(i)\n",
                  "1\n3\n");
    expect_output("async for await",
                  "async func main():\n    total = 0\n    for x in range(3):\n        await delay(0ms)\n        total += "
                  "x\n    print(total)\n",
                  "3\n");
    expect_output("async super method",
                  "class Base:\n    async func run():\n        return 1\nclass Derived : Base:\n    async func run():\n"
                  "        x = await super.run()\n        return x + 1\nasync func main():\n    d = Derived()\n    "
                  "print(await d.run())\n",
                  "2\n");
    expect_output("async mutable closure across await",
                  "async func main():\n    n = 0\n    async func bump():\n        n += 1\n        await delay(0ms)\n"
                  "        n += 1\n        return n\n    print(await bump())\n",
                  "2\n");
    expect_output("async multiple spawn sequential",
                  "async func work(v):\n    await delay(0ms)\n    return v\nasync func main():\n    t1 = work(1)\n    "
                  "t2 = work(2)\n    print(await t1)\n    print(await t2)\n",
                  "1\n2\n");
    {
        std::ostringstream out;
        try {
            Vm vm(compile_with_builtins("async func inner():\n    throw ValueException(\"leaf\", null)\nasync func outer():\n"
                                        "    await inner()\nasync func main():\n    await outer()\n"),
                  out);
            vm.run();
            fail_test("async stack trace nested", "expected VmError");
        } catch (const VmError& err) {
            if (err.message().find("at inner") == std::string::npos) {
                fail_test("async stack trace nested", err.message());
            } else {
                pass_test("async stack trace nested");
            }
        } catch (const std::exception& ex) {
            fail_test("async stack trace nested", ex.what());
        }
    }
    expect_module_output("module async class method",
                         "from svc import Worker\nasync func main():\n    w = Worker()\n    w.n = 7\n    print(await "
                         "w.run())\n",
                         {{"svc.phs", "class Worker:\n    public int n\n    async func run():\n        await delay(0ms)\n        "
                                     "return this.n\n"}},
                         "7\n");

    // Async Task Control — Update Bölüm 1
    expect_builtin_output("async child cancellation throw after await",
                          "async func work():\n    try:\n        await delay(0ms)\n        throw CancellationException(\"Task "
                          "was cancelled\", null)\n    finally:\n        print(\"cleanup\")\nasync func main():\n    try:\n"
                          "        await work()\n    catch e:\n        print(e.message)\n",
                          "cleanup\nTask was cancelled\n");
    expect_builtin_output("async child try finally throw after await",
                          "async func work():\n    try:\n        await delay(0ms)\n        throw ValueException(\"x\", null)\n"
                          "    finally:\n        print(\"cleanup\")\nasync func main():\n    try:\n        await work()\n"
                          "    catch e:\n        print(\"caught\")\n",
                          "cleanup\ncaught\n");
    expect_builtin_output("async child try finally throw",
                          "async func work():\n    try:\n        throw ValueException(\"x\", null)\n    finally:\n"
                          "        print(\"cleanup\")\nasync func main():\n    try:\n        await work()\n    catch e:\n"
                          "        print(\"caught\")\n",
                          "cleanup\ncaught\n");
    expect_builtin_output("async child try finally await",
                          "async func work():\n    try:\n        await delay(1ms)\n        print(\"body\")\n    finally:\n"
                          "        print(\"cleanup\")\nasync func main():\n    await work()\n    print(\"done\")\n",
                          "body\ncleanup\ndone\n");
    expect_builtin_output("task cancel suspended finally",
                          "async func work():\n    try:\n        await delay(100ms)\n        print(\"body\")\n    finally:\n"
                          "        print(\"cleanup\")\nasync func main():\n    t = work()\n    await delay(0ms)\n    "
                          "t.cancel()\n    try:\n        await t\n    catch e:\n        print(e.message)\n",
                          "cleanup\nTask was cancelled\n");
    expect_builtin_output("task cancel pending finally",
                          "async func gate():\n    await delay(50ms)\nasync func work():\n    try:\n        await delay(10ms)\n"
                          "        print(\"body\")\n    finally:\n        print(\"cleanup\")\nasync func main():\n    "
                          "g = gate()\n    await delay(0ms)\n    t = work()\n    t.cancel()\n    try:\n        await t\n"
                          "    catch e:\n        print(\"cancelled\")\n    await g\n",
                          "cleanup\ncancelled\n");
    expect_builtin_output("task cancel double noop",
                          "async func work():\n    try:\n        await delay(100ms)\n    finally:\n        print(\"once\")\n"
                          "async func main():\n    t = work()\n    await delay(0ms)\n    t.cancel()\n    t.cancel()\n"
                          "    try:\n        await t\n    catch e:\n        print(\"cancelled\")\n",
                          "once\ncancelled\n");
    expect_builtin_output("task cancel completed noop",
                          "async func work():\n    return 1\nasync func main():\n    t = work()\n    print(await t)\n"
                          "    t.cancel()\n    print(await t)\n",
                          "1\n1\n");
    expect_builtin_output("task cancel catch type",
                          "async func work():\n    await delay(100ms)\nasync func main():\n    t = work()\n    await "
                          "delay(0ms)\n    t.cancel()\n    try:\n        await t\n    catch e:\n        print(e.message)\n",
                          "Task was cancelled\n");
    expect_builtin_output("task cancel closure finally",
                          "async func main():\n    flag = \"x\"\n    async func work():\n        try:\n            await "
                          "delay(100ms)\n        finally:\n            print(flag)\n    t = work()\n    await delay(0ms)\n"
                          "    t.cancel()\n    try:\n        await t\n    catch e:\n        print(\"done\")\n",
                          "x\ndone\n");
    expect_output("race returns first completed",
                  "async func slow():\n    await delay(50ms)\n    return 2\nasync func fast():\n    await delay(1ms)\n    "
                  "return 1\nasync func main():\n    print(await race(slow(), fast()))\n",
                  "1\n");
    expect_output("race loser keeps running",
                  "async func slow():\n    await delay(20ms)\n    print(\"slow\")\nasync func fast():\n    await delay(1ms)\n"
                  "    return 1\nasync func main():\n    print(await race(slow(), fast()))\n    await delay(50ms)\n",
                  "1\nslow\n");
    expect_builtin_output("race failed task",
                          "async func fail():\n    throw ValueException(\"boom\", null)\nasync func slow():\n    await "
                          "delay(100ms)\n    return 1\nasync func main():\n    try:\n        await race(slow(), fail())\n"
                          "    catch e:\n        print(e.message)\n",
                          "boom\n");
    expect_builtin_output("race cancelled task",
                          "async func slow():\n    await delay(100ms)\n    return 1\nasync func main():\n    t = slow()\n"
                          "    await delay(0ms)\n    t.cancel()\n    try:\n        await race(t, delay(0ms))\n    catch e:\n"
                          "        print(e.message)\n",
                          "Task was cancelled\n");
    expect_output("timeout completes before limit",
                  "async func work():\n    await delay(1ms)\n    return 42\nasync func main():\n    print(await "
                  "timeout(work(), 50ms))\n",
                  "42\n");
    expect_builtin_output("timeout fires",
                          "async func work():\n    try:\n        await delay(100ms)\n        return 1\n    finally:\n"
                          "        print(\"cleanup\")\nasync func main():\n    try:\n        await timeout(work(), 5ms)\n"
                          "    catch e:\n        print(e.message)\n",
                          "cleanup\nTask timed out\n");
    expect_builtin_output("timeout preserves failure",
                          "async func work():\n    throw ValueException(\"fail\", null)\nasync func main():\n    try:\n"
                          "        await timeout(work(), 50ms)\n    catch e:\n        print(e.message)\n",
                          "fail\n");
    expect_output("timeout nested race",
                  "async func fast():\n    await delay(1ms)\n    return 9\nasync func slow():\n    await delay(100ms)\n"
                  "    return 1\nasync func main():\n    print(await timeout(race(fast(), slow()), 50ms))\n",
                  "9\n");
    expect_builtin_output("race timeout nested cancel",
                          "async func slow():\n    try:\n        await delay(100ms)\n    finally:\n        print(\"fin\")\n"
                          "async func main():\n    try:\n        await timeout(race(slow(), delay(100ms)), 5ms)\n    catch e:\n"
                          "        print(e.message)\n    await delay(20ms)\n",
                          "Task timed out\nfin\n");
    expect_module_output("module async task cancel",
                         "import svc\nasync func main():\n    t = svc.work()\n    await delay(0ms)\n    t.cancel()\n    "
                         "try:\n        await t\n    catch e:\n        print(e)\n",
                         {{"svc.phs", "async func work():\n    try:\n        await delay(100ms)\n    finally:\n        "
                                     "print(\"mod\")\n"}},
                         "mod\nTask was cancelled\n");

    // Advanced Async — Bölüm 1
    expect_output("taskgroup wait_all",
                  "async func a():\n    await delay(0ms)\n    return 1\nasync func b():\n    await delay(0ms)\n    return 2\n"
                  "async func main():\n    g = TaskGroup()\n    t1 = g.spawn(a())\n    t2 = g.spawn(b())\n    r = await g.wait()\n"
                  "    print(r.length())\n    print(await t1)\n",
                  "2\n1\n");
    expect_builtin_output("taskgroup async with scope",
                          "async func work():\n    await delay(0ms)\n    return 1\nasync func main():\n    async with TaskGroup() as g:\n"
                          "        t = g.spawn(work())\n        await g.wait()\n    print(await t)\n",
                          "1\n");
    expect_builtin_output("async with exit null on success",
                          "state = {\"ok\": false}\nclass Mgr:\n    func enter():\n        return this\n    func exit(exc):\n        state[\"ok\"] = exc == null\n"
                          "async func main():\n    async with Mgr():\n        pass\n    print(state[\"ok\"])\n",
                          "true\n");
    expect_builtin_output("async with exit receives block exception",
                          "state = {\"seen\": false, \"msg\": \"\"}\nclass Mgr:\n    func enter():\n        return this\n    func exit(exc):\n        state[\"seen\"] = exc != null\n        if exc != null:\n            state[\"msg\"] = exc.message\n"
                          "async func worker():\n    async with Mgr():\n        throw ValueException(\"boom\", null)\n"
                          "async func main():\n    try:\n        await worker()\n    catch e:\n        print(\"caught\")\n    print(state[\"seen\"])\n    print(state[\"msg\"])\n",
                          "caught\ntrue\nboom\n");
    expect_builtin_output("async with exit once on throw",
                          "state = {\"calls\": 0}\nclass Mgr:\n    func enter():\n        return this\n    func exit(exc):\n        state[\"calls\"] = state[\"calls\"] + 1\n"
                          "async func main():\n    try:\n        async with Mgr():\n            throw ValueException(\"x\", null)\n    catch e:\n        pass\n    print(state[\"calls\"])\n",
                          "1\n");
    expect_output("wait_all builtin",
                  "async func f(v):\n    await delay(0ms)\n    return v\nasync func main():\n    r = await wait_all(f(1), f(2))\n"
                  "    print(r.length())\n",
                  "2\n");
    expect_output("wait_any builtin",
                  "async func slow():\n    await delay(50ms)\n    return 0\nasync func fast():\n    await delay(0ms)\n    return 9\n"
                  "async func main():\n    print(await wait_any(slow(), fast()))\n",
                  "9\n");
    expect_builtin_output("async stream iteration colon",
                          "async func main():\n    s = AsyncStream([\"a\", \"b\"])\n    out = \"\"\n    async for chunk in s:\n        out += chunk\n"
                          "    print(out)\n",
                          "ab\n");
    expect_builtin_output("async stream iteration brace",
                          "async func main():\n    s = AsyncStream([\"x\"])\n    async for c in s {\n        print(c)\n    }\n",
                          "x\n");
    expect_builtin_output("taskgroup child failure cancels sibling",
                          "async func fail():\n    await delay(0ms)\n    throw ValueException(\"boom\", null)\nasync func slow():\n"
                          "    try:\n        await delay(100ms)\n    finally:\n        print(\"cleanup\")\nasync func main():\n"
                          "    g = TaskGroup()\n    g.spawn(fail())\n    g.spawn(slow())\n    try:\n        await g.wait()\n"
                          "    catch e:\n        print(e.message)\n    await delay(50ms)\n",
                          "cleanup\nboom\n");
    expect_builtin_output("taskgroup wait reports child failure",
                          "async func fail():\n    throw ValueException(\"boom\", null)\nasync func main():\n    g = TaskGroup()\n"
                          "    g.spawn(fail())\n    try:\n        await g.wait()\n    catch e:\n        print(e.message)\n",
                          "boom\n");

    // Standard Library — Bölüm 1
    expect_output("builtin abs int", "print(abs(-7))\n", "7\n");
    expect_output("builtin abs whitespace", "print(abs -1)\n", "1\n");
    expect_output("print whitespace nested abs", "print abs -1\n", "1\n");
    expect_output("builtin abs float", "print(abs(-1.5))\n", "1.5\n");
    expect_output("whitespace min max", "print(min 1, 2)\nprint(max 1, 2)\n", "1\n2\n");
    expect_output("whitespace round clamp", "print(round 1.5)\nprint(clamp 5, 0, 1)\n", "2\n1\n");
    expect_output("whitespace conversions", "print(int 3.5)\nprint(float 3)\nprint(str 42)\nprint(bool 1)\n",
                  "3\n3\n42\ntrue\n");
    expect_output("builtin min max int", "print(min(3, 1, 2))\nprint(max(1, 5, 3))\n", "1\n5\n");
    expect_output("builtin min max mixed", "print(min(1, 2.5))\nprint(max(1, 2.5))\n", "1\n2.5\n");
    expect_output("builtin round int", "print(round(7))\n", "7\n");
    expect_output("builtin round half away", "print(round(2.5))\nprint(round(-2.5))\n", "3\n-3\n");
    expect_output("builtin clamp normal", "print(clamp(10, 0, 5))\nprint(clamp(-1, 0, 5))\n", "5\n0\n");
    expect_output("builtin clamp boundaries", "print(clamp(3, 3, 7))\n", "3\n");
    expect_unhandled_builtins("builtin clamp invalid bounds", "print(clamp(1, 5, 0))\n", "clamp bounds are invalid");

    expect_output("stdlib math import", "import math\nprint(math.sqrt(9))\nprint(math.floor(2.9))\n", "3\n2\n");
    expect_output("stdlib math constants", "import math\nprint(math.pi > 3)\nprint(math.e > 2)\n", "true\ntrue\n");
    expect_output("stdlib math alias", "import math as m\nprint(m.pow(2, 3))\n", "8\n");
    expect_output("stdlib math from import", "from math import sqrt, log10\nprint(sqrt(16))\nprint(log10(1000))\n",
                  "4\n3\n");
    {
        const auto previous = pscript::native_boundary::test_fault_hook;
        pscript::native_boundary::test_fault_hook = [] { throw std::runtime_error("native boundary"); };
        expect_unhandled("native exception boundary", compile("import math\nprint(math.sqrt(4))\n"), "native boundary");
        pscript::native_boundary::test_fault_hook = previous;
    }
    expect_unhandled("stdlib math domain sqrt", compile("import math\nprint(math.sqrt(-1))\n"), "math domain error");
    expect_unhandled("stdlib math domain log", compile("import math\nprint(math.log(0))\n"), "math domain error");

    expect_output("string trim", "print(\"  hi  \".trim())\n", "hi\n");
    expect_output("string lower upper", "print(\"AbC\".lower())\nprint(\"AbC\".upper())\n", "abc\nABC\n");
    expect_output("string contains", "print(\"hello\".contains(\"ell\"))\n", "true\n");
    expect_output("string starts ends", "print(\"abc\".starts_with(\"a\"))\nprint(\"abc\".ends_with(\"c\"))\n",
                  "true\ntrue\n");
    expect_output("string replace", "print(\"a-b-a\".replace(\"-\", \"+\"))\n", "a+b+a\n");
    expect_output("string split", "print(\"a,b,c\".split(\",\"))\n", "[a, b, c]\n");
    expect_output("string length bytes", "print(\"\xC3\xA7\".length())\n", "2\n");
    expect_output("stdlib string join", "import string\nprint(string.join([\"a\", \"b\"], \",\"))\n", "a,b\n");
    expect_output("stdlib string join empty", "import string\nprint(string.join([], \",\"))\n", "\n");

    expect_output("stdlib collections stack",
                  "from collections import Stack\nStack<int> s = Stack()\ns.push(1)\ns.push(2)\nprint(s.pop())\n"
                  "print(s.peek())\nprint(s.length())\n",
                  "2\n1\n1\n");
    expect_output("stdlib collections queue",
                  "from collections import Queue\nQueue<string> q = Queue()\nq.enqueue(\"a\")\nq.enqueue(\"b\")\n"
                  "print(q.dequeue())\nprint(q.is_empty())\n",
                  "a\nfalse\n");
    expect_output("stdlib collections deque",
                  "from collections import Deque\nDeque<int> d = Deque()\nd.push_front(1)\nd.push_back(2)\n"
                  "print(d.front())\nprint(d.back())\nprint(d.pop_front())\nprint(d.pop_back())\n",
                  "1\n2\n1\n2\n");
    expect_output("stdlib collections from import",
                  "from collections import Stack\nStack<int> s = Stack()\ns.push(42)\nprint(s.pop())\n", "42\n");
    expect_unhandled("stdlib stack empty pop",
                     compile("from collections import Stack\nStack<int> s = Stack()\nprint(s.pop())\n"),
                     "stack is empty");
    {
        try {
            compile("from collections import Stack\nStack<int> s = Stack()\ns.push(\"x\")\n");
            fail_test("stdlib collections type mismatch", "expected compile error");
        } catch (const CompilerError& err) {
            if (std::string_view(err.message()).find("Type error") == std::string_view::npos) {
                fail_test("stdlib collections type mismatch", err.message());
            } else {
                pass_test("stdlib collections type mismatch");
            }
        }
    }

    // Standard Library — Bölüm 2: path, fs, io
    expect_output("stdlib path join", "import path\nprint(path.join(\"data\", \"save.txt\"))\n", "data/save.txt\n");
    expect_output("stdlib path basename", "import path\nprint(path.basename(\"dir/file.txt\"))\n", "file.txt\n");
    expect_output("stdlib path dirname", "import path\nprint(path.dirname(\"dir/file.txt\"))\n", "dir\n");
    expect_output("stdlib path extension", "import path\nprint(path.extension(\"archive.tar.gz\"))\n", ".gz\n");
    expect_output("stdlib path stem", "import path\nprint(path.stem(\"archive.tar.gz\"))\n", "archive.tar\n");
    expect_output("stdlib path normalize dots",
                  "import path\nprint(path.normalize(\"a/../b\"))\nprint(path.normalize(\"./x\"))\n", "b\nx\n");
    expect_output("stdlib path dot file", "import path\nprint(path.basename(\".gitignore\"))\n", ".gitignore\n");
    expect_output("stdlib path is absolute relative",
                  "import path\nprint(path.is_absolute(\"tmp/x\"))\nprint(path.is_relative(\"tmp/x\"))\n",
                  "false\ntrue\n");
    expect_output("stdlib path relative same",
                  "import path\nprint(path.relative(\"a/b\", \"a/b\"))\n", ".\n");
    expect_output("stdlib path import alias", "import path as p\nprint(p.join(\"a\", \"b\"))\n", "a/b\n");
    expect_output("stdlib path from import", "from path import join, basename\nprint(join(\"x\", \"y\"))\nprint(basename(\"a/z\"))\n",
                  "x/y\nz\n");

    expect_fs_output("stdlib fs write read", "import fs\nfs.write(\"note.txt\", \"hello\")\nprint(fs.read(\"note.txt\"))\n",
                     "hello\n");
    expect_fs_output("stdlib fs append", "import fs\nfs.write(\"log.txt\", \"a\")\nfs.append(\"log.txt\", \"b\")\nprint(fs.read(\"log.txt\"))\n",
                     "ab\n");
    expect_fs_output("stdlib fs exists", "import fs\nprint(fs.exists(\"missing.txt\"))\nfs.write(\"x.txt\", \"\")\nprint(fs.exists(\"x.txt\"))\n",
                     "false\ntrue\n");
    expect_fs_output("stdlib fs is file is dir",
                      "import fs\nfs.write(\"f.txt\", \"\")\nfs.create_dir(\"d\")\nprint(fs.is_file(\"f.txt\"))\nprint(fs.is_dir(\"d\"))\n",
                      "true\ntrue\n");
    expect_fs_output("stdlib fs create dir idempotent",
                      "import fs\nfs.create_dir(\"d\")\nfs.create_dir(\"d\")\nprint(fs.is_dir(\"d\"))\n", "true\n");
    expect_fs_output("stdlib fs list dir empty",
                      "import fs\nfs.create_dir(\"empty\")\nprint(fs.list_dir(\"empty\"))\n", "[]\n");
    expect_fs_output("stdlib fs list dir names",
                      "import fs\nfs.create_dir(\"items\")\nfs.write(\"items/a.txt\", \"\")\nfs.write(\"items/b.txt\", \"\")\nprint(fs.list_dir(\"items\"))\n",
                      "[a.txt, b.txt]\n");
    expect_fs_output("stdlib fs copy move remove",
                      "import fs\nfs.write(\"src.txt\", \"data\")\nfs.copy(\"src.txt\", \"copy.txt\")\nprint(fs.read(\"copy.txt\"))\nfs.move(\"copy.txt\", \"moved.txt\")\nprint(fs.exists(\"copy.txt\"))\nprint(fs.read(\"moved.txt\"))\nfs.remove(\"moved.txt\")\nprint(fs.exists(\"moved.txt\"))\n",
                      "data\nfalse\ndata\nfalse\n");
    expect_fs_output("stdlib fs empty file", "import fs\nfs.write(\"empty.txt\", \"\")\nprint(fs.read(\"empty.txt\"))\n", "\n");
    expect_fs_output("stdlib path fs integration",
                      "import path\nimport fs\np = path.join(\"data\", \"save.txt\")\nfs.create_dir(\"data\")\nfs.write(p, \"ok\")\nprint(fs.read(p))\n",
                      "ok\n");
    expect_fs_output("stdlib fs import alias", "import fs as f\nf.write(\"a.txt\", \"1\")\nprint(f.read(\"a.txt\"))\n", "1\n");
    expect_fs_unhandled("stdlib fs read missing", "import fs\nprint(fs.read(\"missing.txt\"))\n", "file not found");
    expect_fs_unhandled("stdlib fs write directory", "import fs\nfs.create_dir(\"d\")\nprint(fs.write(\"d\", \"x\"))\n", "path is a directory");
    expect_fs_unhandled("stdlib fs read directory", "import fs\nfs.create_dir(\"d\")\nprint(fs.read(\"d\"))\n", "path is a directory");
    expect_fs_unhandled("stdlib fs list file", "import fs\nfs.write(\"f.txt\", \"\")\nprint(fs.list_dir(\"f.txt\"))\n", "path is not a directory");
    expect_fs_unhandled("stdlib fs remove missing", "import fs\nfs.remove(\"missing.txt\")\n", "path not found");
    expect_fs_unhandled("stdlib fs create dir missing parent", "import fs\nfs.create_dir(\"a/b\")\n", "parent directory does not exist");
    expect_fs_output("stdlib fs caught read error",
                      "import fs\ntry:\n    fs.read(\"missing.txt\")\ncatch RuntimeException error:\n    print(error.message)\n",
                      "read failed for 'missing.txt': file not found\n");

    expect_fs_output("stdlib io write read close",
                      "import io\nf = io.open(\"io.txt\", \"w\")\nf.write(\"abc\")\nf.close()\nimport fs\nprint(fs.read(\"io.txt\"))\n",
                      "abc\n");
    expect_fs_output("stdlib io append mode",
                      "import io\nf = io.open(\"io.txt\", \"w\")\nf.write(\"a\")\nf.close()\nf = io.open(\"io.txt\", \"a\")\nf.write(\"b\")\nf.close()\nimport fs\nprint(fs.read(\"io.txt\"))\n",
                      "ab\n");
    expect_fs_output("stdlib io read mode",
                      "import fs\nimport io\nfs.write(\"io.txt\", \"xyz\")\nf = io.open(\"io.txt\", \"r\")\nprint(f.read())\nf.close()\n",
                      "xyz\n");
    expect_fs_output("stdlib io is open",
                      "import io\nf = io.open(\"io.txt\", \"w\")\nprint(f.is_open())\nf.close()\nprint(f.is_open())\n",
                      "true\nfalse\n");
    expect_fs_output("stdlib io double close",
                      "import io\nf = io.open(\"io.txt\", \"w\")\nf.close()\nf.close()\nprint(f.is_open())\n",
                      "false\n");
    expect_fs_unhandled("stdlib io use after close",
                        "import io\nf = io.open(\"io.txt\", \"w\")\nf.close()\nf.write(\"x\")\n", "file handle is closed");
    expect_fs_unhandled("stdlib io open missing read",
                        "import io\nf = io.open(\"missing.txt\", \"r\")\n", "open failed");
    expect_fs_unhandled("stdlib io invalid mode", "import io\nf = io.open(\"x.txt\", \"x\")\n", "unsupported file open mode");
    expect_fs_output("stdlib io import from",
                      "from io import open\nf = open(\"io.txt\", \"w\")\nf.write(\"hi\")\nf.close()\nimport fs\nprint(fs.read(\"io.txt\"))\n",
                      "hi\n");
    expect_fs_output("stdlib io function boundary",
                      "import io\nfunc open_writer():\n    return io.open(\"boundary.txt\", \"w\")\nf = open_writer()\nf.write(\"saved\")\nf.close()\nimport fs\nprint(fs.read(\"boundary.txt\"))\n",
                      "saved\n");
    expect_fs_output("stdlib async regression with fs",
                      "import fs\nasync func main():\n    fs.write(\"async.txt\", \"ok\")\n    print(fs.read(\"async.txt\"))\n",
                      "ok\n");

    // Async/Await Update Bölüm 2 — Async I/O + async_input()
    expect_fs_output("async fs read write",
                      "import fs\nasync func main():\n    await fs.async_write(\"note.txt\", \"hello\")\n"
                      "    print(await fs.async_read(\"note.txt\"))\n",
                      "hello\n");
    expect_fs_output("async fs append",
                      "import fs\nasync func main():\n    await fs.async_write(\"log.txt\", \"a\")\n"
                      "    await fs.async_append(\"log.txt\", \"b\")\n    print(await fs.async_read(\"log.txt\"))\n",
                      "ab\n");
    expect_fs_output("async io handle read write",
                      "import io\nimport fs\nasync func main():\n    f = io.open(\"io.txt\", \"w\")\n"
                      "    await f.async_write(\"abc\")\n    f.close()\n    f = io.open(\"io.txt\", \"r\")\n"
                      "    print(await f.async_read())\n    f.close()\n",
                      "abc\n");
    expect_fs_unhandled("async fs read missing",
                        "import fs\nasync func main():\n    print(await fs.async_read(\"missing.txt\"))\n",
                        "file not found");
    expect_fs_output("async fs finally",
                      "import fs\nasync func main():\n    try:\n        await fs.async_write(\"f.txt\", \"x\")\n"
                      "    finally:\n        print(\"cleanup\")\n    print(await fs.async_read(\"f.txt\"))\n",
                      "cleanup\nx\n");
    expect_fs_output("async fs timeout completes early",
                      "import fs\nasync func main():\n    await fs.async_write(\"t.txt\", \"ok\")\n"
                      "    print(await timeout(fs.async_read(\"t.txt\"), 1000ms))\n",
                      "ok\n");
    expect_fs_output("async fs race loser continues",
                      "import fs\nasync func slow():\n    await delay(30ms)\n    return 2\nasync func fast():\n"
                      "    await fs.async_write(\"race.txt\", \"1\")\n    return 1\nasync func main():\n"
                      "    print(await race(fast(), slow()))\n    await delay(50ms)\n",
                      "1\n");
    expect_fs_output("async fs concurrent operations",
                      "import fs\nasync func main():\n    await fs.async_write(\"a.txt\", \"A\")\n"
                      "    await fs.async_write(\"b.txt\", \"B\")\n    a = fs.async_read(\"a.txt\")\n    b = fs.async_read(\"b.txt\")\n"
                      "    x = await a\n    y = await b\n    print(x)\n    print(y)\n",
                      "A\nB\n");
    expect_module_output("async fs module function",
                         "import helper\nasync func main():\n    print(await helper.helper())\n",
                         {{"helper.phs", "import fs\nasync func helper():\n    await fs.async_write(\"m.txt\", \"mod\")\n"
                                         "    return await fs.async_read(\"m.txt\")\n"}},
                         "mod\n");
    {
        try {
            const std::string got = run_async_source_with_input(
                "async func main():\n    print(await async_input())\n", "Alice\n");
            if (got != "Alice\n") {
                fail_test("async input basic", "got \"" + got + "\" expected \"Alice\\n\"");
            } else {
                pass_test("async input basic");
            }
        } catch (const std::exception& ex) {
            fail_test("async input basic", ex.what());
        }
    }
    {
        try {
            const std::string got = run_async_source_with_input(
                "async func main():\n    print(await async_input())\n", "Bob\n");
            if (got != "Bob\n") {
                fail_test("async input async main", "got \"" + got + "\" expected \"Bob\\n\"");
            } else {
                pass_test("async input async main");
            }
        } catch (const std::exception& ex) {
            fail_test("async input async main", ex.what());
        }
    }
    {
        try {
            const std::string got = run_async_source_with_input(
                "async func main():\n    print(await async_input())\n", "Bob\n");
            if (got != "Bob\n") {
                fail_test("async input with injected stream", "got \"" + got + "\" expected \"Bob\\n\"");
            } else {
                pass_test("async input with injected stream");
            }
        } catch (const std::exception& ex) {
            fail_test("async input with injected stream", ex.what());
        }
    }
    {
        try {
            const std::string got = run_async_source_with_input(
                "async func main():\n    print(await race(async_input(), delay(50ms)))\n", "win\n");
            if (got != "win\n") {
                fail_test("async input race delay", "got \"" + got + "\" expected \"win\\n\"");
            } else {
                pass_test("async input race delay");
            }
        } catch (const std::exception& ex) {
            fail_test("async input race delay", ex.what());
        }
    }
    {
        try {
            const std::string got = run_async_source_with_input(
                "async func main():\n    print(await timeout(async_input(), 1000ms))\n", "typed\n");
            if (got != "typed\n") {
                fail_test("async input timeout completes", "got \"" + got + "\" expected \"typed\\n\"");
            } else {
                pass_test("async input timeout completes");
            }
        } catch (const std::exception& ex) {
            fail_test("async input timeout completes", ex.what());
        }
    }
    {
        try {
            const std::string got = run_async_source_with_input(
                "async func main():\n    try:\n        print(await async_input())\n    finally:\n        print(\"done\")\n",
                "hi\n");
            if (got != "hi\ndone\n") {
                fail_test("async input finally", "got \"" + got + "\" expected \"hi\\ndone\\n\"");
            } else {
                pass_test("async input finally");
            }
        } catch (const std::exception& ex) {
            fail_test("async input finally", ex.what());
        }
    }
    {
        try {
            const std::string got = run_async_source_with_input(
                "async func main():\n    t = async_input()\n    await delay(0ms)\n    t.cancel()\n    try:\n"
                "        await t\n    catch CancellationException:\n        print(\"cancelled\")\n",
                "never\n");
            if (got != "cancelled\n") {
                fail_test("async input cancellation", "got \"" + got + "\" expected \"cancelled\\n\"");
            } else {
                pass_test("async input cancellation");
            }
        } catch (const std::exception& ex) {
            fail_test("async input cancellation", ex.what());
        }
    }
    {
        try {
            const std::string got = run_async_source_with_input(
                "async func main():\n    try:\n        await async_input()\n    catch RuntimeException error:\n"
                "        print(error.message)\n",
                "");
            if (got != "end of input\n") {
                fail_test("async input eof", "got \"" + got + "\" expected \"end of input\\n\"");
            } else {
                pass_test("async input eof");
            }
        } catch (const std::exception& ex) {
            fail_test("async input eof", ex.what());
        }
    }
    expect_fs_output("async io scheduler interleaves delay",
                      "import fs\nasync func main():\n    async func marker():\n        await delay(10ms)\n        print(\"tick\")\n"
                      "    t = marker()\n    await fs.async_write(\"x.txt\", \"data\")\n    print(await fs.async_read(\"x.txt\"))\n"
                      "    await t\n",
                      "data\ntick\n");

    // Standard Library — Bölüm 3: os, process, sys
#ifdef _WIN32
    expect_output("stdlib os name", "import os\nprint(os.name())\n", "Windows\n");
    expect_output("stdlib os platform", "import os\nprint(os.platform())\n", "win32\n");
#else
    expect_output("stdlib os name", "import os\nprint(os.name())\n", "Linux\n");
    expect_output("stdlib os platform", "import os\nprint(os.platform())\n", "linux\n");
#endif
    expect_output("stdlib os arch", "import os\nprint(os.arch() != \"\")\n", "true\n");
    expect_fs_output("stdlib os cwd chdir",
                      "import os\nimport fs\nfs.create_dir(\"subdir\")\nprint(os.cwd() != \"\")\n"
                      "os.chdir(\"subdir\")\nprint(os.cwd().contains(\"subdir\"))\n",
                      "true\ntrue\n");
    expect_output("stdlib os env set has remove",
                  "import os\nos.set_env(\"PSCRIPT_TEST_ENV\", \"value\")\nprint(os.has_env(\"PSCRIPT_TEST_ENV\"))\n"
                  "print(os.env()[\"PSCRIPT_TEST_ENV\"])\nos.remove_env(\"PSCRIPT_TEST_ENV\")\nprint(os.has_env(\"PSCRIPT_TEST_ENV\"))\n",
                  "true\nvalue\nfalse\n");
    expect_output("stdlib os home temp",
                  "import os\nprint(os.home_dir() != \"\")\nprint(os.temp_dir() != \"\")\n", "true\ntrue\n");
    expect_fs_output("stdlib os path fs integration",
                      "import os\nimport path\nimport fs\nfs.create_dir(\"work\")\nos.chdir(\"work\")\n"
                      "fs.create_dir(\"data\")\np = path.join(\"data\", \"note.txt\")\nfs.write(p, \"saved\")\nprint(fs.read(p))\n",
                      "saved\n");
    expect_output("stdlib process id", "import process\nprint(process.id() > 0)\n", "true\n");
    expect_fs_output("stdlib process run success",
                      "import process\nresult = process.run(\"echo hello\")\nprint(result.stdout.contains(\"hello\"))\n"
                      "print(result.exit_code == 0)\n",
                      "true\ntrue\n");
    expect_fs_output("stdlib process run stderr",
                      "import process\nresult = process.run(\"echo err>&2\")\nprint(result.stderr.contains(\"err\"))\n",
                      "true\n");
    expect_fs_output("stdlib process run exit code",
                      "import process\nresult = process.run(\"exit 7\")\nprint(result.exit_code)\n", "7\n");
    expect_process_exit("stdlib process exit", "import process\nprocess.exit(3)\nprint(\"skip\")\n", 3);
    expect_source_with_args("stdlib sys args", "import sys\nprint(sys.args)\n", {"hello", "world"}, "[hello, world]\n");
    expect_output("stdlib sys version", "import sys\nprint(sys.version)\n", "1.0.0\n");
    expect_output("stdlib sys import alias", "import sys as s\nprint(s.version)\n", "1.0.0\n");
    expect_fs_output("stdlib process exception caught",
                      "import process\ntry:\n    process.run(\"\")\ncatch ValueException error:\n    print(error.message)\n",
                      "'run' expects a non-empty command\n");

    // Standard Library — Bölüm 4: time, datetime, random
    expect_output("stdlib time unix positive",
                  "import time\nprint(time.unix() > 0)\n", "true\n");
    expect_output("stdlib time monotonic increases",
                  "import time\na = time.monotonic()\nb = time.monotonic()\nprint(b >= a)\n", "true\n");
    expect_output("stdlib time monotonic difference",
                  "import time\na = time.monotonic()\ntime.sleep(0.01)\nb = time.monotonic()\nprint(b > a)\n",
                  "true\n");
    expect_output("stdlib time sleep fractional",
                  "import time\nstart = time.monotonic()\ntime.sleep(0.05)\nelapsed = time.monotonic() - start\n"
                  "print(elapsed >= 0.04)\n",
                  "true\n");
    expect_output("stdlib time now type",
                  "import time\nprint(type(time.now()))\n", "datetime\n");
    expect_fs_output("stdlib time sleep invalid",
                      "import time\ntry:\n    time.sleep(-1)\ncatch ValueException error:\n    print(error.message)\n",
                      "'sleep' expects a non-negative duration\n");
    expect_output("stdlib datetime create fields",
                  "import datetime\ndt = datetime.create(2026, 9, 19, 12, 30, 0)\n"
                  "print(dt.year)\nprint(dt.month)\nprint(dt.day)\nprint(dt.hour)\nprint(dt.minute)\nprint(dt.second)\n",
                  "2026\n9\n19\n12\n30\n0\n");
    expect_output("stdlib datetime leap year",
                  "import datetime\ndt = datetime.create(2024, 2, 29, 0, 0, 0)\nprint(dt.day)\n", "29\n");
    expect_fs_output("stdlib datetime invalid day",
                      "import datetime\ntry:\n    datetime.create(2023, 2, 29, 0, 0, 0)\ncatch ValueException error:\n"
                      "    print(error.message)\n",
                      "invalid datetime day\n");
    expect_output("stdlib datetime parse format",
                  "import datetime\ndt = datetime.parse(\"2026-09-19 12:30:00\")\nprint(dt.format())\n",
                  "2026-09-19 12:30:00\n");
    expect_fs_output("stdlib datetime parse failure",
                      "import datetime\ntry:\n    datetime.parse(\"bad\")\ncatch ValueException error:\n    print(error.message)\n",
                      "invalid datetime string\n");
    expect_output("stdlib datetime from unix roundtrip",
                  "import datetime\ndt = datetime.from_unix(0)\nprint(dt.year)\nprint(dt.month)\nprint(dt.day)\n"
                  "print(dt.hour)\nprint(dt.minute)\nprint(dt.second)\n",
                  "1970\n1\n1\n0\n0\n0\n");
    expect_output("stdlib datetime today midnight",
                  "import datetime\ndt = datetime.today()\nprint(dt.hour)\nprint(dt.minute)\nprint(dt.second)\n",
                  "0\n0\n0\n");
    expect_output("stdlib datetime comparison",
                  "import datetime\na = datetime.create(2026, 1, 1, 0, 0, 0)\nb = datetime.create(2026, 1, 2, 0, 0, 0)\n"
                  "print(a < b)\nprint(a == a)\nprint(b > a)\n",
                  "true\ntrue\ntrue\n");
    expect_output("stdlib datetime duration subtract",
                  "import datetime\na = datetime.create(2026, 1, 2, 0, 0, 0)\nb = datetime.create(2026, 1, 1, 0, 0, 0)\n"
                  "d = a - b\nprint(d.seconds)\n",
                  "86400\n");
    expect_output("stdlib datetime duration add subtract",
                  "import datetime\nbase = datetime.create(2026, 1, 1, 12, 0, 0)\nd = base - datetime.create(2026, 1, 1, 0, 0, 0)\n"
                  "restored = datetime.create(2026, 1, 1, 0, 0, 0) + d\nprint(restored.hour)\nprint(restored.minute)\n",
                  "12\n0\n");
    expect_output("stdlib datetime negative duration",
                  "import datetime\na = datetime.create(2026, 1, 1, 0, 0, 0)\nb = datetime.create(2026, 1, 2, 0, 0, 0)\n"
                  "d = a - b\nprint(d.seconds < 0)\n",
                  "true\n");
    expect_output("stdlib datetime type names",
                  "import datetime\na = datetime.create(2026, 1, 1, 0, 0, 0)\nb = a - datetime.create(2025, 1, 1, 0, 0, 0)\n"
                  "print(type(a))\nprint(type(b))\n",
                  "datetime\nduration\n");
    expect_output("stdlib random int inclusive",
                  "import random\nrandom.seed(123)\nprint(random.int(1, 3) >= 1)\nprint(random.int(1, 3) <= 3)\n",
                  "true\ntrue\n");
    expect_output("stdlib random float unit interval",
                  "import random\nrandom.seed(99)\nvalue = random.float()\nprint(value >= 0)\nprint(value < 1)\n",
                  "true\ntrue\n");
    expect_output("stdlib random float range",
                  "import random\nrandom.seed(7)\nvalue = random.float(0, 100)\nprint(value >= 0)\nprint(value < 100)\n",
                  "true\ntrue\n");
    expect_output("stdlib random choice",
                  "import random\nrandom.seed(1)\nprint(random.choice([10, 20, 30]) == 10)\n", "true\n");
    expect_fs_output("stdlib random choice empty",
                      "import random\ntry:\n    random.choice([])\ncatch ValueException error:\n    print(error.message)\n",
                      "'choice' expects a non-empty sequence\n");
    expect_output("stdlib random shuffle mutates",
                  "import random\nitems = [1, 2, 3, 4, 5]\nrandom.seed(42)\nrandom.shuffle(items)\nprint(items != [1, 2, 3, 4, 5])\n",
                  "true\n");
    expect_output("stdlib random seed determinism",
                  "import random\nrandom.seed(999)\na = random.int(1, 1000)\nrandom.seed(999)\nb = random.int(1, 1000)\n"
                  "print(a == b)\n",
                  "true\n");
    expect_output("stdlib random different seeds",
                  "import random\nrandom.seed(1)\na = random.int(1, 1000000)\nrandom.seed(2)\nb = random.int(1, 1000000)\n"
                  "print(a != b)\n",
                  "true\n");
    expect_fs_output("stdlib random int invalid range",
                      "import random\ntry:\n    random.int(10, 1)\ncatch ValueException error:\n    print(error.message)\n",
                      "'int' expects minimum <= maximum\n");
    expect_output("stdlib time sleep not async delay",
                  "import time\nstart = time.monotonic()\ntime.sleep(0.02)\nprint(time.monotonic() - start >= 0.01)\n",
                  "true\n");
    expect_output("stdlib async monotonic in task",
                  "import time\nasync func main():\n    a = time.monotonic()\n    await delay(0ms)\n    b = time.monotonic()\n    print(b >= a)\n",
                  "true\n");
    expect_output("stdlib datetime module import",
                  "import datetime\nimport time\nprint(type(datetime.now()))\nprint(type(time.now()))\n",
                  "datetime\ndatetime\n");

    // Standard Library — Bölüm 5: json, re, encoding
    expect_output("stdlib json encode primitives",
                  "import json\nprint(json.encode(null))\nprint(json.encode(true))\nprint(json.encode(42))\n"
                  "print(json.encode(1.5))\nprint(json.encode(\"hi\"))\n",
                  "null\ntrue\n42\n1.5\n\"hi\"\n");
    expect_output("stdlib json encode list map",
                  "import json\nprint(json.encode([1, 2]))\nprint(json.encode({\"a\": 1, \"b\": 2}))\n",
                  "[1,2]\n{\"a\":1,\"b\":2}\n");
    expect_output("stdlib json encode tuple",
                  "import json\nprint(json.encode((1, 2)))\n", "[1,2]\n");
    expect_output("stdlib json encode unicode",
                  "import json\nprint(json.encode(\"Merhaba Dünya\"))\n", "\"Merhaba Dünya\"\n");
    expect_output("stdlib json encode pretty",
                  "import json\nprint(json.encode({\"a\": 1}, true).contains(\"\\n\"))\n", "true\n");
    expect_output("stdlib json decode roundtrip",
                  "import json\ntext = json.encode({\"x\": [1, 2], \"y\": null})\n"
                  "data = json.decode(text)\nprint(data[\"x\"][0])\nprint(data[\"y\"] == null)\n",
                  "1\ntrue\n");
    expect_fs_output("stdlib json decode invalid",
                      "import json\ntry:\n    json.decode(\"{\")\ncatch ValueException error:\n    print(error.message)\n",
                      "invalid JSON\n");
    expect_fs_output("stdlib json encode unsupported",
                      "import datetime\nimport json\ntry:\n    json.encode(datetime.now())\ncatch ValueException error:\n"
                      "    print(error.message)\n",
                      "value is not JSON serializable\n");
    expect_fs_output("stdlib json read write",
                      "import json\nimport fs\ndata = {\"msg\": \"Merhaba\", \"n\": 3}\njson.write(\"cfg.json\", data)\n"
                      "loaded = json.read(\"cfg.json\")\nprint(loaded[\"msg\"])\nprint(loaded[\"n\"])\n",
                      "Merhaba\n3\n");
    expect_fs_output("stdlib json write pretty",
                      "import json\nimport fs\njson.write(\"pretty.json\", {\"a\": 1}, true)\n"
                      "print(fs.read(\"pretty.json\").contains(\"\\n\"))\n",
                      "true\n");
    expect_output("stdlib re is match entire string",
                  "import re\nprint(re.is_match(\"Player\", \"Player\"))\nprint(re.is_match(\"Player\", \"PlayerOne\"))\n",
                  "true\nfalse\n");
    expect_output("stdlib re is match anchor",
                  "import re\nprint(re.is_match(\"^Player\", \"Player\"))\nprint(re.is_match(\"^Player\", \"XPlayer\"))\n",
                  "true\nfalse\n");
    expect_output("stdlib re search found",
                  "import re\nm = re.search(\"Player\", \"XPlayerY\")\nprint(m.found)\nprint(m.text)\nprint(m.start)\nprint(m.end)\n",
                  "true\nPlayer\n1\n7\n");
    expect_output("stdlib re search not found",
                  "import re\nm = re.search(\"Player\", \"Hello\")\nprint(m == null)\n", "true\n");
    expect_output("stdlib re search groups",
                  "import re\nm = re.search(\"(Pl)(ayer)\", \"XPlayer\")\nprint(m.groups[0])\nprint(m.groups[1])\n",
                  "Pl\nayer\n");
    expect_output("stdlib re find all",
                  "import re\nmatches = re.find_all(\"a\", \"aba\")\nprint(matches.length())\nprint(matches[0].text)\n"
                  "print(matches[1].text)\n",
                  "2\na\na\n");
    expect_output("stdlib re replace all",
                  "import re\nprint(re.replace(\"a\", \"b\", \"aba\"))\n", "bbb\n");
    expect_output("stdlib re replace count",
                  "import re\nprint(re.replace(\"a\", \"b\", \"aaa\", 2))\n", "bba\n");
    expect_output("stdlib re replace count zero",
                  "import re\nprint(re.replace(\"a\", \"b\", \"aaa\", 0))\n", "aaa\n");
    expect_output("stdlib re split",
                  "import re\nparts = re.split(\",| \", \"a, b c\")\nprint(parts.length())\nprint(parts[0])\nprint(parts[3])\n",
                  "4\na\nc\n");
    expect_output("stdlib re unicode",
                  "import re\nprint(re.is_match(\"Dünya\", \"Dünya\"))\n", "true\n");
    expect_fs_output("stdlib re invalid pattern",
                      "import re\ntry:\n    re.search(\"(\", \"text\")\n    print(\"fail\")\ncatch ValueException:\n    print(\"caught\")\n",
                      "caught\n");
    expect_output("stdlib encoding utf8 roundtrip",
                  "import encoding\ns = \"Merhaba Dünya\"\nb = encoding.encode(s)\nt = encoding.decode(b)\nprint(t)\n",
                  "Merhaba Dünya\n");
    expect_output("stdlib encoding utf8 default name",
                  "import encoding\nb = encoding.encode(\"abc\", \"utf8\")\nprint(encoding.decode(b, \"utf-8\"))\n", "abc\n");
    expect_output("stdlib bytes type",
                  "import encoding\nb = encoding.encode(\"abc\")\nprint(type(b))\n", "bytes\n");
    expect_output("stdlib bytes length index",
                  "import encoding\nb = encoding.encode(\"abc\")\nprint(b.length())\nprint(b[0])\nprint(b[1])\nprint(b[2])\n",
                  "3\n97\n98\n99\n");
    expect_output("stdlib bytes equality",
                  "import encoding\na = encoding.encode(\"abc\")\nb = encoding.encode(\"abc\")\nc = encoding.encode(\"abd\")\n"
                  "print(a == b)\nprint(a == c)\n",
                  "true\nfalse\n");
    expect_output("stdlib bytes function arg",
                  "import encoding\nfunc echo(x):\n    return x\nb = encoding.encode(\"hi\")\nprint(echo(b).length())\n",
                  "2\n");
    expect_output("stdlib bytes closure capture",
                  "import encoding\nfunc make():\n    b = encoding.encode(\"xy\")\n    func get():\n        return b\n    return get\n"
                  "g = make()\nprint(g()[0])\n",
                  "120\n");
    expect_output("stdlib bytes iteration",
                  "import encoding\nb = encoding.encode(\"ab\")\ncount = 0\nfor byte in b:\n    count = count + 1\nprint(count)\n",
                  "2\n");
    expect_output("stdlib encoding utf16 bom",
                  "import encoding\nb = encoding.encode(\"A\", \"utf16\")\nprint(b[0])\nprint(b[1])\n", "255\n254\n");
    expect_output("stdlib encoding utf16 roundtrip",
                  "import encoding\ns = \"Merhaba\"\nb = encoding.encode(s, \"utf16\")\nprint(encoding.decode(b, \"utf16\"))\n",
                  "Merhaba\n");
    expect_output("stdlib encoding ascii valid",
                  "import encoding\nprint(encoding.decode(encoding.encode(\"ABC\", \"ascii\"), \"ascii\"))\n", "ABC\n");
    expect_fs_output("stdlib encoding ascii invalid encode",
                      "import encoding\ntry:\n    encoding.encode(\"Dünya\", \"ascii\")\ncatch ValueException error:\n"
                      "    print(error.message)\n",
                      "character cannot be encoded as ASCII\n");
    expect_fs_output("stdlib encoding invalid utf8 decode",
                      "import encoding\ntry:\n    bad = encoding.decode(encoding.encode(\"A\", \"utf16\"), \"utf8\")\n"
                      "    print(bad)\ncatch ValueException error:\n    print(error.message)\n",
                      "invalid UTF-8 byte sequence\n");
    expect_fs_output("stdlib encoding invalid name",
                      "import encoding\ntry:\n    encoding.encode(\"abc\", \"latin1\")\ncatch ValueException error:\n"
                      "    print(error.message)\n",
                      "unsupported encoding 'latin1'\n");
    expect_output("stdlib encoding empty string",
                  "import encoding\nb = encoding.encode(\"\")\nprint(b.length())\nprint(encoding.decode(b))\n", "0\n\n");
    expect_output("stdlib json module import",
                  "import json\nimport re\nimport encoding\nprint(json.encode(1) == \"1\")\nprint(re.is_match(\"a\", \"a\"))\n"
                  "print(type(encoding.encode(\"x\")))\n",
                  "true\ntrue\nbytes\n");

    // Standard Library Section 6 — net (TCP, UDP, DNS, async integration)
    expect_output("stdlib net module import", "import net\nprint(\"net\")\n", "net\n");
    expect_output("stdlib net udp sync send receive",
                  "import net\nrecv = net.udp()\nport = recv.local_port()\nsender = net.udp()\nsender.send(\"hello\", "
                  "\"127.0.0.1\", port)\npacket = recv.receive()\nprint(packet.data.length())\nprint(packet.host)\n"
                  "print(packet.port > 0)\nrecv.close()\nsender.close()\n",
                  "5\n127.0.0.1\ntrue\n");
    expect_output("stdlib net udp bytes",
                  "import net\nimport encoding\nrecv = net.udp()\nport = recv.local_port()\nsender = net.udp()\n"
                  "payload = encoding.encode(\"abc\")\nsender.send(payload, \"127.0.0.1\", port)\npacket = recv.receive()\n"
                  "print(packet.data[0])\nprint(packet.data.length())\nrecv.close()\nsender.close()\n",
                  "97\n3\n");
    expect_output("stdlib net tcp async echo",
                  "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = server.local_port()\n"
                  "    async func client():\n        conn = await net.connect(\"127.0.0.1\", port)\n        await conn.write("
                  "\"hello\")\n        await delay(10ms)\n        conn.close()\n    task = client()\n    conn = await server.accept()\n"
                  "    data = await conn.read()\n    print(type(data))\n    print(data.length())\n    conn.close()\n    server.close()\n"
                  "    await task\n",
                  "bytes\n5\n");
    expect_output("stdlib net tcp async server to client",
                  "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = server.local_port()\n"
                  "    async func client():\n        conn = await net.connect(\"127.0.0.1\", port)\n        data = await conn.read()\n"
                  "        print(data.length())\n        conn.close()\n    task = client()\n    conn = await server.accept()\n"
                  "    await conn.write(\"hello\")\n    await task\n    conn.close()\n    server.close()\n",
                  "5\n");
    expect_output("stdlib net tcp string write utf8",
                  "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = server.local_port()\n"
                  "    async func client():\n        conn = await net.connect(\"127.0.0.1\", port)\n        written = await "
                  "conn.write(\"hi\")\n        print(written)\n        conn.close()\n    task = client()\n    conn = await "
                  "server.accept()\n    data = await conn.read()\n    print(data.length())\n    conn.close()\n    server.close()\n"
                  "    await task\n",
                  "2\n2\n");
    expect_output("stdlib net tcp empty bytes",
                  "import net\nimport encoding\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = "
                  "server.local_port()\n    async func client():\n        conn = await net.connect(\"127.0.0.1\", port)\n"
                  "        empty = encoding.encode(\"\")\n        written = await conn.write(empty)\n        print(written)\n"
                  "        conn.close()\n    task = client()\n    conn = await server.accept()\n    conn.close()\n    server.close()\n"
                  "    await task\n",
                  "0\n");
    expect_output("stdlib net tcp multiple writes",
                  "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = server.local_port()\n"
                  "    async func client():\n        conn = await net.connect(\"127.0.0.1\", port)\n        await conn.write("
                  "\"abc\")\n        await delay(10ms)\n        conn.close()\n    task = client()\n    conn = await server.accept()\n"
                  "    data = await conn.read()\n    print(data.length())\n    conn.close()\n    server.close()\n    await task\n",
                  "3\n");
    expect_output("stdlib net tcp close and is_open",
                  "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = server.local_port()\n"
                  "    async func client():\n        conn = await net.connect(\"127.0.0.1\", port)\n        print(conn.is_open())\n"
                  "        conn.close()\n        print(conn.is_open())\n        conn.close()\n    task = client()\n    conn = "
                  "await server.accept()\n    conn.close()\n    server.close()\n    await task\n",
                  "true\nfalse\n");
    expect_builtin_output("stdlib net tcp closed read error",
                          "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = "
                          "server.local_port()\n    async func client():\n        conn = await net.connect(\"127.0.0.1\", "
                          "port)\n        conn.close()\n    task = client()\n    conn = await server.accept()\n    conn.close()\n"
                          "    server.close()\n    await task\n    try:\n        conn.read()\n    catch NetworkException error:\n"
                          "        print(error.message)\n",
                          "read on closed resource\n");
    expect_builtin_output("stdlib net invalid port",
                          "import net\ntry:\n    net.connect(\"127.0.0.1\", 70000)\ncatch ValueException error:\n    print("
                          "error.message)\n",
                          "connect expects port in range 0..65535\n");
    expect_builtin_output("stdlib net invalid connect",
                          "import net\ns = net.listen(\"127.0.0.1\", 0)\nport = s.local_port()\ns.close()\ntry:\n    "
                          "net.connect(\"127.0.0.1\", port)\ncatch NetworkException:\n    print(\"failed\")\n",
                          "failed\n");
    expect_output("stdlib net resolve localhost",
                  "import net\naddrs = net.resolve(\"localhost\")\nprint(addrs.length() > 0)\nprint(addrs[0] != \"\")\n",
                  "true\ntrue\n");
    expect_builtin_output("stdlib net resolve invalid host",
                          "import net\ntry:\n    net.resolve(\"bad/host\")\ncatch NetworkException:\n    print(\"failed\")\n",
                          "failed\n");
    expect_output("stdlib net async udp",
                  "import net\nasync func main():\n    recv = net.udp()\n    port = recv.local_port()\n    sender = net.udp()\n"
                  "    async func do_send():\n        await sender.send(\"ping\", \"127.0.0.1\", port)\n    task = do_send()\n"
                  "    packet = await recv.receive()\n    await task\n    print(packet.data.length())\n    recv.close()\n"
                  "    sender.close()\n",
                  "4\n");
    expect_builtin_output("stdlib net async cancellation",
                          "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = "
                          "server.local_port()\n    async func waiter():\n        try:\n            conn = await net.connect("
                          "\"127.0.0.1\", port)\n            await delay(500ms)\n        finally:\n            print(\"cleanup\")\n"
                          "    t = waiter()\n    await delay(0ms)\n    t.cancel()\n    try:\n        await t\n    catch e:\n"
                          "        print(e.message)\n    server.close()\n",
                          "cleanup\nTask was cancelled\n");
    expect_output("stdlib net async timeout",
                  "import net\nasync func main():\n    recv = net.udp()\n    try:\n        await timeout(recv.receive(), "
                  "100ms)\n        print(\"finished\")\n    catch TimeoutException:\n        print(\"timed out\")\n    "
                  "recv.close()\n",
                  "timed out\n");
    expect_builtin_output("stdlib net async exception propagation",
                          "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = "
                          "server.local_port()\n    server.close()\n    try:\n        await net.connect(\"127.0.0.1\", "
                          "port)\n    catch NetworkException:\n        print(\"failed\")\n",
                          "failed\n");
    expect_output("stdlib net concurrent tasks",
                  "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = server.local_port()\n"
                  "    async func client(label):\n        conn = await net.connect(\"127.0.0.1\", port)\n        await "
                  "conn.write(label)\n        conn.close()\n    t1 = client(\"a\")\n    t2 = client(\"b\")\n    c1 = await "
                  "server.accept()\n    c2 = await server.accept()\n    d1 = await c1.read()\n    d2 = await c2.read()\n"
                  "    print(d1.length())\n    print(d2.length())\n    c1.close()\n    c2.close()\n    server.close()\n"
                  "    await t1\n    await t2\n",
                  "1\n1\n");
    expect_output("stdlib net async accept path",
                  "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = server.local_port()\n"
                  "    async func client():\n        conn = await net.connect(\"127.0.0.1\", port)\n        conn.close()\n"
                  "    task = client()\n    conn = await server.accept()\n    print(conn.is_open())\n    conn.close()\n"
                  "    server.close()\n    await task\n",
                  "true\n");
    expect_output("stdlib net udp multiple datagrams",
                  "import net\nrecv = net.udp()\nport = recv.local_port()\nsender = net.udp()\nsender.send(\"one\", "
                  "\"127.0.0.1\", port)\nsender.send(\"two\", \"127.0.0.1\", port)\nfirst = recv.receive()\nsecond = "
                  "recv.receive()\nprint(first.data.length())\nprint(second.data.length())\nrecv.close()\nsender.close()\n",
                  "3\n3\n");
    expect_builtin_output("stdlib net write type error",
                          "import net\nasync func main():\n    server = net.listen(\"127.0.0.1\", 0)\n    port = "
                          "server.local_port()\n    async func client():\n        conn = await net.connect(\"127.0.0.1\", "
                          "port)\n        try:\n            conn.write(1)\n        catch TypeException:\n            print("
                          "\"type\")\n        conn.close()\n    task = client()\n    conn = await server.accept()\n    conn.close()\n"
                          "    server.close()\n    await task\n",
                          "type\n");

    expect_output("stdlib crypto module import", "import crypto\nprint(\"crypto\")\n", "crypto\n");
    expect_output("stdlib crypto sha256 empty",
                  "import crypto\nh = crypto.sha256(\"\")\nprint(h.length())\nprint(h[0])\nprint(h[1])\n",
                  "32\n227\n176\n");
    expect_output("stdlib crypto sha512 empty",
                  "import crypto\nh = crypto.sha512(\"\")\nprint(h.length())\nprint(h[0])\nprint(h[1])\n",
                  "64\n207\n131\n");
    expect_output("stdlib crypto sha3_256 empty",
                  "import crypto\nh = crypto.sha3_256(\"\")\nprint(h.length())\nprint(h[0])\n",
                  "32\n167\n");
    expect_output("stdlib crypto hmac_sha256 rfc4231",
                  "import crypto\nkey = \"Jefe\"\ndata = \"what do ya want for nothing?\"\nh = crypto.hmac_sha256(key, data)\n"
                  "print(h.length())\nprint(h[0])\nprint(h[31])\n",
                  "32\n91\n67\n");
    expect_output("stdlib crypto compare constant time",
                  "import crypto\nimport encoding\na = encoding.encode(\"secret\")\nb = encoding.encode(\"secret\")\n"
                  "c = encoding.encode(\"other!\")\nprint(crypto.compare(a, b))\nprint(crypto.compare(a, c))\n",
                  "true\nfalse\n");
    expect_output("stdlib crypto random_bytes length",
                  "import crypto\nb = crypto.random_bytes(16)\nprint(b.length())\n", "16\n");
    expect_fs_output("stdlib crypto hash_file",
                      "import crypto\nimport fs\nfs.write(\"hashme.txt\", \"abc\")\n"
                      "h = crypto.hash_file(\"hashme.txt\", \"sha256\")\nprint(h.length())\nprint(h[0])\n",
                      "32\n186\n");
    expect_fs_output("stdlib crypto streaming hasher",
                      "import crypto\nimport fs\nfs.write(\"stream.txt\", \"ab\")\n"
                      "h = crypto.hasher(\"sha256\")\nh.update(\"a\")\nh.update(\"b\")\nd = h.digest()\n"
                      "print(d.length())\nprint(d[0])\n",
                      "32\n251\n");
    expect_fs_output("stdlib crypto hash_io",
                      "import crypto\nimport fs\nimport io\nfs.write(\"iohash.txt\", \"data\")\n"
                      "f = io.open(\"iohash.txt\", \"r\")\nh = crypto.hash_io(f, \"sha256\")\nf.close()\n"
                      "print(h.length())\n",
                      "32\n");
    expect_output("stdlib crypto async call",
                  "import crypto\nasync func main():\n    h = crypto.sha256(\"x\")\n    print(h.length())\n",
                  "32\n");
    expect_builtin_output("stdlib crypto invalid compare type",
                          "import crypto\ntry:\n    crypto.compare(1, 2)\ncatch TypeException error:\n    "
                          "print(error.message)\n",
                          "compare expects bytes or string data\n");
    expect_output("stdlib crypto pbkdf2 rfc6070",
                  "import crypto\nk = crypto.pbkdf2_sha256(\"password\", \"salt\", 1, 20)\nprint(k[0])\nprint(k[19])\n",
                  "18\n201\n");
    expect_output("stdlib crypto aes gcm roundtrip",
                  "import crypto\nkey = crypto.random_bytes(32)\nnonce = crypto.random_bytes(12)\n"
                  "enc = crypto.encrypt_aes_gcm(key, nonce, \"hello\", null)\n"
                  "pt = crypto.decrypt_aes_gcm(key, nonce, enc[\"ciphertext\"], enc[\"tag\"], null)\nprint(pt.length())\n",
                  "5\n");
    expect_builtin_output("stdlib crypto aes gcm bad tag",
                          "import crypto\ntry:\n    key = crypto.random_bytes(32)\n    nonce = crypto.random_bytes(12)\n"
                          "    enc = crypto.encrypt_aes_gcm(key, nonce, \"secret\", null)\n"
                          "    bad = crypto.random_bytes(16)\n"
                          "    crypto.decrypt_aes_gcm(key, nonce, enc[\"ciphertext\"], bad, null)\n"
                          "catch ValueException error:\n    print(error.message)\n",
                          "authentication failed\n");
    expect_output("stdlib crypto chacha roundtrip",
                  "import crypto\nkey = crypto.random_bytes(32)\nnonce = crypto.random_bytes(12)\n"
                  "enc = crypto.encrypt_chacha20_poly1305(key, nonce, \"data\", null)\n"
                  "pt = crypto.decrypt_chacha20_poly1305(key, nonce, enc[\"ciphertext\"], enc[\"tag\"], null)\n"
                  "print(pt.length())\n",
                  "4\n");
    expect_builtin_output("stdlib crypto scrypt unavailable without openssl",
                          "import crypto\ntry:\n    crypto.scrypt(\"p\", \"s\", 32, null)\n"
                          "catch ValueException error:\n    print(error.message)\n",
                          "scrypt requires an OpenSSL-enabled build\n");

    expect_output("stdlib sql module import", "import sql\nprint(\"sql\")\n", "sql\n");
    expect_output("stdlib sql memory open",
                  "import sql\ndb = sql.open(\":memory:\")\nprint(db.is_open())\ndb.close()\nprint(db.is_open())\n",
                  "true\nfalse\n");
    expect_fs_output("stdlib sql file database open",
                      "import sql\nimport fs\ndb = sql.open(\"appdata.db\")\nprint(db.is_open())\n"
                      "print(fs.exists(\"appdata.db\"))\ndb.close()\n",
                      "true\ntrue\n");
    expect_fs_output("stdlib sql double close",
                      "import sql\ndb = sql.open(\":memory:\")\ndb.close()\ndb.close()\nprint(db.is_open())\n",
                      "false\n");
    expect_fs_output("stdlib sql module cache",
                      "import sql\nimport sql as s2\nprint(\"ok\")\n",
                      "ok\n");
    expect_builtin_output("stdlib sql invalid open path",
                          "import sql\ntry:\n    sql.open(\"missing_parent/db.sqlite\")\n"
                          "catch RuntimeException error:\n    print(error.message)\n",
                          "failed to open database 'missing_parent/db.sqlite': parent directory does not exist\n");
    expect_builtin_output("stdlib sql empty path",
                          "import sql\ntry:\n    sql.open(\"\")\ncatch ValueException error:\n    "
                          "print(error.message)\n",
                          "database path must not be empty\n");
    expect_output("stdlib sql async regression",
                  "import sql\nasync func main():\n    db = sql.open(\":memory:\")\n    print(db.is_open())\n    db.close()\n",
                  "true\n");

    expect_output("stdlib sql execute insert",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)\")\n"
                  "r = db.execute(\"INSERT INTO users (name, age) VALUES (?, ?)\", [\"Emir\", 15])\n"
                  "print(r.affected_rows)\nprint(r.last_insert_id)\n",
                  "1\n1\n");
    expect_output("stdlib sql execute update delete",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE t (v INTEGER)\")\n"
                  "db.execute(\"INSERT INTO t (v) VALUES (1)\")\n"
                  "db.execute(\"INSERT INTO t (v) VALUES (2)\")\n"
                  "u = db.execute(\"UPDATE t SET v = 9 WHERE v = 1\")\n"
                  "d = db.execute(\"DELETE FROM t WHERE v = 2\")\n"
                  "print(u.affected_rows)\nprint(d.affected_rows)\n",
                  "1\n1\n");
    expect_output("stdlib sql query rows iteration",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE users (name TEXT, age INTEGER)\")\n"
                  "db.execute(\"INSERT INTO users VALUES ('a', 12)\")\n"
                  "db.execute(\"INSERT INTO users VALUES ('b', 20)\")\n"
                  "rows = db.query(\"SELECT name, age FROM users WHERE age > ?\", [10])\n"
                  "names = []\nfor row in rows:\n    names.append(row[\"name\"])\n"
                  "print(names.length())\nprint(names[0])\nprint(names[1])\n",
                  "2\na\nb\n");
    expect_output("stdlib sql query zero rows",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE t (x INTEGER)\")\n"
                  "rows = db.query(\"SELECT x FROM t\")\n"
                  "count = 0\nfor row in rows:\n    count = count + 1\n"
                  "print(count)\n",
                  "0\n");
    expect_output("stdlib sql query_one",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE users (id INTEGER, name TEXT)\")\n"
                  "db.execute(\"INSERT INTO users VALUES (1, 'one')\")\n"
                  "db.execute(\"INSERT INTO users VALUES (2, 'two')\")\n"
                  "u = db.query_one(\"SELECT name FROM users WHERE id = ?\", [1])\n"
                  "missing = db.query_one(\"SELECT name FROM users WHERE id = ?\", [99])\n"
                  "first = db.query_one(\"SELECT name FROM users\")\n"
                  "print(u[\"name\"])\nprint(missing)\nprint(first[\"name\"])\n",
                  "one\nnull\none\n");
    expect_output("stdlib sql binding types",
                  "import sql\nimport encoding\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE t (i INTEGER, f REAL, s TEXT, b BLOB, n TEXT)\")\n"
                  "blob = encoding.encode(\"ab\")\n"
                  "db.execute(\"INSERT INTO t VALUES (?, ?, ?, ?, ?)\", [7, 3.5, \"hi\", blob, null])\n"
                  "row = db.query_one(\"SELECT i, f, s, b, n FROM t\")\n"
                  "print(row[\"i\"])\nprint(row[\"f\"])\nprint(row[\"s\"])\nprint(row[\"b\"].length())\nprint(row[\"n\"])\n",
                  "7\n3.5\nhi\n2\nnull\n");
    expect_builtin_output("stdlib sql binding wrong count",
                          "import sql\ndb = sql.open(\":memory:\")\n"
                          "try:\n    db.query(\"SELECT ?\", [1, 2])\n"
                          "catch ValueException error:\n    print(error.message)\n",
                          "SQL parameter count mismatch: expected 1, got 2\n");
    expect_builtin_output("stdlib sql binding invalid type",
                          "import sql\ndb = sql.open(\":memory:\")\n"
                          "try:\n    db.query(\"SELECT ?\", [{\"a\": 1}])\n"
                          "catch TypeException error:\n    print(error.message)\n",
                          "unsupported SQL parameter type: {\"a\": 1}\n");
    expect_output("stdlib sql injection safe binding",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE users (name TEXT)\")\n"
                  "evil = \"x'); DROP TABLE users; --\"\n"
                  "db.execute(\"INSERT INTO users (name) VALUES (?)\", [evil])\n"
                  "row = db.query_one(\"SELECT name FROM users\")\n"
                  "print(row[\"name\"])\n",
                  "x'); DROP TABLE users; --\n");
    expect_output("stdlib sql prepared statement reuse",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE users (id INTEGER, name TEXT)\")\n"
                  "db.execute(\"INSERT INTO users VALUES (10, 'a')\")\n"
                  "db.execute(\"INSERT INTO users VALUES (20, 'b')\")\n"
                  "stmt = db.prepare(\"SELECT name FROM users WHERE id = ?\")\n"
                  "u1 = stmt.query_one([10])\n"
                  "u2 = stmt.query_one([20])\n"
                  "print(u1[\"name\"])\nprint(u2[\"name\"])\n"
                  "stmt.close()\n",
                  "a\nb\n");
    expect_builtin_output("stdlib sql statement use after close",
                          "import sql\ndb = sql.open(\":memory:\")\n"
                          "stmt = db.prepare(\"SELECT 1\")\n"
                          "stmt.close()\n"
                          "try:\n    stmt.query_one(null)\n"
                          "catch ValueException error:\n    print(error.message)\n",
                          "query_one on a closed statement\n");
    expect_builtin_output("stdlib sql rows early close",
                          "import sql\ndb = sql.open(\":memory:\")\n"
                          "db.execute(\"CREATE TABLE t (x INTEGER)\")\n"
                          "db.execute(\"INSERT INTO t VALUES (1)\")\n"
                          "db.execute(\"INSERT INTO t VALUES (2)\")\n"
                          "rows = db.query(\"SELECT x FROM t\")\n"
                          "rows.close()\n"
                          "try:\n    for row in rows:\n        print(row[\"x\"])\n"
                          "catch ValueException error:\n    print(error.message)\n",
                          "cannot iterate a closed SQL result set\n");
    expect_builtin_output("stdlib sql closed database query",
                          "import sql\ndb = sql.open(\":memory:\")\n"
                          "db.close()\n"
                          "try:\n    db.execute(\"SELECT 1\")\n"
                          "catch ValueException error:\n    print(error.message)\n",
                          "execute on a closed database\n");

    expect_output("stdlib sql transaction commit",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE accounts (id INTEGER, balance INTEGER)\")\n"
                  "db.execute(\"INSERT INTO accounts VALUES (1, 100), (2, 0)\")\n"
                  "tx = db.begin()\n"
                  "tx.execute(\"UPDATE accounts SET balance = balance - ? WHERE id = ?\", [50, 1])\n"
                  "tx.execute(\"UPDATE accounts SET balance = balance + ? WHERE id = ?\", [50, 2])\n"
                  "tx.commit()\n"
                  "row = db.query_one(\"SELECT balance FROM accounts WHERE id = 2\")\n"
                  "print(row[\"balance\"])\n",
                  "50\n");
    expect_output("stdlib sql transaction rollback",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE t (v INTEGER)\")\n"
                  "db.execute(\"INSERT INTO t VALUES (1)\")\n"
                  "tx = db.begin()\n"
                  "tx.execute(\"INSERT INTO t VALUES (2)\")\n"
                  "tx.rollback()\n"
                  "row = db.query_one(\"SELECT COUNT(*) AS c FROM t\")\n"
                  "print(row[\"c\"])\n",
                  "1\n");
    expect_builtin_output("stdlib sql transaction after commit",
                          "import sql\ndb = sql.open(\":memory:\")\n"
                          "tx = db.begin()\n"
                          "tx.commit()\n"
                          "try:\n    tx.execute(\"SELECT 1\")\n"
                          "catch ValueException error:\n    print(error.message)\n",
                          "execute on a committed transaction\n");
    expect_output("stdlib sql savepoint rollback_to",
                  "import sql\ndb = sql.open(\":memory:\")\n"
                  "db.execute(\"CREATE TABLE t (v INTEGER)\")\n"
                  "tx = db.begin()\n"
                  "tx.execute(\"INSERT INTO t VALUES (1)\")\n"
                  "tx.savepoint(\"sp1\")\n"
                  "tx.execute(\"INSERT INTO t VALUES (2)\")\n"
                  "tx.rollback_to(\"sp1\")\n"
                  "tx.commit()\n"
                  "row = db.query_one(\"SELECT COUNT(*) AS c FROM t\")\n"
                  "print(row[\"c\"])\n",
                  "1\n");
    expect_output("stdlib sql async execute",
                  "import sql\nasync func main():\n"
                  "    db = sql.open(\":memory:\")\n"
                  "    db.execute(\"CREATE TABLE t (v INTEGER)\")\n"
                  "    r = await db.async_execute(\"INSERT INTO t (v) VALUES (?)\", [7])\n"
                  "    print(r.affected_rows)\n",
                  "1\n");
    expect_output("stdlib sql async query",
                  "import sql\nasync func main():\n"
                  "    db = sql.open(\":memory:\")\n"
                  "    db.execute(\"CREATE TABLE t (v INTEGER)\")\n"
                  "    db.execute(\"INSERT INTO t VALUES (3)\")\n"
                  "    rows = await db.async_query(\"SELECT v FROM t\")\n"
                  "    for row in rows:\n        print(row[\"v\"])\n",
                  "3\n");
    expect_output("stdlib sql async transaction",
                  "import sql\nasync func main():\n"
                  "    db = sql.open(\":memory:\")\n"
                  "    db.execute(\"CREATE TABLE t (v INTEGER)\")\n"
                  "    tx = db.begin()\n"
                  "    await tx.async_execute(\"INSERT INTO t (v) VALUES (?)\", [9])\n"
                  "    tx.commit()\n"
                  "    row = await db.async_query_one(\"SELECT v FROM t\")\n"
                  "    print(row[\"v\"])\n",
                  "9\n");

    expect_builtin_output("stdlib testing module import", "import testing\nprint(\"testing\")\n", "testing\n");
    expect_builtin_output("stdlib testing discovery pass",
                  "import testing\nfunc test_ok():\n    testing.equal(1, 1)\nfunc test_two():\n    testing.true(true)\n"
                  "testing.run()\n",
                  "Testing...\nok test_ok\nok test_two\n2 passed, 0 failed\n");
    expect_builtin_output("stdlib testing assertion failure",
                  "import testing\nfunc test_bad():\n    testing.equal(1, 2)\nfunc test_good():\n    testing.true(true)\n"
                  "testing.run()\n",
                  "Testing...\nx test_bad\n  Testing assertion failed: expected 2, got 1\nok test_good\n1 passed, 1 "
                  "failed\n");
    expect_process_exit("stdlib testing run exit code on failure",
                        "import testing\nfunc test_bad():\n    testing.fail(\"nope\")\ntesting.run()\n", 1);
    expect_builtin_output("stdlib testing throws",
                  "import testing\nfunc raises():\n    testing.fail(\"boom\")\nfunc test_throws():\n    "
                  "testing.throws(raises)\ntesting.run()\n",
                  "Testing...\nok test_throws\n1 passed, 0 failed\n");
    expect_builtin_output("stdlib testing async test",
                  "import testing\nasync func test_async():\n    await delay(0ms)\n    testing.equal(2, 2)\n"
                  "async func main():\n    testing.run()\n",
                  "Testing...\nok test_async\n1 passed, 0 failed\n");
    expect_builtin_output("stdlib testing sql async cancel",
                  "import testing\nimport sql\nasync func test_sql_async_cancel():\n    db = sql.open(\":memory:\")\n"
                  "    db.execute(\"CREATE TABLE t (v INTEGER)\")\n    task = db.async_execute(\"INSERT INTO t (v) VALUES "
                  "(?)\", [1])\n    await delay(0ms)\n    task.cancel()\n    try:\n        await task\n        "
                  "testing.fail(\"expected cancel\")\n    catch CancellationException:\n        testing.true(true)\n"
                  "async func main():\n    testing.run()\n",
                  "Testing...\nok test_sql_async_cancel\n1 passed, 0 failed\n");

    expect_output("stdlib cli module import", "import cli\nprint(\"cli\")\n", "cli\n");
    expect_source_with_args("stdlib cli positional args",
                            "import cli\nprint(cli.arg(0))\nprint(cli.arg(1))\n",
                            {"build", "game.phs"}, "build\ngame.phs\n");
    expect_source_with_args("stdlib cli option default",
                            "import cli\nname = cli.option(\"name\", \"World\")\nprint(name)\n", {}, "World\n");
    expect_source_with_args("stdlib cli option value",
                            "import cli\nname = cli.option(\"name\", \"World\")\nprint(name)\n", {"--name", "Emir"},
                            "Emir\n");
    expect_source_with_args("stdlib cli flag",
                            "import cli\nverbose = cli.flag(\"verbose\")\nprint(verbose)\n", {"--verbose"}, "true\n");
    expect_source_with_args("stdlib cli has",
                            "import cli\ncli.flag(\"verbose\")\nprint(cli.has(\"verbose\"))\nprint(cli.has(\"missing\"))\n",
                            {"--verbose"}, "true\nfalse\n");
    expect_source_with_args("stdlib cli subcommand",
                            "import cli\nis_build = cli.command(\"build\")\nprint(is_build)\n", {"build"}, "true\n");
    expect_source_with_args("stdlib cli help",
                            "import cli\ncli.help(\"Demo app\")\n", {"--help"},
                            "Usage: [options] [arguments...]\nDemo app\n  --help, -h       show this help\n  "
                            "--version, -V    show version\n");
    expect_source_with_args("stdlib cli version",
                            "import cli\ncli.version(\"demo 1.0\")\n", {"--version"}, "demo 1.0\n");
    expect_source_with_args("stdlib cli unknown option",
                            "import cli\ntry:\n    cli.args()\ncatch ValueException error:\n    print(error.message)\n",
                            {"--unknown"}, "unknown option '--unknown'\n");

#if defined(PSCRIPT_HAS_OPENSSL)
    expect_output("stdlib crypto blake2b empty",
                  "import crypto\nh = crypto.blake2b(\"\")\nprint(h.length())\nprint(h[0])\n", "64\n191\n");
#endif

    expect_output("stdlib log module import", "import log\nprint(\"log\")\n", "log\n");
    expect_log_stderr("stdlib log default filters debug",
                      "import log\nlog.debug(\"hidden\")\nlog.info(\"visible\")\nprint(\"stdout\")\n", "<stdin>",
                      [](const std::string& stdout_text, const std::string& stderr_text) {
                          if (stdout_text != "stdout\n") {
                              throw std::runtime_error("unexpected stdout: " + stdout_text);
                          }
                          if (stderr_text.find("hidden") != std::string::npos) {
                              throw std::runtime_error("debug line leaked to stderr");
                          }
                          if (stderr_text.find("INFO") == std::string::npos ||
                              stderr_text.find("visible") == std::string::npos) {
                              throw std::runtime_error("missing info line: " + stderr_text);
                          }
                      });
    expect_log_stderr("stdlib log structured data",
                      "import log\nlog.info(\"User logged in\", {\"user_id\": 42, \"method\": \"password\"})\n",
                      "<stdin>", [](const std::string&, const std::string& stderr_text) {
                          if (stderr_text.find("User logged in") == std::string::npos ||
                              stderr_text.find("user_id=42") == std::string::npos ||
                              stderr_text.find("method=password") == std::string::npos) {
                              throw std::runtime_error("structured log missing fields: " + stderr_text);
                          }
                      });
    expect_log_stderr("stdlib log set_level debug",
                      "import log\nlog.set_level(log.DEBUG)\nlog.debug(\"trace\")\n", "<stdin>",
                      [](const std::string&, const std::string& stderr_text) {
                          if (stderr_text.find("DEBUG") == std::string::npos ||
                              stderr_text.find("trace") == std::string::npos) {
                              throw std::runtime_error("debug not logged after set_level: " + stderr_text);
                          }
                      });
    expect_log_stderr("stdlib log source location", "import log\n\nlog.info(\"marker\")\n", "app.phs",
                      [](const std::string&, const std::string& stderr_text) {
                          if (stderr_text.find("app.phs:3 marker") == std::string::npos) {
                              throw std::runtime_error("missing source location: " + stderr_text);
                          }
                      });
    expect_log_stderr("stdlib log warning and error",
                      "import log\nlog.warning(\"warn\")\nlog.error(\"err\")\n", "<stdin>",
                      [](const std::string&, const std::string& stderr_text) {
                          if (stderr_text.find("WARNING") == std::string::npos ||
                              stderr_text.find("warn") == std::string::npos ||
                              stderr_text.find("ERROR") == std::string::npos ||
                              stderr_text.find("err") == std::string::npos) {
                              throw std::runtime_error("missing severity lines: " + stderr_text);
                          }
                      });
    expect_output("stdlib log async main",
                  "import log\nasync func main():\n    log.info(\"async\")\n    print(\"done\")\n", "done\n");
    expect_log_stderr("stdlib log async stderr",
                      "import log\nasync func main():\n    log.info(\"async\")\n    print(\"done\")\n", "<stdin>",
                      [](const std::string& stdout_text, const std::string& stderr_text) {
                          if (stdout_text != "done\n") {
                              throw std::runtime_error("unexpected stdout: " + stdout_text);
                          }
                          if (stderr_text.find("INFO") == std::string::npos ||
                              stderr_text.find("async") == std::string::npos) {
                              throw std::runtime_error("missing async log: " + stderr_text);
                          }
                      });
    expect_builtin_output("stdlib log invalid set_level",
                          "import log\ntry:\n    log.set_level(\"INVALID\")\ncatch ValueException error:\n    "
                          "print(error.message)\n",
                          "invalid log level 'INVALID'\n");
    expect_builtin_output("stdlib log structured type error",
                          "import log\ntry:\n    log.info(\"x\", 1)\ncatch TypeException error:\n    "
                          "print(error.message)\n",
                          "log structured data must be a dict\n");

    expect_output("stdlib http module import", "import http\nprint(\"http\")\n", "http\n");
    expect_output("stdlib http client get",
                  "import http\nasync func main():\n    server = http.server(0)\n    port = server.local_port()\n    "
                  "func handler(req):\n        return \"ok\"\n    server.get(\"/\", handler)\n    server.start()\n    "
                  "url = \"http://127.0.0.1:\" + str(port) + \"/\"\n    response = await http.get(url)\n    "
                  "print(response.status)\n    print(response.body.length())\n    server.close()\n",
                  "200\n2\n");
    expect_output("stdlib http client post body",
                  "import http\nasync func main():\n    server = http.server(0)\n    port = server.local_port()\n    "
                  "func echo(req):\n        return req.body\n    server.post(\"/echo\", echo)\n    server.start()\n    "
                  "url = \"http://127.0.0.1:\" + str(port) + \"/echo\"\n    response = await http.post(url, \"data\")\n    "
                  "print(response.status)\n    print(response.body.length())\n    server.close()\n",
                  "200\n4\n");
    expect_output("stdlib http request headers",
                  "import http\nasync func main():\n    server = http.server(0)\n    port = server.local_port()\n    "
                  "func header(req):\n        return req.headers[\"x-test\"]\n    server.get(\"/\", header)\n    "
                  "server.start()\n    url = \"http://127.0.0.1:\" + str(port) + \"/\"\n    response = await http.request("
                  "\"GET\", url, null, {\"X-Test\": \"abc\"})\n    print(response.status)\n    print(response.body.length())\n"
                  "    server.close()\n",
                  "200\n3\n");
    expect_output("stdlib http server sync handler",
                  "import http\nasync func main():\n    server = http.server(0)\n    port = server.local_port()\n    "
                  "func ping(req):\n        return \"pong\"\n    server.get(\"/ping\", ping)\n    server.start()\n    "
                  "url = \"http://127.0.0.1:\" + str(port) + \"/ping\"\n    response = await http.get(url)\n    "
                  "print(response.body.length())\n    server.close()\n",
                  "4\n");
    expect_output("stdlib http async client",
                  "import http\nasync func main():\n    server = http.server(0)\n    port = server.local_port()\n    "
                  "func handler(req):\n        return \"ok\"\n    server.get(\"/\", handler)\n    server.start()\n    "
                  "url = \"http://127.0.0.1:\" + str(port) + \"/\"\n    response = await http.get(url)\n    "
                  "print(response.status)\n    server.close()\n",
                  "200\n");
    expect_output("stdlib http async handler",
                  "import http\nasync func main():\n    server = http.server(0)\n    port = server.local_port()\n    "
                  "async func handler(req):\n        return \"async\"\n    server.get(\"/\", handler)\n    server.start()\n"
                  "    url = \"http://127.0.0.1:\" + str(port) + \"/\"\n    response = await http.get(url)\n    "
                  "print(response.body.length())\n    server.close()\n",
                  "5\n");

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All VM tests passed\n";
    return 0;
}
