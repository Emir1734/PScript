#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace pscript {

struct ExecuteOptions {
    bool enable_profiling = false;
    std::string profile_output_path;
    bool print_profile_report = false;
    std::string pgo_profile_path;
};

std::string strip_utf8_bom(std::string source);

int check_source(std::string source, const std::string& source_file, std::ostream& err);

int execute_source(std::string source, const std::string& source_file, const std::vector<std::string>& program_args,
                   std::ostream& out, std::ostream& err, const ExecuteOptions& options = {});

bool read_source_file(const std::string& path, std::string& source, std::ostream& err);

struct TestFileRunResult {
    int exit_code = 0;
    std::size_t tests_discovered = 0;
};

// Compile a test module and run test_* functions (does not execute program main).
TestFileRunResult run_test_source_file(const std::string& path, std::ostream& out, std::ostream& err);

void print_located_error(std::ostream& err, std::string_view reason, int line, int column);
void print_compile_error(std::ostream& err, std::string_view reason, int line, int column);
void print_cli_error(std::ostream& err, std::string_view reason);
void print_vm_error(std::ostream& err, const std::string& message);

}  // namespace pscript
