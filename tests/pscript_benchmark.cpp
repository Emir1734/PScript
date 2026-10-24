#include "benchmark/bench_harness.h"
#include "benchmark/bench_workloads.h"

#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_backend.h"
#include "jit/jit_compiler.h"
#include "jit/jit_ic.h"
#include "jit/jit_opt_profile.h"
#include "jit/jit_runtime.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "runtime/profiler.h"
#include "runtime/runtime.h"
#include "version.h"
#include "vm/vm.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

using pscript::Bytecode;
using pscript::CompileOptions;
using pscript::Lexer;
using pscript::ProfileDocument;
using pscript::ProfileHotness;
using pscript::Runtime;
using pscript::Vm;
using pscript::compile;
using pscript::compile_with_builtins;
using pscript::parse;

pscript::bench::BenchRecord make_record(std::string name, std::string category, std::string tier, std::string phase,
                                        pscript::bench::SampleStats stats) {
    pscript::bench::BenchRecord record;
    record.name = std::move(name);
    record.category = std::move(category);
    record.tier = std::move(tier);
    record.phase = std::move(phase);
    record.stats = stats;
    return record;
}

ProfileDocument hot_profile_for(const Bytecode& bc, const std::string& fn_name) {
    ProfileDocument doc;
    doc.version = ProfileDocument::kFormatVersion;
    doc.pgo_compat_version = pscript::kPgoCompatVersion;
    doc.program_fingerprint = pscript::compute_program_fingerprint(bc);
    for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
        if (bc.functions[i].name == fn_name) {
            pscript::ProfileFunctionRecord fn;
            fn.function_index = i;
            fn.calls = 50'000;
            fn.hotness = ProfileHotness::Hot;
            doc.functions.push_back(fn);
            pscript::ProfileLoopRecord loop;
            loop.function_index = i;
            loop.ip = bc.functions[i].entry;
            loop.iterations = 100'000;
            doc.loops.push_back(loop);
            break;
        }
    }
    return doc;
}

