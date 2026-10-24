#include "cli/pipeline.h"

#include "cli/exit_codes.h"
#include "compiler/compiler.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "compiler/pgo.h"
#include "runtime/profiler.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include <fstream>
#include <iterator>
#include <string_view>

namespace pscript {

std::string strip_utf8_bom(std::string source) {
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB && static_cast<unsigned char>(source[2]) == 0xBF) {
        source.erase(0, 3);
    }
    return source;
}

void print_cli_error(std::ostream& err, std::string_view reason) {
    err << "Error: " << reason << '\n';
}

void print_compile_error(std::ostream& err, std::string_view reason, int line, int column) {
    err << "Compile error: " << reason << '\n';
    err << "line: " << line << '\n';
    err << "column: " << column << '\n';
}

void print_located_error(std::ostream& err, std::string_view reason, int line, int column) {
    print_compile_error(err, reason, line, column);
}

void print_vm_error(std::ostream& err, const std::string& message) {
    if (message.rfind("Unhandled exception:", 0) == 0) {
        err << message;
        if (!message.empty() && message.back() != '\n') {
            err << '\n';
        }
        return;
    }
    err << "Error: " << message << '\n';
}

bool read_source_file(const std::string& path, std::string& source, std::ostream& err) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        print_cli_error(err, "cannot open file '" + path + "'");
        return false;
    }
    source.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (in.bad()) {
        print_cli_error(err, "cannot open file '" + path + "'");
        return false;
    }
    return true;
}

namespace {

std::string with_exception_prelude(std::string source) {
    source = strip_utf8_bom(std::move(source));
    return std::string(exception_prelude_source()) + source;
}

int map_compile_exception(const LexerError& ex, std::ostream& err) {
    print_compile_error(err, ex.message(), ex.line(), ex.column());
    return kExitCompile;
}

int map_compile_exception(const ParserError& ex, std::ostream& err) {
    print_compile_error(err, ex.message(), ex.line(), ex.column());
    return kExitCompile;
}

int map_compile_exception(const CompilerError& ex, std::ostream& err) {
    print_compile_error(err, ex.message(), ex.line(), ex.column());
    return kExitCompile;
}

}  // namespace

int check_source(std::string source, const std::string& source_file, std::ostream& err) {
    try {
        source = with_exception_prelude(std::move(source));
        Lexer lexer(std::move(source));
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        Program program = parser.parse();
        CompileOptions options;
        options.source_file = source_file;
        options.source_line_base = exception_prelude_line_count();
        compile(program, options);
        return kExitSuccess;
    } catch (const LexerError& ex) {
        return map_compile_exception(ex, err);
    } catch (const ParserError& ex) {
        return map_compile_exception(ex, err);
    } catch (const CompilerError& ex) {
        return map_compile_exception(ex, err);
    } catch (const std::exception& ex) {
        print_cli_error(err, ex.what());
        return kExitCompile;
    }
}

namespace {

std::size_t count_discovered_tests(const Bytecode& bytecode) {
    std::size_t count = 0;
    for (const FunctionInfo& fn : bytecode.functions) {
        if (fn.name.size() >= 5 && fn.name.compare(0, 5, "test_") == 0) {
            ++count;
        }
    }
    return count;
}

Bytecode compile_source_to_bytecode(std::string source, const std::string& source_file, std::ostream& err) {
    source = with_exception_prelude(std::move(source));
    Lexer lexer(std::move(source));
    std::vector<Token> tokens = lexer.tokenize();
    Parser parser(std::move(tokens));
    Program program = parser.parse();
    CompileOptions compile_options;
    compile_options.source_file = source_file;
    compile_options.source_line_base = exception_prelude_line_count();
    return compile(program, compile_options);
}

}  // namespace

TestFileRunResult run_test_source_file(const std::string& path, std::ostream& out, std::ostream& err) {
    TestFileRunResult result;
    std::string source;
    if (!read_source_file(path, source, err)) {
        result.exit_code = kExitCompile;
        return result;
    }
    try {
        Bytecode bytecode = compile_source_to_bytecode(std::move(source), path, err);
        result.tests_discovered = count_discovered_tests(bytecode);
        if (result.tests_discovered == 0) {
            result.exit_code = kExitSuccess;
            return result;
        }
        Runtime runtime(out);
        Vm vm(std::move(bytecode), runtime);
        const int failed = vm.run_project_tests(std::nullopt);
        if (failed > 0) {
            result.exit_code = kExitRuntime;
            return result;
        }
        if (vm.process_exit_requested()) {
            result.exit_code = vm.process_exit_code();
            return result;
        }
        result.exit_code = kExitSuccess;
        return result;
    } catch (const LexerError& ex) {
        result.exit_code = map_compile_exception(ex, err);
        return result;
    } catch (const ParserError& ex) {
        result.exit_code = map_compile_exception(ex, err);
        return result;
    } catch (const CompilerError& ex) {
        result.exit_code = map_compile_exception(ex, err);
        return result;
    } catch (const std::exception& ex) {
        print_cli_error(err, ex.what());
        result.exit_code = kExitCompile;
        return result;
    }
}

int execute_source(std::string source, const std::string& source_file, const std::vector<std::string>& program_args,
                   std::ostream& out, std::ostream& err, const ExecuteOptions& execute_options) {
    try {
        source = with_exception_prelude(std::move(source));
        Lexer lexer(std::move(source));
        std::vector<Token> tokens = lexer.tokenize();
        Parser parser(std::move(tokens));
        Program program = parser.parse();
        CompileOptions compile_options;
        compile_options.source_file = source_file;
        compile_options.source_line_base = exception_prelude_line_count();
        Bytecode bytecode = compile(program, compile_options);
        if (!execute_options.pgo_profile_path.empty()) {
            std::string profile_error;
            const std::optional<ProfileDocument> profile =
                load_profile_document(execute_options.pgo_profile_path, profile_error);
            if (profile.has_value()) {
                apply_profile_guided_optimizations(bytecode, *profile);
            }
        }
        Runtime runtime(out);
        runtime.set_program_args(program_args);
        if (execute_options.enable_profiling) {
            runtime.profiler().set_enabled(true);
        }
        Vm vm(std::move(bytecode), runtime);
        vm.run();
        if (execute_options.enable_profiling) {
            ProfileDocument profile = runtime.profiler().finalize(vm.bytecode());
            profile.program_fingerprint = compute_program_fingerprint(vm.bytecode());
            profile.pgo_compat_version = kPgoCompatVersion;
            if (!execute_options.profile_output_path.empty()) {
                std::string profile_error;
                if (!save_profile_document(profile, execute_options.profile_output_path, profile_error)) {
                    print_cli_error(err, profile_error);
                    return kExitRuntime;
                }
            }
            if (execute_options.print_profile_report) {
                write_profile_report(profile, out);
            }
        }
        if (vm.process_exit_requested()) {
            return vm.process_exit_code();
        }
        return kExitSuccess;
    } catch (const LexerError& ex) {
        return map_compile_exception(ex, err);
    } catch (const ParserError& ex) {
        return map_compile_exception(ex, err);
    } catch (const CompilerError& ex) {
        return map_compile_exception(ex, err);
    } catch (const VmError& ex) {
        print_vm_error(err, ex.message());
        return kExitRuntime;
    } catch (const RuntimeError& ex) {
        print_cli_error(err, ex.message());
        return kExitRuntime;
    } catch (const std::exception& ex) {
        print_cli_error(err, ex.what());
        return kExitRuntime;
    }
}

}  // namespace pscript
