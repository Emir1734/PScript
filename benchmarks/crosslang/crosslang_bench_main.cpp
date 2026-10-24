#include "crosslang_bench.h"

int main(int argc, char** argv) {
    const std::string_view workload = crosslang::workload_name(argc, argv);
    if (workload == "verify") {
        std::cout << "loops\t" << crosslang::run_pscript_loops() << '\n';
        std::cout << "num_lu_divmod\t" << crosslang::run_pscript_num_lu_divmod() << '\n';
        std::cout << "closure_call\t" << crosslang::run_pscript_closure_call() << '\n';
        std::cout << "closure_nested\t" << crosslang::run_pscript_closure_nested() << '\n';
        std::cout << "closure_recursive\t" << crosslang::run_pscript_closure_recursive() << '\n';
        std::cout << "closure_recursive_iter\t" << crosslang::run_pscript_closure_recursive_iter() << '\n';
        return 0;
    }
    const std::uint64_t iters =
        crosslang::bench_iterations(argc, argv, crosslang::default_workload_iterations(workload));
    std::int64_t checksum = 0;
    double median = 0.0;
    if (workload == "loops") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_pscript_loops(); }, 7);
    } else if (workload == "num_lu_divmod") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_pscript_num_lu_divmod(); }, 7);
    } else if (workload == "closure_call") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_pscript_closure_call(); }, 7);
    } else if (workload == "closure_nested") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_pscript_closure_nested(); }, 7);
    } else if (workload == "closure_recursive") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_pscript_closure_recursive(); }, 7);
    } else if (workload == "closure_recursive_iter") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_pscript_closure_recursive_iter(); }, 7);
    } else if (workload == "int_loop") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_int_loop(iters); }, 7);
    } else if (workload == "list_index") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_list_index(iters); }, 7);
    } else if (workload == "string_eq") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_string_eq(iters); }, 7);
    } else if (workload == "string_concat") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_string_concat(iters); }, 7);
    } else if (workload == "direct_inc_call") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_direct_inc_call(iters); }, 7);
    } else if (workload == "nested_call") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_nested_call(iters); }, 7);
    } else if (workload == "closure_call_legacy") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_closure_call(iters, 1); }, 7);
    } else if (workload == "add_constant") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_add_constant(iters, 7); }, 7);
    } else if (workload == "indirect_call") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_indirect_call(iters); }, 7);
    } else if (workload == "arithmetic") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_arithmetic(iters); }, 7);
    } else if (workload == "dict_lookup") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_dict_lookup(iters); }, 7);
    } else if (workload == "json_parse") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_json_parse(iters); }, 7);
    } else if (workload == "json_stringify") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_json_stringify(iters); }, 7);
    } else if (workload == "json_pipeline") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_json_pipeline(iters); }, 7);
    } else if (workload == "mixed_runtime") {
        median = crosslang::median_ms([&] { checksum = crosslang::run_mixed_runtime(iters); }, 7);
    } else {
        std::cerr << "unsupported workload: " << workload << '\n';
        return 2;
    }
    crosslang::print_timing("cpp", workload, median, checksum);
    return 0;
}