std::uint32_t function_index_by_name(const Bytecode& bc, const std::string& name) {
    for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
        if (bc.functions[i].name == name) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

struct RunResult {
    std::int64_t elapsed_ns = 0;
    std::string output;
    std::optional<pscript::ProfileMemoryRecord> memory;
    std::size_t compile_attempts = 0;
    std::size_t optimizing_compile_attempts = 0;
};

struct WarmBenchResult {
    std::int64_t cold_ns = 0;
    std::size_t compile_attempts_cold = 0;
    pscript::bench::SampleStats steady;
    std::string output;
};

WarmBenchResult run_program_warm(const Bytecode& bytecode_template, bool jit, const ProfileDocument* profile,
                                 const pscript::jit::JitTieringConfig* config_override,
                                 const pscript::bench::BenchOptions& options) {
    Bytecode bc = bytecode_template;
    if (profile) {
        pscript::apply_profile_guided_optimizations(bc, *profile);
    }
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(std::move(bc), runtime);
    vm.set_jit_enabled(jit);
    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;
    config.optimizing_invocation_threshold = 100'000;
    config.require_profile_for_optimizing = true;
    if (config_override) {
        config = *config_override;
    }
    vm.set_jit_config(config);
    if (jit) {
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        if (profile) {
            vm.jit_engine().set_profile(profile);
        }
    }
    WarmBenchResult result;
    const std::int64_t t0 = pscript::bench::steady_now_ns();
    out.str("");
    out.clear();
    vm.run();
    result.cold_ns = pscript::bench::steady_now_ns() - t0;
    result.compile_attempts_cold = vm.jit_engine().compile_attempts();
    for (int w = 0; w < options.warmup; ++w) {
        out.str("");
        out.clear();
        vm.run();
    }
    std::vector<std::int64_t> samples;
    samples.reserve(static_cast<std::size_t>(options.iterations));
    for (int i = 0; i < options.iterations; ++i) {
        out.str("");
        out.clear();
        const std::int64_t ts = pscript::bench::steady_now_ns();
        vm.run();
        samples.push_back(pscript::bench::steady_now_ns() - ts);
    }
    result.steady = pscript::bench::summarize_samples(std::move(samples));
    result.output = out.str();
    return result;
}

RunResult run_program(const Bytecode& bytecode_template, bool jit, const ProfileDocument* profile,
                      const pscript::jit::JitTieringConfig* config_override, bool profile_memory) {
    Bytecode bc = bytecode_template;
    if (profile) {
        pscript::apply_profile_guided_optimizations(bc, *profile);
    }
    std::ostringstream out;
    Runtime runtime(out);
    runtime.profiler().set_enabled(profile_memory);
    if (profile_memory) {
        runtime.profiler().begin_session();
    }
    Vm vm(std::move(bc), runtime);
    vm.set_jit_enabled(jit);
    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;
    config.optimizing_invocation_threshold = 100'000;
    config.require_profile_for_optimizing = true;
    if (config_override) {
        config = *config_override;
    }
    vm.set_jit_config(config);
    if (jit) {
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        if (profile) {
            vm.jit_engine().set_profile(profile);
        }
    }
    const std::int64_t t0 = pscript::bench::steady_now_ns();
    out.str("");
    out.clear();
    vm.run();
    RunResult result;
    result.elapsed_ns = pscript::bench::steady_now_ns() - t0;
    result.output = out.str();
    result.compile_attempts = vm.jit_engine().compile_attempts();
    result.optimizing_compile_attempts = vm.jit_engine().optimizing_compile_attempts();
    if (profile_memory) {
        runtime.profiler().end_session();
        result.memory = runtime.profiler().finalize(vm.bytecode()).memory;
    }
    return result;
}

std::string probe_jit_hot_ic_metrics(const pscript::bench::BenchOptions& options, const char* source,
                                     const char* hot_fn_name) {
    Bytecode bc = compile_with_builtins(std::string(source));
    const std::uint32_t hot_idx = function_index_by_name(bc, hot_fn_name);
    ProfileDocument profile =
        hot_idx != 0xFFFFFFFFu ? hot_profile_for(bc, hot_fn_name) : hot_profile_for(bc, bc.functions[0].name);
    pscript::apply_profile_guided_optimizations(bc, profile);
    std::ostringstream out;
    Runtime runtime(out);
    Vm vm(std::move(bc), runtime);
    vm.set_jit_enabled(true);
    pscript::jit::JitTieringConfig opt_cfg;
    opt_cfg.baseline_invocation_threshold = 1;
    opt_cfg.optimizing_invocation_threshold = 1;
    opt_cfg.require_profile_for_optimizing = false;
    vm.set_jit_config(opt_cfg);
    vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
    vm.jit_engine().set_profile(&profile);
    pscript::jit::set_ic_timing_enabled(options.jit_ic_profile);
    for (int w = 0; w < options.warmup; ++w) {
        out.str("");
        out.clear();
        vm.run();
    }
    pscript::jit::reset_ic_metrics(vm.jit_engine().ic_metrics());
    out.str("");
    out.clear();
    vm.run();
    pscript::jit::set_ic_timing_enabled(false);
    std::string metrics = pscript::jit::format_ic_metrics(vm.jit_engine().ic_metrics());
    if (vm.jit_engine().ic_metrics().emit_mono_invoke_fused == 0 &&
        vm.jit_engine().ic_metrics().nested_begin_mono_call_value == 0 &&
        vm.jit_engine().ic_metrics().call_mono_hits == 0) {
        metrics += " note=zero_counters_may_indicate_callvalue_inlined_or_vm_osr_path";
    }
    metrics += " native_jit_completions=" + std::to_string(vm.jit_engine().native_jit_completions());
    if (hot_idx != 0xFFFFFFFFu) {
        if (const pscript::jit::JitCompiledCode* compiled = vm.jit_engine().lookup(hot_idx)) {
            metrics += " bench_ir_insn=" + std::to_string(compiled->ir_instruction_count);
            metrics += " bench_code_bytes=" + std::to_string(compiled->code_size);
            metrics += " dispatch_loop=" + std::string(compiled->uses_dispatch_loop ? "1" : "0");
            metrics += " cold_calls_outside_loop=" + std::string(compiled->cold_calls_outside_loop ? "1" : "0");
            metrics += " buffered_locals=" + std::string(compiled->buffered_integer_locals ? "1" : "0");
        } else {
            metrics += " bench_compile=missing";
        }
        metrics += " jit_compile_attempts=" + std::to_string(vm.jit_engine().compile_attempts());
        metrics += " optimizing_compile_attempts=" + std::to_string(vm.jit_engine().optimizing_compile_attempts());
        for (std::uint32_t i = 0; i < vm.bytecode().functions.size(); ++i) {
            if (const pscript::jit::JitCompiledCode* fn_code = vm.jit_engine().lookup(i)) {
                metrics += " fn_" + vm.bytecode().functions[i].name + "_code_bytes=" +
                           std::to_string(fn_code->code_size);
            }
        }
    }
    return metrics;
}

std::string probe_closure_call_ic_metrics(const pscript::bench::BenchOptions& options) {
    return probe_jit_hot_ic_metrics(options, pscript::bench::kRuntimeClosureCall, "bench");
}

bool include_category(const pscript::bench::BenchOptions& options, std::string_view category) {
    if (options.filter_prefix.empty()) {
        return true;
    }
    if (options.filter_prefix.rfind(category, 0) == 0) {
        return true;
    }
    const std::string prefix = std::string(category);
    return options.filter_prefix.rfind(prefix, 0) == 0;
}

bool include_jit_exec_slug(const pscript::bench::BenchOptions& options, std::string_view slug) {
    if (options.filter_prefix.empty()) {
        return true;
    }
    const std::string jit_name = "jit.exec." + std::string(slug);
    return pscript::bench::bench_record_matches_filter(jit_name, options.filter_prefix);
}

bool include_runtime_slug(const pscript::bench::BenchOptions& options, std::string_view slug) {
    if (options.filter_prefix.empty()) {
        return true;
    }
    if (options.filter_prefix.rfind("jit.exec.", 0) == 0) {
        return false;
    }
    const std::string runtime_name = "runtime." + std::string(slug);
    if (pscript::bench::bench_record_matches_filter(runtime_name, options.filter_prefix)) {
        return true;
    }
    const std::string jit_name = "jit.exec." + std::string(slug);
    return pscript::bench::bench_record_matches_filter(jit_name, options.filter_prefix);
}

bool include_record_name(const pscript::bench::BenchOptions& options, std::string_view name) {
    return pscript::bench::bench_record_matches_filter(name, options.filter_prefix);
}

bool runtime_slug_tracks_memory(std::string_view slug) {
    return slug == "loops" || slug == "collections" || slug == "strings" || slug == "string_eq" ||
           slug == "string_equal_same_ref" || slug == "string_equal_diff_object" || slug == "string_equal_diff_len" ||
           slug == "string_equal_same_len_diff" || slug == "string_equal_long" || slug == "string_equal_short" ||
           slug == "string_equal_mixed" || slug == "string_search" || slug == "string_methods" ||
           slug == "string_heavy_loop" || slug == "mixed_string_collection" || slug == "closure_call" ||
           slug == "dict_lookup" || slug == "dict_lu_str_hit" || slug == "dict_lu_str_miss" ||
           slug == "dict_lu_int_hit" || slug == "dict_lu_int_miss" || slug == "dict_lu_same_key" ||
           slug == "dict_lu_scatter" || slug == "dict_lu_large" || slug == "dict_lu_collision" ||
           slug == "dict_lu_insert_mix" || slug == "dict_lu_mixed" || slug == "list_iterate" || slug == "calls";
}

void add_runtime_suite(const pscript::bench::BenchOptions& options, std::vector<pscript::bench::BenchRecord>& records,
                       std::string_view slug, const char* source) {
    if (!include_runtime_slug(options, slug)) {
        return;
    }
    const Bytecode bc = compile_with_builtins(std::string(source));

    {
        const auto stats = pscript::bench::time_repeated(options, [&] {
            (void)run_program(bc, false, nullptr, nullptr, false);
        });
        records.push_back(make_record("runtime." + std::string(slug), "runtime", "vm", "steady", stats));
    }
    {
        const std::int64_t cold = run_program(bc, false, nullptr, nullptr, false).elapsed_ns;
        records.push_back(make_record("runtime." + std::string(slug), "runtime", "vm", "cold",
                                      pscript::bench::summarize_samples({cold})));
    }

    if (runtime_slug_tracks_memory(slug)) {
        RunResult mem_run = run_program(bc, false, nullptr, nullptr, true);
        auto record = make_record("runtime." + std::string(slug) + ".alloc", "memory", "vm", "steady",
                                  pscript::bench::summarize_samples({mem_run.elapsed_ns}));
        if (mem_run.memory) {
            record.memory_allocation_count = mem_run.memory->allocation_count;
            record.memory_allocation_bytes = mem_run.memory->allocation_bytes;
        }
        records.push_back(std::move(record));
    }
}

void add_compiler_suite(const pscript::bench::BenchOptions& options, std::vector<pscript::bench::BenchRecord>& records) {
    if (!include_category(options, "compiler.")) {
        return;
    }
    const std::string small = pscript::bench::generated_functions_source(20);
    const std::string medium = pscript::bench::generated_functions_source(120);
    const std::string large = pscript::bench::generated_functions_source(180);

    const struct {
        const char* slug;
        const std::string* source;
    } sizes[] = {
        {"small", &small},
        {"medium", &medium},
        {"large", &large},
    };

    for (const auto& item : sizes) {
        const std::string& source = *item.source;
        {
            const auto stats = pscript::bench::time_repeated(options, [&] {
                (void)Lexer(source).tokenize();
            });
            records.push_back(make_record("compiler.lexer." + std::string(item.slug), "compiler", "lexer", "steady",
                                        stats));
        }
        {
            const auto stats = pscript::bench::time_repeated(options, [&] {
                (void)parse(source);
            });
            records.push_back(make_record("compiler.parser." + std::string(item.slug), "compiler", "parser", "steady",
                                        stats));
        }
        {
            const auto stats = pscript::bench::time_repeated(options, [&] {
                (void)compile_with_builtins(source);
            });
            records.push_back(make_record("compiler.full." + std::string(item.slug), "compiler", "full", "steady",
                                        stats));
        }
        CompileOptions compile_options;
        compile_options.source_file = "<bench>";
        const auto program = parse(source);
        {
            const auto stats = pscript::bench::time_repeated(options, [&] {
                (void)compile(program, compile_options);
            });
            records.push_back(make_record("compiler.bytecode." + std::string(item.slug), "compiler", "bytecode",
                                        "steady", stats));
        }
    }
}

bool slug_has_warm_cold_split(std::string_view slug) {
    return slug == "calls" || slug == "nested_calls" || slug == "simple_calls" || slug == "recursive_sum" ||
           slug == "callvalue_mono" || slug == "callvalue_poly" ||
           slug == "inline_tiny" || slug == "inline_medium" || slug == "inline_chain2" ||
           slug == "inline_chain3" || slug == "inline_large" || slug == "inline_cold" ||
           slug == "closure_call" || slug == "closure_mutable" || slug == "closure_nested" ||
           slug == "closure_create" || slug == "closure_poly" || slug == "closure_shared" ||
           slug == "closure_recursive" ||
           slug == "loops" || slug == "perf_lu_local_rw" || slug == "num_lu_divmod" ||
           slug == "strings" || slug == "string_eq" || slug == "string_equal_same_ref" ||
           slug == "string_equal_diff_object" || slug == "string_equal_diff_len" || slug == "string_equal_same_len_diff" ||
           slug == "string_equal_long" || slug == "string_equal_short" || slug == "string_equal_mixed" ||
           slug == "string_search" || slug == "string_methods" || slug == "string_heavy_loop" ||
           slug == "mixed_string_collection" || slug == "collections" || slug == "kernel_large";
}

void bench_progress(const char* step);

void add_jit_exec_runtime_suite(const pscript::bench::BenchOptions& options,
                                std::vector<pscript::bench::BenchRecord>& records, std::string_view slug,
                                const char* source) {
    if (!pscript::jit::jit_backend_available()) {
        return;
    }
    if (!options.filter_prefix.empty() && options.filter_prefix.rfind("jit.exec.", 0) != 0 &&
        options.filter_prefix.rfind("runtime.", 0) != 0) {
        return;
    }
    if (!include_jit_exec_slug(options, slug)) {
        return;
    }
    Bytecode bc = compile_with_builtins(std::string(source));
    std::string hot_fn = "acc";
    if (slug == "integer_ops") {
        hot_fn = "imul";
    } else if (slug == "float_ops") {
        hot_fn = "fsum";
    } else if (slug == "loops") {
        hot_fn = "loop";
    } else if (slug == "perf_lu_local_rw") {
        hot_fn = "local_rw";
    } else if (slug == "calls" || slug == "nested_calls") {
        hot_fn = slug == "nested_calls" ? "d" : "top";
    } else if (slug == "simple_calls") {
        hot_fn = "hot";
    } else if (slug == "num_lu_divmod") {
        hot_fn = "divmod_loop";
    } else if (slug == "num_lu_int_cmp" || slug == "num_lu_mixed" || slug == "num_lu_nested" || slug == "num_lu_short" ||
               slug == "num_lu_convert") {
        hot_fn = slug == "num_lu_int_cmp" ? "cmp_loop" : slug == "num_lu_mixed" ? "mixed" :
                                                  slug == "num_lu_nested" ? "nested_sum" :
                                                  slug == "num_lu_short" ? "short_loop" : "convert";
    } else if (slug == "recursive_sum") {
        hot_fn = "rs";
    } else if (slug == "callvalue_mono" || slug == "callvalue_poly") {
        hot_fn = "hot";
    } else if (slug == "collections") {
        hot_fn = "coll";
    } else if (slug == "collection_allocation") {
        hot_fn = "allocate";
    } else if (slug == "field_access") {
        hot_fn = "read_fields";
    } else if (slug == "list_index") {
        hot_fn = "index_loop";
    } else if (slug == "dict_lookup") {
        hot_fn = "dict_lookup";
    } else if (slug == "dict_lu_str_hit" || slug == "dict_lu_str_miss" || slug == "dict_lu_int_hit" ||
               slug == "dict_lu_int_miss" || slug == "dict_lu_same_key" || slug == "dict_lu_scatter" ||
               slug == "dict_lu_large" || slug == "dict_lu_collision" || slug == "dict_lu_insert_mix" ||
               slug == "dict_lu_mixed") {
        hot_fn = "bench";
    } else if (slug == "set_lookup") {
        hot_fn = "set_lookup";
    } else if (slug == "strings") {
        hot_fn = "swork";
    } else if (slug == "string_eq" || slug == "string_equal_same_ref" || slug == "string_equal_diff_object" ||
               slug == "string_equal_diff_len" || slug == "string_equal_same_len_diff" || slug == "string_equal_long" ||
               slug == "string_equal_short" || slug == "string_equal_mixed") {
        hot_fn = "seq";
    } else if (slug == "string_search") {
        hot_fn = "sfind";
    } else if (slug == "string_methods") {
        hot_fn = "sm";
    } else if (slug == "string_heavy_loop") {
        hot_fn = "heavy";
    } else if (slug == "string_index") {
        hot_fn = "str_index";
    } else if (slug == "string_utf8") {
        hot_fn = "utf8_work";
    } else if (slug == "mixed_string_collection") {
        hot_fn = "mix";
    } else if (slug == "branching") {
        hot_fn = "branch";
    } else if (slug == "kernel_large") {
        hot_fn = "kernel";
    } else if (slug == "list_iterate") {
        hot_fn = "sum_list";
    } else if (slug == "range_iterate") {
        hot_fn = "sum_range";
    } else if (slug == "list_write") {
        hot_fn = "write_loop";
    } else if (slug == "tuple_index") {
        hot_fn = "tuple_loop";
    } else if (slug == "bytes_index") {
        hot_fn = "bytes_loop";
    } else if (slug == "nested_collection") {
        hot_fn = "nested";
    } else if (slug == "collection_closure") {
        hot_fn = "bench";
    } else if (slug == "closure_call") {
        hot_fn = "bench";
    } else if (slug == "inline_tiny" || slug == "inline_medium" || slug == "inline_chain2" ||
               slug == "inline_chain3" || slug == "inline_large" || slug == "inline_cold") {
        hot_fn = "bench";
    } else if (slug == "closure_mutable" || slug == "closure_nested" || slug == "closure_create" ||
               slug == "closure_poly") {
        hot_fn = "bench";
    } else if (slug == "closure_shared") {
        hot_fn = "shared";
    } else if (slug == "closure_recursive") {
        hot_fn = "sum";
    }
    const std::uint32_t hot_idx = function_index_by_name(bc, hot_fn);
    ProfileDocument profile =
        hot_idx != 0xFFFFFFFFu ? hot_profile_for(bc, hot_fn) : hot_profile_for(bc, bc.functions[0].name);
    if (std::getenv("PSCRIPT_BENCH_PROGRESS") != nullptr) {
        std::cerr << "[bench] jit.exec." << slug << ": vm reference\n";
    }
    const std::string expected_output = run_program(bc, false, nullptr, nullptr, false).output;

    pscript::jit::JitTieringConfig baseline_cfg;
    baseline_cfg.baseline_invocation_threshold = 1;
    baseline_cfg.optimizing_invocation_threshold = 1'000'000;
    baseline_cfg.require_profile_for_optimizing = true;

    pscript::jit::JitTieringConfig opt_cfg;
    opt_cfg.baseline_invocation_threshold = 1;
    opt_cfg.optimizing_invocation_threshold = 1;
    opt_cfg.require_profile_for_optimizing = false;

    const auto record_exec = [&](std::string_view tier, bool jit, const ProfileDocument* prof,
                                 const pscript::jit::JitTieringConfig* cfg, std::string_view phase) {
        const RunResult run = run_program(bc, jit, prof, cfg, false);
        if (run.output != expected_output) {
            if (options.continue_on_checksum_mismatch) {
                std::cerr << "benchmark checksum mismatch (continuing): benchmark checksum mismatch for jit.exec."
                          << slug << " (" << tier << ")\n";
                return;
            }
            throw std::runtime_error("benchmark checksum mismatch for jit.exec." + std::string(slug) + " (" +
                                     std::string(tier) + ")");
        }
        auto rec = make_record("jit.exec." + std::string(slug), "jit", std::string(tier), std::string(phase),
                               pscript::bench::summarize_samples({run.elapsed_ns}));
        // phase "steady" == legacy single-run per tier (see bench_harness print_human_report)
        if (jit && phase == "cold") {
            rec.compile_median_ns = static_cast<std::int64_t>(run.elapsed_ns);
        }
        records.push_back(std::move(rec));
    };

    if (std::getenv("PSCRIPT_BENCH_PROGRESS") != nullptr) {
        std::cerr << "[bench] jit.exec." << slug << ": steady vm\n";
    }
    record_exec("vm", false, nullptr, nullptr, "steady");
    if (std::getenv("PSCRIPT_BENCH_PROGRESS") != nullptr) {
        std::cerr << "[bench] jit.exec." << slug << ": steady baseline\n";
    }
    record_exec("baseline_jit", true, nullptr, &baseline_cfg, "steady");
    if (std::getenv("PSCRIPT_BENCH_PROGRESS") != nullptr) {
        std::cerr << "[bench] jit.exec." << slug << ": steady optimizing\n";
    }
    record_exec("optimizing_jit", true, &profile, &opt_cfg, "steady");

    if (slug_has_warm_cold_split(slug)) {
        const auto record_warm = [&](std::string_view tier, bool jit, const ProfileDocument* prof,
                                     const pscript::jit::JitTieringConfig* cfg) {
            const WarmBenchResult warm = run_program_warm(bc, jit, prof, cfg, options);
            if (warm.output != expected_output) {
                if (options.continue_on_checksum_mismatch) {
                    std::cerr << "benchmark checksum mismatch (continuing): jit.exec." << slug << " (" << tier
                              << ", warm)\n";
                    return;
                }
                throw std::runtime_error("benchmark checksum mismatch for jit.exec." + std::string(slug) + " (" +
                                         std::string(tier) + ", warm)");
            }
            auto cold_rec = make_record("jit.exec." + std::string(slug), "jit", std::string(tier), "cold",
                                        pscript::bench::summarize_samples({warm.cold_ns}));
            if (jit) {
                cold_rec.compile_median_ns = static_cast<std::int64_t>(warm.cold_ns);
            }
            records.push_back(std::move(cold_rec));
            auto warm_rec = make_record("jit.exec." + std::string(slug), "jit", std::string(tier), "warm_steady",
                                        warm.steady);
            if (jit) {
                warm_rec.compile_median_ns = static_cast<std::int64_t>(warm.compile_attempts_cold);
            }
            records.push_back(std::move(warm_rec));
        };
        record_warm("vm", false, nullptr, nullptr);
        record_warm("baseline_jit", true, &profile, &baseline_cfg);
        record_warm("optimizing_jit", true, &profile, &opt_cfg);
    }
}

void add_jit_compile_suite(const pscript::bench::BenchOptions& options,
                           std::vector<pscript::bench::BenchRecord>& records) {
    if (!options.filter_prefix.empty() && options.filter_prefix.rfind("jit.compile", 0) != 0) {
        return;
    }
    if (!include_category(options, "jit.")) {
        return;
    }
    if (!pscript::jit::jit_backend_available()) {
        return;
    }

    const bool need_kernel_bc =
        include_record_name(options, "jit.compile.baseline.kernel") ||
        include_record_name(options, "jit.compile.optimizing.kernel");
    bool need_exec = include_record_name(options, "jit.exec.kernel");
    if (need_exec && !options.filter_prefix.empty()) {
        if (options.filter_prefix == "jit.exec.kernel") {
            need_exec = true;
        } else if (options.filter_prefix.rfind("jit.exec.", 0) == 0) {
            need_exec = false;
        } else {
            need_exec = options.filter_prefix.rfind("jit.", 0) == 0;
        }
    }

    std::optional<Bytecode> bc;
    std::optional<Bytecode> amortized_bc;
    std::optional<std::uint32_t> idx;
    std::optional<std::string> fingerprint;
    std::optional<ProfileDocument> profile;
    std::optional<pscript::jit::JitOptProfile> opt;

    if (need_kernel_bc) {
        bc = compile_with_builtins(pscript::bench::kJitHotKernel);
        idx = function_index_by_name(*bc, "kernel");
        if (*idx == 0xFFFFFFFFu) {
            return;
        }
        fingerprint = pscript::compute_program_fingerprint(*bc);
        profile = hot_profile_for(*bc, "kernel");
        opt = pscript::jit::build_jit_opt_profile(&(*profile), *bc);
    }
    if (need_exec) {
        amortized_bc = compile_with_builtins(pscript::bench::kJitAmortizedKernel);
        if (!profile) {
            profile = hot_profile_for(*amortized_bc, "kernel");
        }
    }

    if (need_kernel_bc && include_record_name(options, "jit.compile.baseline.kernel")) {
        const auto stats = pscript::bench::time_repeated(options, [&] {
            std::string error;
            (void)pscript::jit::compile_function_baseline(*bc, *idx, *fingerprint, nullptr, error);
        });
        records.push_back(
            make_record("jit.compile.baseline.kernel", "jit", "baseline_jit", "compile_only", stats));
    }
    if (need_kernel_bc && include_record_name(options, "jit.compile.optimizing.kernel")) {
        const auto stats = pscript::bench::time_repeated(options, [&] {
            std::string error;
            (void)pscript::jit::compile_function_optimizing(*bc, *idx, *fingerprint, &(*opt), error);
        });
        records.push_back(
            make_record("jit.compile.optimizing.kernel", "jit", "optimizing_jit", "compile_only", stats));
    }

    if (!need_exec || !amortized_bc) {
        return;
    }

    const auto run_exec_tier = [&](std::string_view tier, bool jit, const ProfileDocument* prof,
                                   const pscript::jit::JitTieringConfig* cfg) {
        const RunResult cold = run_program(*amortized_bc, jit, prof, cfg, false);
        auto cold_record = make_record("jit.exec.kernel", "jit", std::string(tier), "cold",
                                       pscript::bench::summarize_samples({cold.elapsed_ns}));
        if (jit) {
            cold_record.compile_median_ns = static_cast<std::int64_t>(cold.elapsed_ns);
        }
        records.push_back(std::move(cold_record));
        const auto stats = pscript::bench::time_repeated(options, [&] {
            (void)run_program(*amortized_bc, jit, prof, cfg, false);
        });
        auto steady = make_record("jit.exec.kernel", "jit", std::string(tier), "amortized", stats);
        if (tier == "optimizing_jit") {
            const double kernel_ops = 5.0 * 4'000.0;
            steady.throughput_ops_per_sec = kernel_ops / (static_cast<double>(stats.median_ns) / 1e9);
        }
        records.push_back(std::move(steady));
    };

    run_exec_tier("vm", false, nullptr, nullptr);

    pscript::jit::JitTieringConfig baseline_cfg;
    baseline_cfg.baseline_invocation_threshold = 1;
    baseline_cfg.optimizing_invocation_threshold = 1'000'000;
    baseline_cfg.require_profile_for_optimizing = true;
    run_exec_tier("baseline_jit", true, nullptr, &baseline_cfg);

    pscript::jit::JitTieringConfig opt_cfg;
    opt_cfg.baseline_invocation_threshold = 1;
    opt_cfg.optimizing_invocation_threshold = 1;
    opt_cfg.require_profile_for_optimizing = false;
    // optimizing tier enabled below after baseline validation
    run_exec_tier("optimizing_jit", true, &(*profile), &opt_cfg);
}

void add_stdlib_suite(const pscript::bench::BenchOptions& options, std::vector<pscript::bench::BenchRecord>& records) {
    if (!include_category(options, "stdlib.")) {
        return;
    }
    const struct {
        const char* slug;
        const char* source;
    } items[] = {
        {"math", pscript::bench::kStdlibMath},
        {"json", pscript::bench::kStdlibJson},
        {"json_encode", pscript::bench::kStdlibJsonEncode},
        {"json_roundtrip", pscript::bench::kStdlibJsonRoundtrip},
        {"json_nested", pscript::bench::kStdlibJsonNested},
        {"json_pipeline", pscript::bench::kStdlibJsonPipeline},
        {"csv", pscript::bench::kStdlibCsv},
        {"url", pscript::bench::kStdlibUrl},
        {"uuid", pscript::bench::kStdlibUuid},
        {"xml", pscript::bench::kStdlibXml},
        {"crypto", pscript::bench::kStdlibCrypto},
        {"archive_zip_small", pscript::bench::kStdlibArchiveZipSmall},
    };
    for (const auto& item : items) {
        const Bytecode bc = compile_with_builtins(item.source);
        const auto stats = pscript::bench::time_repeated(options, [&] {
            (void)run_program(bc, false, nullptr, nullptr, false);
        });
        records.push_back(make_record("stdlib." + std::string(item.slug), "stdlib", "vm", "steady", stats));
    }
}

void add_real_world_suite(const pscript::bench::BenchOptions& options,
                          std::vector<pscript::bench::BenchRecord>& records) {
    if (!include_category(options, "realworld.")) {
        return;
    }
    const struct {
        const char* slug;
        const char* source;
    } items[] = {
        {"json_file_pipeline", pscript::bench::kRealJsonPipeline},
        {"mixed_runtime", pscript::bench::kRealMixedRuntime},
        {"csv_aggregate", pscript::bench::kRealCsvAggregate},
        {"sqlite_memory", pscript::bench::kRealSqlite},
    };
    for (const auto& item : items) {
        const Bytecode bc = compile_with_builtins(item.source);
        const auto stats = pscript::bench::time_repeated(options, [&] {
            (void)run_program(bc, false, nullptr, nullptr, false);
        });
        records.push_back(make_record("realworld." + std::string(item.slug), "realworld", "vm", "steady", stats));
    }
}

bool matches_filter(const pscript::bench::BenchOptions& options, const pscript::bench::BenchRecord& record) {
    return pscript::bench::bench_record_matches_filter(record.name, options.filter_prefix);
}

void add_representation_records(const pscript::bench::BenchOptions& options,
                                std::vector<pscript::bench::BenchRecord>& records) {
    if (!options.filter_prefix.empty() &&
        options.filter_prefix.rfind("representation", 0) != 0 &&
        !pscript::bench::bench_record_matches_filter("representation.sizeof_value", options.filter_prefix)) {
        return;
    }
    auto push_size = [&](const char* name, std::int64_t bytes) {
        pscript::bench::BenchRecord record;
        record.name = name;
        record.category = "representation";
        record.tier = "metadata";
        record.phase = "size";
        record.stats.median_ns = bytes;
        record.stats.min_ns = bytes;
        record.stats.max_ns = bytes;
        record.stats.count = 1;
        records.push_back(std::move(record));
    };
    push_size("representation.sizeof_value", static_cast<std::int64_t>(sizeof(pscript::Value)));
    push_size("representation.sizeof_cell", static_cast<std::int64_t>(sizeof(pscript::Cell)));
    push_size("representation.sizeof_optional_value",
              static_cast<std::int64_t>(sizeof(std::optional<pscript::Value>)));
    push_size("representation.sizeof_value_kind", static_cast<std::int64_t>(sizeof(pscript::ValueKind)));
}

void bench_progress(const char* step) {
    if (std::getenv("PSCRIPT_BENCH_PROGRESS") != nullptr) {
        std::cerr << "[bench] " << step << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::ios::sync_with_stdio(true);
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    try {
    const pscript::bench::BenchOptions options = pscript::bench::parse_args(argc, argv);
    pscript::bench::BenchRunEnvironment run_environment;
    run_environment.cpu_start = pscript::bench::capture_host_cpu_snapshot();
    std::vector<pscript::bench::BenchRecord> records;

    bench_progress("compiler");
    add_compiler_suite(options, records);
    bench_progress("representation");
    add_representation_records(options, records);

    bench_progress("runtime suites begin");
    add_runtime_suite(options, records, "arithmetic", pscript::bench::kRuntimeArithmetic);
    add_runtime_suite(options, records, "integer_ops", pscript::bench::kRuntimeIntOps);
    add_runtime_suite(options, records, "float_ops", pscript::bench::kRuntimeFloatOps);
    add_runtime_suite(options, records, "num_lu_int_cmp", pscript::bench::kRuntimeNumLuIntCmp);
    add_runtime_suite(options, records, "num_lu_divmod", pscript::bench::kRuntimeNumLuDivMod);
    add_runtime_suite(options, records, "num_lu_mixed", pscript::bench::kRuntimeNumLuMixed);
    add_runtime_suite(options, records, "num_lu_nested", pscript::bench::kRuntimeNumLuNested);
    add_runtime_suite(options, records, "num_lu_short", pscript::bench::kRuntimeNumLuShort);
    add_runtime_suite(options, records, "num_lu_convert", pscript::bench::kRuntimeNumLuConvert);
    add_runtime_suite(options, records, "perf_lu_local_rw", pscript::bench::kRuntimePerfLuLocalRw);
    add_runtime_suite(options, records, "loops", pscript::bench::kRuntimeLoop);
    add_runtime_suite(options, records, "calls", pscript::bench::kRuntimeCalls);
    add_runtime_suite(options, records, "simple_calls", pscript::bench::kRuntimeSimpleCalls);
    add_runtime_suite(options, records, "recursive_sum", pscript::bench::kRuntimeRecursiveSum);
    add_runtime_suite(options, records, "nested_calls", pscript::bench::kRuntimeNestedCalls);
    add_runtime_suite(options, records, "callvalue_mono", pscript::bench::kRuntimeCallValueMono);
    add_runtime_suite(options, records, "callvalue_poly", pscript::bench::kRuntimeCallValuePoly);
    add_runtime_suite(options, records, "branching", pscript::bench::kRuntimeBranch);
    add_runtime_suite(options, records, "collections", pscript::bench::kRuntimeCollections);
    add_runtime_suite(options, records, "collection_allocation", pscript::bench::kRuntimeCollectionAllocation);
    add_runtime_suite(options, records, "field_access", pscript::bench::kRuntimeFieldAccess);
    add_runtime_suite(options, records, "list_index", pscript::bench::kRuntimeListIndex);
    add_runtime_suite(options, records, "dict_lookup", pscript::bench::kRuntimeDictLookup);
    add_runtime_suite(options, records, "dict_lu_str_hit", pscript::bench::kRuntimeDictLuStrHit);
    add_runtime_suite(options, records, "dict_lu_str_miss", pscript::bench::kRuntimeDictLuStrMiss);
    add_runtime_suite(options, records, "dict_lu_int_hit", pscript::bench::kRuntimeDictLuIntHit);
    add_runtime_suite(options, records, "dict_lu_int_miss", pscript::bench::kRuntimeDictLuIntMiss);
    add_runtime_suite(options, records, "dict_lu_same_key", pscript::bench::kRuntimeDictLuSameKey);
    add_runtime_suite(options, records, "dict_lu_scatter", pscript::bench::kRuntimeDictLuScatter);
    add_runtime_suite(options, records, "dict_lu_large", pscript::bench::kRuntimeDictLuLarge);
    add_runtime_suite(options, records, "dict_lu_collision", pscript::bench::kRuntimeDictLuCollision);
    add_runtime_suite(options, records, "dict_lu_insert_mix", pscript::bench::kRuntimeDictLuInsertMix);
    add_runtime_suite(options, records, "dict_lu_mixed", pscript::bench::kRuntimeDictLuMixed);
    add_runtime_suite(options, records, "set_lookup", pscript::bench::kRuntimeSetLookup);
    add_runtime_suite(options, records, "list_iterate", pscript::bench::kRuntimeListIterate);
    add_runtime_suite(options, records, "range_iterate", pscript::bench::kRuntimeRangeIterate);
    add_runtime_suite(options, records, "list_write", pscript::bench::kRuntimeListWrite);
    add_runtime_suite(options, records, "tuple_index", pscript::bench::kRuntimeTupleIndex);
    add_runtime_suite(options, records, "bytes_index", pscript::bench::kRuntimeBytesIndex);
    add_runtime_suite(options, records, "nested_collection", pscript::bench::kRuntimeNestedCollection);
    add_runtime_suite(options, records, "collection_closure", pscript::bench::kRuntimeCollectionClosure);
    add_runtime_suite(options, records, "closure_call", pscript::bench::kRuntimeClosureCall);
    add_runtime_suite(options, records, "closure_mutable", pscript::bench::kRuntimeClosureMutable);
    add_runtime_suite(options, records, "closure_nested", pscript::bench::kRuntimeClosureNested);
    add_runtime_suite(options, records, "closure_create", pscript::bench::kRuntimeClosureCreate);
    add_runtime_suite(options, records, "closure_poly", pscript::bench::kRuntimeClosurePoly);
    add_runtime_suite(options, records, "closure_shared", pscript::bench::kRuntimeClosureShared);
    add_runtime_suite(options, records, "closure_recursive", pscript::bench::kRuntimeClosureRecursive);
    add_runtime_suite(options, records, "strings", pscript::bench::kRuntimeString);
    add_runtime_suite(options, records, "string_eq", pscript::bench::kRuntimeStringEq);
    add_runtime_suite(options, records, "string_equal_same_ref", pscript::bench::kRuntimeStringEqSameRef);
    add_runtime_suite(options, records, "string_equal_diff_object", pscript::bench::kRuntimeStringEqDiffObject);
    add_runtime_suite(options, records, "string_equal_diff_len", pscript::bench::kRuntimeStringEqDiffLen);
    add_runtime_suite(options, records, "string_equal_same_len_diff", pscript::bench::kRuntimeStringEqSameLenDiff);
    add_runtime_suite(options, records, "string_equal_long", pscript::bench::kRuntimeStringEqLong);
    add_runtime_suite(options, records, "string_equal_short", pscript::bench::kRuntimeStringEqShort);
    add_runtime_suite(options, records, "string_equal_mixed", pscript::bench::kRuntimeStringEqMixed);
    add_runtime_suite(options, records, "string_search", pscript::bench::kRuntimeStringSearch);
    add_runtime_suite(options, records, "string_methods", pscript::bench::kRuntimeStringMethods);
    add_runtime_suite(options, records, "string_heavy_loop", pscript::bench::kRuntimeStringHeavyLoop);
    add_runtime_suite(options, records, "string_index", pscript::bench::kRuntimeStringIndex);
    add_runtime_suite(options, records, "string_utf8", pscript::bench::kRuntimeStringUtf8);
    add_runtime_suite(options, records, "mixed_string_collection", pscript::bench::kRuntimeMixedStringCollection);
    if (include_category(options, "runtime.exceptions")) {
        const Bytecode bc = compile_with_builtins(pscript::bench::kRuntimeException);
        const auto stats = pscript::bench::time_repeated(options, [&] {
            (void)run_program(bc, false, nullptr, nullptr, false);
        });
        records.push_back(make_record("runtime.exceptions", "runtime", "vm", "steady", stats));
    }
    if (include_category(options, "runtime.defer")) {
        const Bytecode bc = compile_with_builtins(pscript::bench::kRuntimeDefer);
        const auto stats = pscript::bench::time_repeated(options, [&] {
            (void)run_program(bc, false, nullptr, nullptr, false);
        });
        records.push_back(make_record("runtime.defer", "runtime", "vm", "steady", stats));
    }
    if (include_category(options, "runtime.async_tasks")) {
        const Bytecode bc = compile_with_builtins(pscript::bench::kRuntimeAsync);
        const auto stats = pscript::bench::time_repeated(options, [&] {
            (void)run_program(bc, false, nullptr, nullptr, false);
        });
        records.push_back(make_record("runtime.async_tasks", "runtime", "vm", "steady", stats));
    }

    bench_progress("runtime suites end");
    bench_progress("jit.compile");
    add_jit_compile_suite(options, records);
    bench_progress("jit.exec suites begin");
    add_jit_exec_runtime_suite(options, records, "arithmetic", pscript::bench::kRuntimeArithmetic);
    add_jit_exec_runtime_suite(options, records, "integer_ops", pscript::bench::kRuntimeIntOps);
    add_jit_exec_runtime_suite(options, records, "float_ops", pscript::bench::kRuntimeFloatOps);
    add_jit_exec_runtime_suite(options, records, "num_lu_int_cmp", pscript::bench::kRuntimeNumLuIntCmp);
    add_jit_exec_runtime_suite(options, records, "num_lu_divmod", pscript::bench::kRuntimeNumLuDivMod);
    add_jit_exec_runtime_suite(options, records, "num_lu_mixed", pscript::bench::kRuntimeNumLuMixed);
    add_jit_exec_runtime_suite(options, records, "num_lu_nested", pscript::bench::kRuntimeNumLuNested);
    add_jit_exec_runtime_suite(options, records, "num_lu_short", pscript::bench::kRuntimeNumLuShort);
    add_jit_exec_runtime_suite(options, records, "num_lu_convert", pscript::bench::kRuntimeNumLuConvert);
    add_jit_exec_runtime_suite(options, records, "perf_lu_local_rw", pscript::bench::kRuntimePerfLuLocalRw);
    add_jit_exec_runtime_suite(options, records, "loops", pscript::bench::kRuntimeLoop);
    add_jit_exec_runtime_suite(options, records, "calls", pscript::bench::kRuntimeCalls);
    add_jit_exec_runtime_suite(options, records, "simple_calls", pscript::bench::kRuntimeSimpleCalls);
    add_jit_exec_runtime_suite(options, records, "recursive_sum", pscript::bench::kRuntimeRecursiveSum);
    add_jit_exec_runtime_suite(options, records, "nested_calls", pscript::bench::kRuntimeNestedCalls);
    add_jit_exec_runtime_suite(options, records, "inline_tiny", pscript::bench::kRuntimeInlineTiny);
    add_jit_exec_runtime_suite(options, records, "inline_medium", pscript::bench::kRuntimeInlineMedium);
    add_jit_exec_runtime_suite(options, records, "inline_chain2", pscript::bench::kRuntimeInlineChain2);
    add_jit_exec_runtime_suite(options, records, "inline_chain3", pscript::bench::kRuntimeInlineChain3);
    add_jit_exec_runtime_suite(options, records, "inline_large", pscript::bench::kRuntimeInlineLargeCallee);
    add_jit_exec_runtime_suite(options, records, "inline_cold", pscript::bench::kRuntimeInlineColdCall);
    add_jit_exec_runtime_suite(options, records, "callvalue_mono", pscript::bench::kRuntimeCallValueMono);
    add_jit_exec_runtime_suite(options, records, "callvalue_poly", pscript::bench::kRuntimeCallValuePoly);
    add_jit_exec_runtime_suite(options, records, "closure_call", pscript::bench::kRuntimeClosureCall);
    add_jit_exec_runtime_suite(options, records, "closure_mutable", pscript::bench::kRuntimeClosureMutable);
    add_jit_exec_runtime_suite(options, records, "closure_nested", pscript::bench::kRuntimeClosureNested);
    add_jit_exec_runtime_suite(options, records, "closure_create", pscript::bench::kRuntimeClosureCreate);
    add_jit_exec_runtime_suite(options, records, "closure_poly", pscript::bench::kRuntimeClosurePoly);
    add_jit_exec_runtime_suite(options, records, "closure_shared", pscript::bench::kRuntimeClosureShared);
    add_jit_exec_runtime_suite(options, records, "closure_recursive", pscript::bench::kRuntimeClosureRecursive);
    add_jit_exec_runtime_suite(options, records, "collections", pscript::bench::kRuntimeCollections);
    add_jit_exec_runtime_suite(options, records, "collection_allocation",
                               pscript::bench::kRuntimeCollectionAllocation);
    add_jit_exec_runtime_suite(options, records, "strings", pscript::bench::kRuntimeString);
    add_jit_exec_runtime_suite(options, records, "string_eq", pscript::bench::kRuntimeStringEq);
    add_jit_exec_runtime_suite(options, records, "string_equal_same_ref", pscript::bench::kRuntimeStringEqSameRef);
    add_jit_exec_runtime_suite(options, records, "string_equal_diff_object", pscript::bench::kRuntimeStringEqDiffObject);
    add_jit_exec_runtime_suite(options, records, "string_equal_diff_len", pscript::bench::kRuntimeStringEqDiffLen);
    add_jit_exec_runtime_suite(options, records, "string_equal_same_len_diff", pscript::bench::kRuntimeStringEqSameLenDiff);
    add_jit_exec_runtime_suite(options, records, "string_equal_long", pscript::bench::kRuntimeStringEqLong);
    add_jit_exec_runtime_suite(options, records, "string_equal_short", pscript::bench::kRuntimeStringEqShort);
    add_jit_exec_runtime_suite(options, records, "string_equal_mixed", pscript::bench::kRuntimeStringEqMixed);
    add_jit_exec_runtime_suite(options, records, "string_search", pscript::bench::kRuntimeStringSearch);
    add_jit_exec_runtime_suite(options, records, "string_methods", pscript::bench::kRuntimeStringMethods);
    add_jit_exec_runtime_suite(options, records, "string_heavy_loop", pscript::bench::kRuntimeStringHeavyLoop);
    add_jit_exec_runtime_suite(options, records, "string_index", pscript::bench::kRuntimeStringIndex);
    add_jit_exec_runtime_suite(options, records, "string_utf8", pscript::bench::kRuntimeStringUtf8);
    add_jit_exec_runtime_suite(options, records, "mixed_string_collection", pscript::bench::kRuntimeMixedStringCollection);
    add_jit_exec_runtime_suite(options, records, "list_iterate", pscript::bench::kRuntimeListIterate);
    add_jit_exec_runtime_suite(options, records, "range_iterate", pscript::bench::kRuntimeRangeIterate);
    add_jit_exec_runtime_suite(options, records, "list_index", pscript::bench::kRuntimeListIndex);
    add_jit_exec_runtime_suite(options, records, "dict_lookup", pscript::bench::kRuntimeDictLookup);
    add_jit_exec_runtime_suite(options, records, "dict_lu_str_hit", pscript::bench::kRuntimeDictLuStrHit);
    add_jit_exec_runtime_suite(options, records, "dict_lu_str_miss", pscript::bench::kRuntimeDictLuStrMiss);
    add_jit_exec_runtime_suite(options, records, "dict_lu_int_hit", pscript::bench::kRuntimeDictLuIntHit);
    add_jit_exec_runtime_suite(options, records, "dict_lu_int_miss", pscript::bench::kRuntimeDictLuIntMiss);
    add_jit_exec_runtime_suite(options, records, "dict_lu_same_key", pscript::bench::kRuntimeDictLuSameKey);
    add_jit_exec_runtime_suite(options, records, "dict_lu_scatter", pscript::bench::kRuntimeDictLuScatter);
    add_jit_exec_runtime_suite(options, records, "dict_lu_large", pscript::bench::kRuntimeDictLuLarge);
    add_jit_exec_runtime_suite(options, records, "dict_lu_collision", pscript::bench::kRuntimeDictLuCollision);
    add_jit_exec_runtime_suite(options, records, "dict_lu_insert_mix", pscript::bench::kRuntimeDictLuInsertMix);
    add_jit_exec_runtime_suite(options, records, "dict_lu_mixed", pscript::bench::kRuntimeDictLuMixed);
    add_jit_exec_runtime_suite(options, records, "set_lookup", pscript::bench::kRuntimeSetLookup);
    add_jit_exec_runtime_suite(options, records, "list_write", pscript::bench::kRuntimeListWrite);
    add_jit_exec_runtime_suite(options, records, "tuple_index", pscript::bench::kRuntimeTupleIndex);
    add_jit_exec_runtime_suite(options, records, "bytes_index", pscript::bench::kRuntimeBytesIndex);
    add_jit_exec_runtime_suite(options, records, "nested_collection", pscript::bench::kRuntimeNestedCollection);
    add_jit_exec_runtime_suite(options, records, "collection_closure", pscript::bench::kRuntimeCollectionClosure);
    bench_progress("jit.exec.kernel_large");
    add_jit_exec_runtime_suite(options, records, "kernel_large", pscript::bench::kJitHotKernel);
    bench_progress("stdlib");
    add_stdlib_suite(options, records);
    bench_progress("realworld");
    add_real_world_suite(options, records);

    std::vector<pscript::bench::BenchRecord> filtered;
    filtered.reserve(records.size());
    for (const pscript::bench::BenchRecord& record : records) {
        if (matches_filter(options, record)) {
            filtered.push_back(record);
        }
    }

    run_environment.cpu_end = pscript::bench::capture_host_cpu_snapshot();
    pscript::bench::print_human_report(options, filtered);
    if (!options.json_path.empty()) {
        std::ofstream out(options.json_path, std::ios::binary);
        if (!out) {
            std::cerr << "Failed to write JSON report: " << options.json_path << '\n';
            return 1;
        }
        pscript::bench::write_json_report(options, filtered, out, pscript::kPscriptVersion, &run_environment);
        std::cout << "Wrote JSON baseline: " << options.json_path << '\n';
    }

    if (options.jit_ic_dump) {
        const auto dump_if = [&](const char* label, const char* source, const char* hot_fn) {
            if (!options.filter_prefix.empty() && options.filter_prefix.find(label) == std::string::npos) {
                return;
            }
            const std::string ic_line = probe_jit_hot_ic_metrics(options, source, hot_fn);
            std::cout << "jit_ic_metrics " << label << " optimizing_jit: " << ic_line << '\n';
        };
        dump_if("closure_call", pscript::bench::kRuntimeClosureCall, "bench");
        dump_if("simple_calls", pscript::bench::kRuntimeSimpleCalls, "hot");
        dump_if("callvalue_mono", pscript::bench::kRuntimeCallValueMono, "hot");
        dump_if("inline_large", pscript::bench::kRuntimeInlineLargeCallee, "bench");
        dump_if("inline_medium", pscript::bench::kRuntimeInlineMedium, "bench");
        dump_if("loops", pscript::bench::kRuntimeLoop, "loop");
        dump_if("simple_calls", pscript::bench::kRuntimeSimpleCalls, "hot");
    }

    std::cout << "Benchmark complete (" << filtered.size() << " records, pscript " << pscript::kPscriptVersion
              << ")\n";
    bench_progress("done");
    return 0;
    } catch (const std::exception& ex) {
        std::cerr << "benchmark fatal: " << ex.what() << '\n';
        return 1;
    }
}
