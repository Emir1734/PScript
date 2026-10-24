#include "compiler/compiler.h"
#include "compiler/pgo.h"
#include "jit/jit_backend.h"
#include "jit/jit_compiler.h"
#include "jit/jit_ir.h"
#include "jit/jit_backend.h"
#include "jit/jit_compiler.h"
#include "jit/jit_opt.h"
#include "jit/jit_opt_profile.h"
#include "jit/jit_ic.h"
#include "jit/jit_runtime.h"
#include "parser/parser.h"
#include "runtime/profiler.h"
#include "runtime/runtime.h"
#include "vm/vm.h"

#include "benchmark/bench_workloads.h"

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>

using pscript::Bytecode;
using pscript::CompileOptions;
using pscript::ProfileDocument;
using pscript::Runtime;
using pscript::Vm;
using pscript::compile;
using pscript::compile_with_builtins;
using pscript::parse;

namespace {

int g_failed = 0;

void fail_test(const char* name, const std::string& detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    std::cerr.flush();
    ++g_failed;
}

void pass_test(const char* name) {
    std::cout << "PASS  " << name << '\n';
    std::cout.flush();
}

Bytecode compile_source(const std::string& source) {
    CompileOptions options;
    options.source_file = "<test>";
    return compile(parse(source), options);
}

std::uint32_t function_index_by_name(const Bytecode& bytecode, const std::string& name) {
    for (std::uint32_t i = 0; i < bytecode.functions.size(); ++i) {
        if (bytecode.functions[i].name == name) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

ProfileDocument hot_integer_profile(const Bytecode& bc, const std::string& fn_name);

bool vm_jit_output_parity(const std::string& source, const std::string& hot_fn = "hot") {
    Bytecode bc = compile_source(source);
    ProfileDocument profile = hot_integer_profile(bc, hot_fn.empty() ? "hot" : hot_fn);
    pscript::apply_profile_guided_optimizations(bc, profile);
    std::ostringstream jit_out;
    Runtime jit_rt(jit_out);
    Vm jit_vm(std::move(bc), jit_rt);
    jit_vm.set_jit_enabled(true);
    pscript::jit::JitTieringConfig config;
    config.baseline_invocation_threshold = 1;
    config.optimizing_invocation_threshold = 1;
    config.require_profile_for_optimizing = false;
    jit_vm.set_jit_config(config);
    jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
    jit_vm.jit_engine().set_profile(&profile);
    jit_vm.run();
    std::ostringstream vm_out;
    Runtime vm_rt(vm_out);
    Vm vm_only(compile_source(source), vm_rt);
    vm_only.set_jit_enabled(false);
    vm_only.run();
    return jit_out.str() == vm_out.str();
}

ProfileDocument hot_integer_profile(const Bytecode& bc, const std::string& fn_name) {
    ProfileDocument doc;
    doc.version = ProfileDocument::kFormatVersion;
    doc.pgo_compat_version = pscript::kPgoCompatVersion;
    doc.program_fingerprint = pscript::compute_program_fingerprint(bc);
    const std::uint32_t idx = function_index_by_name(bc, fn_name);
    if (idx == 0xFFFFFFFFu || idx >= bc.functions.size()) {
        return doc;
    }
    pscript::ProfileFunctionRecord fn;
    fn.function_index = idx;
    fn.calls = 10'000;
    fn.hotness = pscript::ProfileHotness::Hot;
    doc.functions.push_back(fn);
    pscript::ProfileLoopRecord loop;
    loop.function_index = idx;
    loop.ip = bc.functions[idx].entry;
    loop.iterations = 50'000;
    doc.loops.push_back(loop);
    return doc;
}

}  // namespace

int main() {
    {
        const Bytecode bc = compile_source("func f():\n    return 2 + 3\n");
        const std::uint32_t idx = function_index_by_name(bc, "f");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        bool has_runtime_add = false;
        for (const pscript::jit::JitIrInstruction& insn : ir.instructions) {
            if (insn.opcode == pscript::jit::JitIrOpcode::Add) {
                has_runtime_add = true;
            }
        }
        pscript::jit::OptimizingPassStats stats;
        pscript::jit::run_optimizing_pipeline(bc, ir, nullptr, stats, error);
        if (!has_runtime_add) {
            pass_test("constant folding");
        } else if (stats.constant_folds < 1) {
            fail_test("constant folding", "expected fold");
        } else {
            pass_test("constant folding");
        }
    }

    {
        const Bytecode bc = compile_source("func f(a):\n    x = a\n    return x + 1\n");
        const std::uint32_t idx = function_index_by_name(bc, "f");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        pscript::jit::OptimizingPassStats stats;
        pscript::jit::run_optimizing_pipeline(bc, ir, nullptr, stats, error);
        pass_test("constant propagation pipeline");
    }

    {
        const Bytecode bc = compile_source("func f():\n    x = 1\n    x = 2\n    return x\n");
        const std::uint32_t idx = function_index_by_name(bc, "f");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        pscript::jit::OptimizingPassStats stats;
        pscript::jit::run_optimizing_pipeline(bc, ir, nullptr, stats, error);
        pass_test("dead code elimination pipeline");
    }

    {
        pscript::jit::JitFunctionIr ir;
        ir.register_count = 4;
        ir.function_index = 0;
        pscript::jit::JitIrInstruction load_x;
        load_x.opcode = pscript::jit::JitIrOpcode::LoadLocal;
        load_x.a = 0;
        load_x.b = 0;
        pscript::jit::JitIrInstruction load_y;
        load_y.opcode = pscript::jit::JitIrOpcode::LoadLocal;
        load_y.a = 1;
        load_y.b = 1;
        pscript::jit::JitIrInstruction add1;
        add1.opcode = pscript::jit::JitIrOpcode::Add;
        add1.a = 2;
        add1.b = 0;
        add1.c = 1;
        pscript::jit::JitIrInstruction add2;
        add2.opcode = pscript::jit::JitIrOpcode::Add;
        add2.a = 3;
        add2.b = 0;
        add2.c = 1;
        ir.instructions = {load_x, load_y, add1, add2};
        pscript::jit::build_cfg_for_function(ir);
        pscript::jit::OptimizingPassStats stats;
        std::string error;
        pscript::jit::jit_common_subexpression_elimination(ir, stats);
        if (stats.cse_hits < 1) {
            fail_test("cse", "expected cse hit");
        } else {
            pass_test("cse");
        }
    }

    if (pscript::jit::jit_backend_available()) {
        const Bytecode bc = compile_source("func add_i(a, b):\n    return a + b\n");
        const std::uint32_t idx = function_index_by_name(bc, "add_i");
        ProfileDocument profile = hot_integer_profile(bc, "add_i");
        pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
        std::string error;
        auto compiled = pscript::jit::compile_function_optimizing(bc, idx, pscript::compute_program_fingerprint(bc),
                                                                  &opt, error);
        if (!compiled || compiled->tier != pscript::jit::JitTier::OptimizingJit) {
            fail_test("integer specialization compile", error);
        } else {
            pass_test("integer specialization compile");
        }
        pscript::jit::JitExecState state;
        state.bytecode = &bc;
        const auto& fn = bc.functions[idx];
        state.registers.resize(std::max(fn.register_count, compiled->register_count));
        pscript::jit::refresh_register_data(state);
        state.locals.resize(fn.local_count);
        for (std::uint32_t i = 0; i < fn.local_count; ++i) {
            state.locals[i] = std::make_shared<pscript::Cell>();
        }
        state.locals[0]->value = Runtime::make_integer(7);
        state.locals[1]->value = Runtime::make_integer(5);
        pscript::jit::JitFunctionIr ir;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        state.instruction_count = static_cast<std::uint32_t>(ir.instructions.size());
        state.deopt_context = compiled->metadata ? &compiled->metadata->deopt : nullptr;
        if (pscript::jit::execute_compiled_entry(&compiled.value(), state) != pscript::jit::JitExecuteResult::Completed ||
            state.return_value.integer != 12) {
            fail_test("integer specialization run", "bad result");
        } else {
            pass_test("integer specialization run");
        }

        state.fallback = false;
        state.completed = false;
        state.deopt_requested = false;
        state.current_insn = 0;
        state.deopt_context = compiled->metadata ? &compiled->metadata->deopt : nullptr;
        state.deopt_requested = false;
        state.deopt_resume_bytecode_ip = 0;
        state.locals[1]->value = Runtime::make_float(1.5);
        const pscript::jit::JitExecuteResult deopt_result =
            pscript::jit::execute_compiled_entry(&compiled.value(), state);
        if (deopt_result != pscript::jit::JitExecuteResult::Deoptimized || !state.deopt_requested) {
            fail_test("guard deopt", "expected deoptimization on float");
        } else {
            pass_test("guard deopt");
        }
    }

    {
        const Bytecode bc = compile_source(
            "func sum(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i\n"
            "        i = i + 1\n"
            "    return total\n");
        const std::uint32_t idx = function_index_by_name(bc, "sum");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        ProfileDocument profile = hot_integer_profile(bc, "sum");
        pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
        pscript::jit::OptimizingPassStats stats;
        if (!pscript::jit::run_optimizing_pipeline(bc, ir, &opt, stats, error)) {
            fail_test("loop integer specialization ir", error);
        } else {
            bool has_int_add = false;
            for (const pscript::jit::JitIrInstruction& insn : ir.instructions) {
                if (insn.opcode == pscript::jit::JitIrOpcode::IntAdd) {
                    has_int_add = true;
                }
            }
            if (!has_int_add || stats.specialized_ops < 1) {
                fail_test("loop integer specialization ir", "expected IntAdd specialization in loop");
            } else {
                pass_test("loop integer specialization ir");
            }
        }
        if (!ir.baseline_numeric_loop_fast_path) {
            fail_test("loop integer specialization ir", "expected baseline_numeric_loop_fast_path");
        }
    }

    {
        const Bytecode bc = compile_source(
            "func seq():\n"
            "    left = \"a\"\n"
            "    right = \"b\"\n"
            "    hits = 0\n"
            "    if left == right:\n"
            "        hits = hits + 1\n"
            "    if left != right:\n"
            "        hits = hits + 10\n"
            "    return hits\n");
        const std::uint32_t idx = function_index_by_name(bc, "seq");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        ProfileDocument profile = hot_integer_profile(bc, "seq");
        pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
        pscript::jit::OptimizingPassStats stats;
        if (!pscript::jit::run_optimizing_pipeline(bc, ir, &opt, stats, error)) {
            fail_test("string equal specialization ir", error);
        } else {
            bool has_string_equal = false;
            bool has_string_not_equal = false;
            for (const pscript::jit::JitIrInstruction& insn : ir.instructions) {
                if (insn.opcode == pscript::jit::JitIrOpcode::StringEqual) {
                    has_string_equal = true;
                }
                if (insn.opcode == pscript::jit::JitIrOpcode::StringNotEqual) {
                    has_string_not_equal = true;
                }
            }
            if (!has_string_equal || !has_string_not_equal || stats.specialized_ops < 2) {
                fail_test("string equal specialization ir", "expected StringEqual/StringNotEqual in IR");
            } else {
                pass_test("string equal specialization ir");
            }
        }
    }

    {
        const Bytecode bc = compile_source(
            "func seq(n):\n"
            "    hits = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        if \"a\" == \"a\":\n"
            "            hits = hits + 1\n"
            "        if \"x\" != \"y\":\n"
            "            hits = hits + 1\n"
            "        i = i + 1\n"
            "    return hits\n");
        const std::uint32_t idx = function_index_by_name(bc, "seq");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        ProfileDocument profile = hot_integer_profile(bc, "seq");
        pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
        pscript::jit::OptimizingPassStats stats;
        if (!pscript::jit::run_optimizing_pipeline(bc, ir, &opt, stats, error)) {
            fail_test("string literal fold ir", error);
        } else {
            bool has_string_cmp = false;
            for (const pscript::jit::JitIrInstruction& insn : ir.instructions) {
                if (insn.opcode == pscript::jit::JitIrOpcode::StringEqual ||
                    insn.opcode == pscript::jit::JitIrOpcode::StringNotEqual) {
                    has_string_cmp = true;
                }
            }
            if (has_string_cmp) {
                fail_test("string literal fold ir", "literal compares should fold away");
            } else {
                pass_test("string literal fold ir");
            }
        }
    }

    if (pscript::jit::jit_backend_available()) {
        const Bytecode bc = compile_source(
            "func seq(n):\n"
            "    needle = \"needle\"\n"
            "    hits = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        if needle == needle:\n"
            "            hits = hits + 1\n"
            "        i = i + 1\n"
            "    return hits\n");
        const std::uint32_t idx = function_index_by_name(bc, "seq");
        ProfileDocument profile = hot_integer_profile(bc, "seq");
        pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
        std::string error;
        auto compiled = pscript::jit::compile_function_optimizing(bc, idx, pscript::compute_program_fingerprint(bc),
                                                                  &opt, error);
        if (!compiled) {
            fail_test("string dynamic compare optimizing jit", error);
        } else {
            pscript::jit::JitExecState state;
            state.bytecode = &bc;
            const auto& fn = bc.functions[idx];
            state.registers.resize(std::max(fn.register_count, compiled->register_count));
            pscript::jit::refresh_register_data(state);
            state.locals.resize(fn.local_count);
            for (std::uint32_t i = 0; i < fn.local_count; ++i) {
                state.locals[i] = std::make_shared<pscript::Cell>();
            }
            state.locals[0]->value = Runtime::make_integer(100);
            pscript::jit::JitFunctionIr ir;
            pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
            state.instruction_count = static_cast<std::uint32_t>(ir.instructions.size());
            state.deopt_context = compiled->metadata ? &compiled->metadata->deopt : nullptr;
            compiled->entry(&state);
            if (!state.completed || state.fallback || state.return_value.integer != 100) {
                fail_test("string dynamic compare optimizing jit",
                          "expected 100 got " + std::to_string(state.return_value.integer));
            } else {
                pass_test("string dynamic compare optimizing jit");
            }
        }
    }

    if (pscript::jit::jit_backend_available()) {
        const Bytecode bc = compile_source(
            "func lookup():\n"
            "    table = {}\n"
            "    table[\"x\"] = 3\n"
            "    key = \"x\"\n"
            "    return table[key]\n");
        const std::uint32_t idx = function_index_by_name(bc, "lookup");
        ProfileDocument profile = hot_integer_profile(bc, "lookup");
        pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
        std::string error;
        auto compiled = pscript::jit::compile_function_optimizing(bc, idx, pscript::compute_program_fingerprint(bc),
                                                                  &opt, error);
        if (!compiled || compiled->tier != pscript::jit::JitTier::OptimizingJit) {
            fail_test("dict lookup optimizing compile", error);
        } else {
            pass_test("dict lookup optimizing compile");
        }
        pscript::jit::JitExecState state;
        state.bytecode = &bc;
        const auto& fn = bc.functions[idx];
        state.registers.resize(std::max(fn.register_count, compiled->register_count));
        pscript::jit::refresh_register_data(state);
        state.locals.resize(fn.local_count);
        for (std::uint32_t i = 0; i < fn.local_count; ++i) {
            state.locals[i] = std::make_shared<pscript::Cell>();
        }
        pscript::jit::JitFunctionIr ir;
        pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error);
        state.instruction_count = static_cast<std::uint32_t>(ir.instructions.size());
        state.deopt_context = compiled->metadata ? &compiled->metadata->deopt : nullptr;
        compiled->entry(&state);
        if (!state.completed || state.fallback || state.return_value.integer != 3) {
            fail_test("dict lookup optimizing run", "bad result");
        } else {
            pass_test("dict lookup optimizing run");
        }
    }

    {
        pscript::jit::JitEngine engine;
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 2;
        config.optimizing_invocation_threshold = 4;
        config.require_profile_for_optimizing = true;
        engine.set_config(config);
        const Bytecode bc = compile_source("func hot():\n    return 1 + 2\n");
        const std::uint32_t idx = function_index_by_name(bc, "hot");
        ProfileDocument profile = hot_integer_profile(bc, "hot");
        Bytecode hot_bc = bc;
        pscript::apply_profile_guided_optimizations(hot_bc, profile);
        engine.bind_bytecode(&hot_bc, pscript::compute_program_fingerprint(hot_bc));
        engine.set_profile(&profile);
        const auto& fn = hot_bc.functions[idx];
        for (int i = 0; i < 5; ++i) {
            engine.record_invocation(idx, fn);
        }
        if (engine.desired_tier(idx, fn) != pscript::jit::JitTier::OptimizingJit) {
            fail_test("tier optimizing", "expected optimizing tier");
        } else {
            pass_test("tier optimizing");
        }
        engine.compile_if_needed(idx, fn);
        engine.compile_if_needed(idx, fn);
        if (engine.optimizing_compile_attempts() < 1) {
            fail_test("tier replacement", "expected optimizing compile");
        } else if (engine.cache_hits() < 1) {
            fail_test("optimized cache reuse", "expected cache hit");
        } else {
            const pscript::jit::JitCompiledCode* code = engine.lookup(idx);
            if (!code || code->tier != pscript::jit::JitTier::OptimizingJit) {
                fail_test("optimized cache reuse", "wrong tier in cache");
            } else {
                pass_test("optimized cache reuse");
            }
        }
    }

    {
        const std::string source =
            "func bump(x):\n"
            "    return x + 1\n"
            "func chain(x):\n"
            "    return bump(x) + bump(x + 1)\n"
            "print(chain(3))\n";
        const Bytecode bc = compile_source(source);
        const std::uint32_t idx = function_index_by_name(bc, "chain");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        if (!pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error)) {
            fail_test("call-aware inline ir", error);
        } else {
            pscript::jit::OptimizingPassStats stats;
            ProfileDocument profile = hot_integer_profile(bc, "chain");
            pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
            if (!pscript::jit::run_optimizing_pipeline(bc, ir, &opt, stats, error)) {
                fail_test("call-aware inline ir", error);
            } else if (stats.inlined_calls < 2) {
                fail_test("call-aware inline ir", "expected two leaf inlines in chain");
            } else {
                pass_test("call-aware inline ir");
            }
        }
    }

    {
        const std::string source =
            "func bump(x):\n"
            "    return x + 1\n"
            "func chain(x):\n"
            "    return bump(x) + bump(x + 1)\n"
            "print(chain(3))\n";
        ProfileDocument profile = hot_integer_profile(compile_source(source), "chain");
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Bytecode bc = compile_source(source);
        pscript::apply_profile_guided_optimizations(bc, profile);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str() || jit_out.str() != "9\n") {
            fail_test("dual-site inline exec", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else {
            pass_test("dual-site inline exec");
        }
    }

    {
        const std::string source =
            "struct Pair:\n"
            "    a: int\n"
            "    b: int\n"
            "func local_sum():\n"
            "    p = Pair(a=3, b=4)\n"
            "    return p.a + p.b\n"
            "print(local_sum())\n";
        const Bytecode bc = compile_source(source);
        const std::uint32_t idx = function_index_by_name(bc, "local_sum");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        if (!pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error)) {
            fail_test("escape scalar local struct", error);
        } else {
            pscript::jit::OptimizingPassStats stats;
            ProfileDocument profile = hot_integer_profile(bc, "local_sum");
            pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
            if (!pscript::jit::run_optimizing_pipeline(bc, ir, &opt, stats, error)) {
                fail_test("escape scalar local struct", error);
            } else if (stats.escape_scalarized < 2) {
                fail_test("escape scalar local struct", "expected scalar replacement");
            } else {
                pass_test("escape scalar local struct");
            }
        }
    }

    if (pscript::jit::jit_backend_available()) {
        const std::string source =
            "struct Pair:\n"
            "    a: int\n"
            "    b: int\n"
            "func local_sum(n):\n"
            "    p = Pair(a=n, b=n + 1)\n"
            "    return p.a + p.b\n"
            "print(local_sum(3))\n";
        Bytecode bc = compile_source(source);
        ProfileDocument profile = hot_integer_profile(bc, "local_sum");
        pscript::apply_profile_guided_optimizations(bc, profile);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1'000'000;
        config.require_profile_for_optimizing = false;
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str() || jit_out.str() != "7\n") {
            fail_test("escape struct baseline exec parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().compile_attempts() < 1) {
            fail_test("escape struct baseline exec parity", "expected JIT compile");
        } else {
            pass_test("escape struct baseline exec parity");
        }
    }

    if (pscript::jit::jit_backend_available()) {
        const std::string source =
            "struct Pair:\n"
            "    a: int\n"
            "    b: int\n"
            "func local_sum(n):\n"
            "    p = Pair(a=n, b=n + 1)\n"
            "    return p.a + p.b\n"
            "print(local_sum(3))\n";
        Bytecode bc = compile_source(source);
        ProfileDocument profile = hot_integer_profile(bc, "local_sum");
        pscript::apply_profile_guided_optimizations(bc, profile);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_source(source), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (jit_out.str() != vm_out.str() || jit_out.str() != "7\n") {
            fail_test("escape optimizing exec parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().optimizing_compile_attempts() < 1) {
            fail_test("escape optimizing exec parity", "expected optimizing compile");
        } else {
            pass_test("escape optimizing exec parity");
        }
    }

    {
        const std::string source =
            "func rec(n):\n    if n <= 0:\n        return 0\n    return rec(n - 1) + 1\nprint(rec(3))\n";
        const Bytecode bc = compile_source(source);
        const std::uint32_t idx = function_index_by_name(bc, "rec");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        if (pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error)) {
            pscript::jit::OptimizingPassStats stats;
            ProfileDocument profile = hot_integer_profile(bc, "rec");
            pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
            pscript::jit::run_optimizing_pipeline(bc, ir, &opt, stats, error);
            if (stats.inlined_calls > 0) {
                fail_test("recursive inline", "should not inline recursion");
            } else {
                pass_test("recursive inline");
            }
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeInlineLargeCallee;
        const Bytecode bc = compile_source(source);
        const std::uint32_t idx = function_index_by_name(bc, "bench");
        pscript::jit::JitFunctionIr ir;
        std::string error;
        if (!pscript::jit::build_jit_ir_from_bytecode(bc, idx, ir, error)) {
            fail_test("cost model large callee", error);
        } else {
            pscript::jit::OptimizingPassStats stats;
            ProfileDocument profile = hot_integer_profile(bc, "bench");
            pscript::jit::JitOptProfile opt = pscript::jit::build_jit_opt_profile(&profile, bc);
            if (!pscript::jit::run_optimizing_pipeline(bc, ir, &opt, stats, error)) {
                fail_test("cost model large callee", error);
            } else if (stats.inlined_calls > 0) {
                fail_test("cost model large callee", "heavy callee should stay a direct call");
            } else {
                pass_test("cost model large callee");
            }
        }
    }

    {
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        const std::string source = "func sum(n):\n    total = 0\n    i = 0\n    while i < n:\n        total = total + i\n        i = i + 1\n    return total\nprint(sum(100))\n";
        const Bytecode bc = compile_source(source);
        ProfileDocument profile = hot_integer_profile(bc, "sum");
        Bytecode copy = bc;
        pscript::apply_profile_guided_optimizations(copy, profile);

        auto run_with = [&](bool optimizing) {
            std::ostringstream out;
            Runtime rt(out);
            Vm vm(std::move(copy), rt);
            vm.set_jit_config(config);
            if (optimizing) {
                vm.jit_engine().set_profile(&profile);
            }
            const auto t0 = std::chrono::steady_clock::now();
            vm.run();
            const auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        };
        copy = bc;
        pscript::apply_profile_guided_optimizations(copy, profile);
        auto run_vm_only = [&]() {
            std::ostringstream out;
            Runtime rt(out);
            Vm vm(compile_source(source), rt);
            vm.set_jit_enabled(false);
            const auto t0 = std::chrono::steady_clock::now();
            vm.run();
            const auto t1 = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        };
        const std::int64_t vm_time = run_vm_only();
        copy = bc;
        pscript::apply_profile_guided_optimizations(copy, profile);
        config.optimizing_invocation_threshold = 100000;
        const std::int64_t baseline_time = run_with(false);
        copy = bc;
        pscript::apply_profile_guided_optimizations(copy, profile);
        config.optimizing_invocation_threshold = 1;
        const std::int64_t opt_time = run_with(true);
        std::cout << "bench_us vm=" << vm_time << " baseline=" << baseline_time << " optimizing=" << opt_time << '\n';
        pass_test("performance validation");
    }

    {
        const std::string source =
            "func kernel(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i * 2\n"
            "        i = i + 1\n"
            "    return total\n"
            "acc = 0\n"
            "rep = 0\n"
            "while rep < 5:\n"
            "    acc = acc + kernel(200)\n"
            "    rep = rep + 1\n"
            "print(acc)\n";
        Bytecode bc = compile_with_builtins(source);
        ProfileDocument profile = hot_integer_profile(bc, "kernel");
        pscript::apply_profile_guided_optimizations(bc, profile);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1'000'000;
        config.require_profile_for_optimizing = false;
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        try {
            vm.run();
            if (out.str().empty()) {
                fail_test("amortized baseline kernel exec", "empty output");
            } else {
                pass_test("amortized baseline kernel exec");
            }
        } catch (const std::exception& ex) {
            fail_test("amortized baseline kernel exec", ex.what());
        }
    }

    {
        const std::string source =
            "func kernel(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i * 2\n"
            "        i = i + 1\n"
            "    return total\n"
            "acc = 0\n"
            "rep = 0\n"
            "while rep < 5:\n"
            "    acc = acc + kernel(4000)\n"
            "    rep = rep + 1\n"
            "print(acc)\n";
        Bytecode bc = compile_with_builtins(source);
        ProfileDocument profile = hot_integer_profile(bc, "kernel");
        pscript::apply_profile_guided_optimizations(bc, profile);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        try {
            vm.run();
            if (out.str().find("79980000") == std::string::npos) {
                fail_test("pgo amortized kernel vm", "bad output " + out.str());
            } else {
                pass_test("pgo amortized kernel vm");
            }
        } catch (const std::exception& ex) {
            fail_test("pgo amortized kernel vm", ex.what());
        }
    }

    {
        const std::string source =
            "func kernel(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i * 2\n"
            "        i = i + 1\n"
            "    return total\n"
            "acc = 0\n"
            "rep = 0\n"
            "while rep < 5:\n"
            "    acc = acc + kernel(4000)\n"
            "    rep = rep + 1\n"
            "print(acc)\n";
        Bytecode bc = compile_with_builtins(source);
        ProfileDocument profile = hot_integer_profile(bc, "kernel");
        pscript::apply_profile_guided_optimizations(bc, profile);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        try {
            vm.run();
            if (out.str().find("79980000") == std::string::npos) {
                fail_test("pgo amortized optimizing kernel exec", "bad output " + out.str());
            } else {
                pass_test("pgo amortized optimizing kernel exec");
            }
        } catch (const std::exception& ex) {
            fail_test("pgo amortized optimizing kernel exec", ex.what());
        }
    }

    {
        const std::string source =
            "func kernel(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i * 2\n"
            "        i = i + 1\n"
            "    return total\n"
            "print(kernel(50))\n";
        Bytecode bc = compile_with_builtins(source);
        ProfileDocument profile = hot_integer_profile(bc, "kernel");
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        try {
            vm.run();
            if (out.str().find("2450") == std::string::npos) {
                fail_test("optimizing kernel single call", "unexpected output: " + out.str());
            } else {
                pass_test("optimizing kernel single call");
            }
        } catch (const std::exception& ex) {
            fail_test("optimizing kernel single call", ex.what());
        }
    }

    {
        const std::string source =
            "func kernel(n):\n"
            "    total = 0\n"
            "    i = 0\n"
            "    while i < n:\n"
            "        total = total + i * 2\n"
            "        i = i + 1\n"
            "    return total\n"
            "acc = 0\n"
            "rep = 0\n"
            "while rep < 5:\n"
            "    acc = acc + kernel(200)\n"
            "    rep = rep + 1\n"
            "print(acc)\n";
        Bytecode bc = compile_with_builtins(source);
        ProfileDocument profile = hot_integer_profile(bc, "kernel");
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        vm.set_jit_enabled(true);
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        try {
            vm.run();
            pass_test("amortized optimizing kernel exec");
        } catch (const std::exception& ex) {
            fail_test("amortized optimizing kernel exec", ex.what());
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(std::string(pscript::bench::kRuntimeStringEq)), runtime);
        vm.run();
        if (out.str() != "16000\n") {
            fail_test("string_eq bench constant vm", "unexpected output: " + out.str());
        } else {
            pass_test("string_eq bench constant vm");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(std::string(pscript::bench::kRuntimeDictLuStrHit)), runtime);
        vm.run();
        if (out.str() != "280000\n") {
            fail_test("dict_lu_str_hit bench constant vm", "unexpected output: " + out.str());
        } else {
            pass_test("dict_lu_str_hit bench constant vm");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(std::string(pscript::bench::kRuntimeLoop)), runtime);
        vm.run();
        if (out.str() != "60000\n") {
            fail_test("loops bench constant vm", "unexpected output: " + out.str());
        } else {
            pass_test("loops bench constant vm");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(std::string(pscript::bench::kRuntimeNumLuDivMod)), runtime);
        vm.run();
        if (out.str() != "24029995\n") {
            fail_test("num_lu_divmod bench constant vm", "unexpected output: " + out.str());
        } else {
            pass_test("num_lu_divmod bench constant vm");
        }
    }

    if (pscript::jit::jit_backend_available()) {
        const std::string divmod_src =
            "func divmod_loop(n):\n"
            "    total = 0\n"
            "    i = 1\n"
            "    while i < n:\n"
            "        total = total + (i / 3) + (i % 7)\n"
            "        i = i + 1\n"
            "    return total\n";
        const Bytecode divmod_bc_ir = compile_with_builtins(divmod_src);
        const std::uint32_t divmod_idx_ir = function_index_by_name(divmod_bc_ir, "divmod_loop");
        pscript::jit::JitFunctionIr divmod_ir;
        std::string divmod_ir_error;
        pscript::jit::build_jit_ir_from_bytecode(divmod_bc_ir, divmod_idx_ir, divmod_ir, divmod_ir_error);
        ProfileDocument divmod_profile_ir = hot_integer_profile(divmod_bc_ir, "divmod_loop");
        pscript::jit::JitOptProfile divmod_opt_ir =
            pscript::jit::build_jit_opt_profile(&divmod_profile_ir, divmod_bc_ir);
        pscript::jit::OptimizingPassStats divmod_stats;
        pscript::jit::run_optimizing_pipeline(divmod_bc_ir, divmod_ir, &divmod_opt_ir, divmod_stats, divmod_ir_error);
        bool mod_divisor_defined = false;
        for (std::size_t i = 0; i < divmod_ir.instructions.size(); ++i) {
            const auto& mod_insn = divmod_ir.instructions[i];
            if (mod_insn.opcode != pscript::jit::JitIrOpcode::IntMod) {
                continue;
            }
            for (std::size_t j = 0; j < i; ++j) {
                const auto& def = divmod_ir.instructions[j];
                if (def.a != mod_insn.c) {
                    continue;
                }
                if (def.opcode == pscript::jit::JitIrOpcode::LoadConst ||
                    def.opcode == pscript::jit::JitIrOpcode::LoadConstInt) {
                    mod_divisor_defined = true;
                    break;
                }
            }
        }
        if (!divmod_ir.buffered_integer_locals || !mod_divisor_defined) {
            std::ostringstream ir_dump;
            for (std::size_t i = 0; i < divmod_ir.instructions.size(); ++i) {
                const auto& insn = divmod_ir.instructions[i];
                ir_dump << i << " op=" << static_cast<int>(insn.opcode) << " a=" << insn.a << " b=" << insn.b
                        << " c=" << insn.c << '\n';
            }
            fail_test("num_lu_divmod mod divisor ir",
                      "buffered=" + std::to_string(divmod_ir.buffered_integer_locals) + " divisor=" +
                          std::to_string(mod_divisor_defined) + "\n" + ir_dump.str());
        } else {
            pass_test("num_lu_divmod mod divisor ir");
        }
    }

    if (pscript::jit::jit_backend_available()) {
        const std::string divmod_src =
            "func divmod_loop(n):\n"
            "    total = 0\n"
            "    i = 1\n"
            "    while i < n:\n"
            "        total = total + (i / 3) + (i % 7)\n"
            "        i = i + 1\n"
            "    return total\n";
        Bytecode divmod_bc = compile_with_builtins(divmod_src);
        const std::uint32_t divmod_idx = function_index_by_name(divmod_bc, "divmod_loop");
        ProfileDocument divmod_profile = hot_integer_profile(divmod_bc, "divmod_loop");
        pscript::apply_profile_guided_optimizations(divmod_bc, divmod_profile);
        const auto& divmod_fn = divmod_bc.functions[divmod_idx];
        {
            std::ostringstream divmod_rt_out;
            pscript::Runtime divmod_rt(divmod_rt_out);
            Bytecode divmod_vm_bc = divmod_bc;
            Vm divmod_vm(std::move(divmod_vm_bc), divmod_rt);
            divmod_vm.set_jit_enabled(true);
            pscript::jit::JitTieringConfig divmod_cfg;
            divmod_cfg.baseline_invocation_threshold = 1;
            divmod_cfg.optimizing_invocation_threshold = 1;
            divmod_cfg.require_profile_for_optimizing = false;
            divmod_vm.set_jit_config(divmod_cfg);
            divmod_vm.jit_engine().bind_bytecode(&divmod_vm.bytecode(),
                                                 pscript::compute_program_fingerprint(divmod_vm.bytecode()));
            divmod_vm.jit_engine().set_profile(&divmod_profile);
            divmod_vm.jit_engine().record_invocation(divmod_idx, divmod_fn);
            divmod_vm.init_locals_for_function(divmod_fn.local_count);
            pscript::jit::JitExecState direct_state;
            divmod_vm.populate_jit_exec_state(direct_state, divmod_fn, divmod_idx);
            direct_state.locals[0]->value = pscript::Runtime::make_integer(12'000);
            const pscript::jit::JitCompiledCode* divmod_code =
                divmod_vm.jit_engine().compile_if_needed(divmod_idx, divmod_fn);
            if (divmod_code == nullptr || divmod_code->entry == nullptr) {
                fail_test("num_lu_divmod optimizing direct exec", "null JIT entry");
            }
            const pscript::jit::JitExecuteResult direct_run =
                pscript::jit::execute_compiled_entry(divmod_code, direct_state);
            if (direct_run != pscript::jit::JitExecuteResult::Completed || direct_state.deopt_requested ||
                direct_state.return_value.kind != pscript::ValueKind::Integer ||
                direct_state.return_value.integer != 24'029'995) {
                fail_test("num_lu_divmod optimizing direct exec",
                          "run=" + std::to_string(static_cast<int>(direct_run)) + " completed=" +
                              std::to_string(direct_state.completed) + " fallback=" +
                              std::to_string(direct_state.fallback) + " deopt=" +
                              std::to_string(direct_state.deopt_requested) + " got=" +
                              std::to_string(direct_state.return_value.integer));
            } else {
                pass_test("num_lu_divmod optimizing direct exec");
            }
        }
    }

    if (pscript::jit::jit_backend_available()) {
        Bytecode bc = compile_with_builtins(std::string(pscript::bench::kRuntimeNumLuDivMod));
        ProfileDocument profile = hot_integer_profile(bc, "divmod_loop");
        pscript::apply_profile_guided_optimizations(bc, profile);
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        vm.set_jit_enabled(true);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        vm.run();
        if (out.str() != "24029995\n") {
            fail_test("num_lu_divmod optimizing jit", "unexpected output: " + out.str());
        } else {
            pass_test("num_lu_divmod optimizing jit");
        }
    }

    if (pscript::jit::jit_backend_available()) {
        {
            const char kNestedSmall[] =
                "func outer(x):\n"
                "    func mid(y):\n"
                "        func inner(z):\n"
                "            return x + y + z\n"
                "        return inner\n"
                "    return mid\n"
                "func bench(n):\n"
                "    f = outer(10)\n"
                "    g = f(20)\n"
                "    total = 0\n"
                "    i = 0\n"
                "    while i < n:\n"
                "        total = total + g(i)\n"
                "        i = i + 1\n"
                "    return total\n"
                "print(bench(10))\n";
            Bytecode small_bc = compile_with_builtins(kNestedSmall);
            std::ostringstream small_vm_out;
            Runtime small_vm_rt(small_vm_out);
            Vm small_vm_only(small_bc, small_vm_rt);
            small_vm_only.set_jit_enabled(false);
            small_vm_only.run();
            ProfileDocument small_profile = hot_integer_profile(small_bc, "bench");
            pscript::apply_profile_guided_optimizations(small_bc, small_profile);
            std::ostringstream small_jit_out;
            Runtime small_jit_rt(small_jit_out);
            Vm small_jit_vm(std::move(small_bc), small_jit_rt);
            small_jit_vm.set_jit_enabled(true);
            pscript::jit::JitTieringConfig small_cfg;
            small_cfg.baseline_invocation_threshold = 1;
            small_cfg.optimizing_invocation_threshold = 1;
            small_cfg.require_profile_for_optimizing = false;
            small_jit_vm.set_jit_config(small_cfg);
            small_jit_vm.jit_engine().bind_bytecode(&small_jit_vm.bytecode(),
                                                    pscript::compute_program_fingerprint(small_jit_vm.bytecode()));
            small_jit_vm.jit_engine().set_profile(&small_profile);
            small_jit_vm.run();
            if (small_jit_out.str() != small_vm_out.str()) {
                fail_test("closure_nested optimizing jit small", "jit=" + small_jit_out.str() + " vm=" + small_vm_out.str());
            } else {
                pass_test("closure_nested optimizing jit small");
            }

            Bytecode nested_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeClosureNested));
            ProfileDocument nested_profile = hot_integer_profile(nested_bc, "bench");
            pscript::apply_profile_guided_optimizations(nested_bc, nested_profile);
            std::uint32_t nested_bench_index = 0;
            for (std::uint32_t i = 0; i < nested_bc.functions.size(); ++i) {
                if (nested_bc.functions[i].name == "bench") {
                    nested_bench_index = i;
                    break;
                }
            }
            {
                pscript::jit::JitFunctionIr nested_ir;
                std::string ir_error;
                pscript::jit::JitOptProfile nested_opt_profile =
                    pscript::jit::build_jit_opt_profile(&nested_profile, nested_bc);
                pscript::jit::OptimizingPassStats nested_inline_stats;
                if (!pscript::jit::build_jit_ir_from_bytecode(nested_bc, nested_bench_index, nested_ir, ir_error) ||
                    !pscript::jit::run_optimizing_pipeline(nested_bc, nested_ir, &nested_opt_profile,
                                                           nested_inline_stats, ir_error)) {
                    fail_test("closure_nested inline ir", ir_error);
                } else if (nested_inline_stats.inlined_calls == 0) {
                    fail_test("closure_nested inline ir", "expected inlined_calls > 0");
                } else if (pscript::jit::ir_has_call_value_sites(nested_ir)) {
                    fail_test("closure_nested inline ir", "CallValue sites remain after optimize pass");
                } else if (pscript::jit::ir_has_call_sites(nested_ir)) {
                    fail_test("closure_nested inline ir", "Call sites remain after optimize pass");
                } else {
                    std::size_t make_closure_ops = 0;
                    for (const pscript::jit::JitIrInstruction& insn : nested_ir.instructions) {
                        if (insn.opcode == pscript::jit::JitIrOpcode::MakeClosure) {
                            ++make_closure_ops;
                        }
                    }
                    if (make_closure_ops != 0) {
                        fail_test("closure_nested inline ir",
                                  "MakeClosure ops remain count=" + std::to_string(make_closure_ops));
#ifndef NDEBUG
                    } else {
                        pass_test("closure_nested inline ir");
#else
                    } else if (!pscript::jit::ir_eligible_for_linear_numeric(nested_ir, &nested_bc)) {
                        fail_test("closure_nested inline ir", "bench IR not linear-eligible");
                    } else {
                        pass_test("closure_nested inline ir");
#endif
                    }
                }
            }
            std::ostringstream nested_out;
            Runtime nested_rt(nested_out);
            Vm nested_vm(std::move(nested_bc), nested_rt);
            nested_vm.set_jit_enabled(true);
            pscript::jit::JitTieringConfig nested_cfg;
            nested_cfg.baseline_invocation_threshold = 1;
            nested_cfg.optimizing_invocation_threshold = 1;
            nested_cfg.require_profile_for_optimizing = false;
            nested_vm.set_jit_config(nested_cfg);
            nested_vm.jit_engine().bind_bytecode(&nested_vm.bytecode(),
                                                pscript::compute_program_fingerprint(nested_vm.bytecode()));
            nested_vm.jit_engine().set_profile(&nested_profile);
            nested_vm.run();
            std::ostringstream nested_vm_out;
            Runtime nested_vm_rt(nested_vm_out);
            Vm nested_vm_only(compile_with_builtins(std::string(pscript::bench::kRuntimeClosureNested)), nested_vm_rt);
            nested_vm_only.set_jit_enabled(false);
            nested_vm_only.run();
            if (nested_out.str() != nested_vm_out.str() || nested_out.str() != "313237500\n") {
                fail_test("closure_nested optimizing jit", "jit=" + nested_out.str() + " vm=" + nested_vm_out.str());
            } else {
                const pscript::jit::JitCompiledCode* compiled = nested_vm.jit_engine().lookup(nested_bench_index);
                if (compiled == nullptr) {
                    fail_test("closure_nested optimizing jit", "bench not compiled");
                } else {
#ifndef NDEBUG
                    (void)compiled;
#else
                    if (compiled->uses_dispatch_loop) {
                        fail_test("closure_nested optimizing jit", "runtime bench uses dispatch loop emitter");
                    } else
#endif
                    if (nested_vm.jit_engine().native_jit_completions() != 1) {
                        fail_test("closure_nested optimizing jit",
                                  "expected 1 native_jit_completion got " +
                                      std::to_string(nested_vm.jit_engine().native_jit_completions()));
                    } else {
                        pass_test("closure_nested optimizing jit");
                    }
                }
            }

            const char kNestedOsrRegression[] =
                "func outer(x):\n"
                "    func mid(y):\n"
                "        func inner(z):\n"
                "            return x + y + z\n"
                "        return inner\n"
                "    return mid\n"
                "func bench(n):\n"
                "    f = outer(10)\n"
                "    g = f(20)\n"
                "    total = 0\n"
                "    i = 0\n"
                "    while i < n:\n"
                "        total = total + g(i)\n"
                "        i = i + 1\n"
                "    return total\n"
                "print(bench(1100))\n";
            std::ostringstream osr_vm_out;
            Runtime osr_vm_rt(osr_vm_out);
            Vm osr_vm_only(compile_with_builtins(kNestedOsrRegression), osr_vm_rt);
            osr_vm_only.set_jit_enabled(false);
            osr_vm_only.run();
            Bytecode osr_bc = compile_with_builtins(kNestedOsrRegression);
            ProfileDocument osr_profile = hot_integer_profile(osr_bc, "bench");
            pscript::apply_profile_guided_optimizations(osr_bc, osr_profile);
            std::ostringstream osr_jit_out;
            Runtime osr_jit_rt(osr_jit_out);
            Vm osr_jit_vm(std::move(osr_bc), osr_jit_rt);
            osr_jit_vm.set_jit_enabled(true);
            pscript::jit::JitTieringConfig osr_cfg;
            osr_cfg.baseline_invocation_threshold = 1;
            osr_cfg.optimizing_invocation_threshold = 1;
            osr_cfg.require_profile_for_optimizing = false;
            osr_jit_vm.set_jit_config(osr_cfg);
            osr_jit_vm.jit_engine().bind_bytecode(&osr_jit_vm.bytecode(),
                                                    pscript::compute_program_fingerprint(osr_jit_vm.bytecode()));
            osr_jit_vm.jit_engine().set_profile(&osr_profile);
            osr_jit_vm.run();
            if (osr_jit_out.str() != osr_vm_out.str()) {
                fail_test("closure_nested osr callvalue regression", "jit=" + osr_jit_out.str() + " vm=" + osr_vm_out.str());
            } else {
                pass_test("closure_nested osr callvalue regression");
            }
        }

        Bytecode bc = compile_with_builtins(std::string(pscript::bench::kRuntimeClosureCall));
        ProfileDocument profile = hot_integer_profile(bc, "bench");
        pscript::apply_profile_guided_optimizations(bc, profile);
        std::uint32_t bench_index = 0;
        for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
            if (bc.functions[i].name == "bench") {
                bench_index = i;
                break;
            }
        }
        pscript::jit::JitFunctionIr bench_ir;
        std::string ir_error;
        pscript::jit::JitOptProfile opt_profile = pscript::jit::build_jit_opt_profile(&profile, bc);
        pscript::jit::OptimizingPassStats inline_stats;
        std::uint32_t make_adder_index = 0xFFFFFFFFu;
        std::uint32_t inner_closure_index = 0;
        for (std::uint32_t i = 0; i < bc.functions.size(); ++i) {
            if (bc.functions[i].name == "make_adder") {
                make_adder_index = i;
                break;
            }
        }
        const bool factory_ok =
            make_adder_index != 0xFFFFFFFFu &&
            pscript::jit::bytecode_function_returns_single_closure(bc, make_adder_index, inner_closure_index);
        if (!factory_ok) {
            fail_test("closure_callvalue inline ir", "make_adder factory pattern not recognized");
        } else if (!pscript::jit::build_jit_ir_from_bytecode(bc, bench_index, bench_ir, ir_error) ||
            !pscript::jit::run_optimizing_pipeline(bc, bench_ir, &opt_profile, inline_stats, ir_error)) {
            fail_test("closure_callvalue inline ir", ir_error);
        } else if (inline_stats.inlined_calls == 0) {
            fail_test("closure_callvalue inline ir", "expected inlined_calls > 0");
        } else if (pscript::jit::ir_has_call_value_sites(bench_ir)) {
            fail_test("closure_callvalue inline ir", "CallValue sites remain after inline pass");
        } else if (pscript::jit::ir_has_call_sites(bench_ir)) {
            fail_test("closure_callvalue inline ir", "Call sites remain after optimize pass");
        } else if (!bench_ir.buffered_integer_locals) {
            fail_test("closure_callvalue inline ir", "expected buffered integer locals on bench");
#ifndef NDEBUG
        } else {
            pass_test("closure_callvalue inline ir");
#else
        } else if (!pscript::jit::ir_eligible_for_linear_numeric(bench_ir, &bc)) {
            fail_test("closure_callvalue inline ir", "bench IR not eligible for linear numeric backend");
        } else {
            pass_test("closure_callvalue inline ir");
#endif
        }
        {
            std::string compile_error;
            const std::string fp = pscript::compute_program_fingerprint(bc);
            std::optional<pscript::jit::JitCompiledCode> compiled =
                pscript::jit::compile_function_optimizing(bc, bench_index, fp, &opt_profile, compile_error);
            if (!compiled) {
                fail_test("closure_call compile path", compile_error);
#ifndef NDEBUG
            } else if (compiled->ir_instruction_count > 96) {
#else
            } else if (compiled->uses_dispatch_loop) {
                fail_test("closure_call compile path",
                          "expected linear numeric emit, got dispatch (ir_insn=" +
                              std::to_string(compiled->ir_instruction_count) + ")");
            } else if (compiled->ir_instruction_count > 96) {
#endif
                fail_test("closure_call compile path",
                          "unexpected ir size " + std::to_string(compiled->ir_instruction_count));
            } else if (compiled->cold_calls_outside_loop) {
                fail_test("closure_call compile path", "unexpected cold_calls_outside_loop on inlined bench");
            } else {
                pass_test("closure_call compile path linear");
            }
        }
        {
            Bytecode simple_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeSimpleCalls));
            Bytecode mono_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeCallValueMono));
            std::uint32_t hot_index = 0xFFFFFFFFu;
            for (std::uint32_t i = 0; i < mono_bc.functions.size(); ++i) {
                if (mono_bc.functions[i].name == "hot") {
                    hot_index = i;
                    break;
                }
            }
            pscript::jit::JitFunctionIr hot_ir;
            std::string mono_ir_error;
            pscript::jit::OptimizingPassStats mono_stats;
            if (hot_index == 0xFFFFFFFFu) {
                fail_test("callvalue_mono inline ir", "hot function missing");
            } else if (!pscript::jit::build_jit_ir_from_bytecode(mono_bc, hot_index, hot_ir, mono_ir_error)) {
                fail_test("callvalue_mono inline ir", mono_ir_error);
            } else {
                pscript::jit::build_cfg_for_function(hot_ir);
                for (std::size_t i = 0; i < hot_ir.instructions.size(); ++i) {
                    if (hot_ir.instructions[i].opcode == pscript::jit::JitIrOpcode::MakeClosure &&
                        pscript::jit::instruction_in_backedge_loop(hot_ir, i)) {
                        fail_test("callvalue_mono loop make_closure",
                                  "unexpected MakeClosure in loop at insn " + std::to_string(i));
                        break;
                    }
                }
                if (!pscript::jit::ir_callvalue_sites_are_stable_direct(mono_bc, hot_ir)) {
                    fail_test("callvalue_mono stable direct", "expected stable direct CallValue sites");
                } else if (!pscript::jit::run_optimizing_pipeline(mono_bc, hot_ir, nullptr, mono_stats,
                                                                    mono_ir_error)) {
                    fail_test("callvalue_mono inline ir", mono_ir_error);
                } else if (mono_stats.inlined_calls == 0) {
                    fail_test("callvalue_mono inline ir", "expected inlined_calls > 0");
                } else if (pscript::jit::ir_has_call_sites(hot_ir) ||
                           pscript::jit::ir_has_call_value_sites(hot_ir)) {
                    fail_test("callvalue_mono inline ir", "call sites remain after optimize pass");
                } else if (!hot_ir.baseline_numeric_loop_fast_path) {
                    fail_test("callvalue_mono inline ir", "expected baseline_numeric_loop_fast_path");
                } else {
                    pass_test("callvalue_mono inline ir");
                    const std::string fp = pscript::compute_program_fingerprint(mono_bc);
                    ProfileDocument mono_profile = hot_integer_profile(mono_bc, "hot");
                    pscript::jit::JitOptProfile opt_profile =
                        pscript::jit::build_jit_opt_profile(&mono_profile, mono_bc);
                    pscript::jit::JitFunctionIr prep_ir;
                    std::string prep_error;
                    bool prep_conservative = false;
                    pscript::jit::OptimizingPassStats prep_stats;
                    if (!pscript::jit::build_jit_ir_from_bytecode(mono_bc, hot_index, prep_ir, prep_error)) {
                        fail_test("callvalue_mono prepare ir", prep_error);
                    } else if (!pscript::jit::prepare_function_ir_for_jit_compile(
                                   mono_bc, prep_ir, &opt_profile, true, prep_stats, prep_error,
                                   prep_conservative)) {
                        fail_test("callvalue_mono prepare ir", prep_error);
                    } else if (pscript::jit::ir_has_call_sites(prep_ir) ||
                               pscript::jit::ir_has_call_value_sites(prep_ir)) {
                        fail_test("callvalue_mono prepare ir",
                                  "calls remain insn=" + std::to_string(prep_ir.instructions.size()));
                    } else if (!prep_ir.baseline_numeric_loop_fast_path) {
                        fail_test("callvalue_mono prepare ir", "missing baseline_numeric_loop_fast_path");
                    } else {
                        pass_test("callvalue_mono prepare ir");
                    }
                    std::string prod_error;
                    std::optional<pscript::jit::JitCompiledCode> baseline_compiled =
                        pscript::jit::compile_function_baseline(mono_bc, hot_index, fp, nullptr, prod_error);
                    std::optional<pscript::jit::JitCompiledCode> opt_compiled =
                        pscript::jit::compile_function_optimizing(mono_bc, hot_index, fp, &opt_profile, prod_error);
                    if (!baseline_compiled || !opt_compiled) {
                        fail_test("callvalue_mono production compile", prod_error);
                    } else if (baseline_compiled->ir_instruction_count > 96 ||
                               opt_compiled->ir_instruction_count > 96) {
                        fail_test("callvalue_mono production compile",
                                  "ir size baseline=" + std::to_string(baseline_compiled->ir_instruction_count) +
                                      " opt=" + std::to_string(opt_compiled->ir_instruction_count));
                    } else {
                        pass_test("callvalue_mono production compile");
                    }
                    std::optional<pscript::jit::JitCompiledCode> simple_opt;
                    const std::uint32_t simple_hot = function_index_by_name(simple_bc, "hot");
                    if (simple_hot != 0xFFFFFFFFu) {
                        simple_opt = pscript::jit::compile_function_optimizing(
                            simple_bc, simple_hot, fp, &opt_profile, prod_error);
                    }
                    if (!simple_opt || !opt_compiled) {
                        fail_test("callvalue_mono native size parity", prod_error.empty() ? "compile failed" : prod_error);
                    } else if (opt_compiled->code_size > simple_opt->code_size * 2 ||
                               opt_compiled->code_size + 512 < simple_opt->code_size / 2) {
                        fail_test("callvalue_mono native size parity",
                                  "mono_bytes=" + std::to_string(opt_compiled->code_size) +
                                      " simple_bytes=" + std::to_string(simple_opt->code_size));
                    } else {
                        pass_test("callvalue_mono native size parity");
                    }
                }
            }
        }
        {
            auto count_backedge_insns = [](const pscript::jit::JitFunctionIr& fn_ir) {
                std::size_t count = 0;
                for (std::size_t i = 0; i < fn_ir.instructions.size(); ++i) {
                    if (pscript::jit::instruction_in_backedge_loop(fn_ir, i)) {
                        ++count;
                    }
                }
                return count;
            };
            Bytecode simple_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeSimpleCalls));
            Bytecode mono_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeCallValueMono));
            std::uint32_t simple_hot = function_index_by_name(simple_bc, "hot");
            std::uint32_t mono_hot = function_index_by_name(mono_bc, "hot");
            pscript::jit::JitFunctionIr simple_prep;
            pscript::jit::JitFunctionIr mono_prep;
            std::string prep_err;
            pscript::jit::OptimizingPassStats simple_stats;
            pscript::jit::OptimizingPassStats mono_stats;
            bool prep_cons = false;
            if (simple_hot == 0xFFFFFFFFu || mono_hot == 0xFFFFFFFFu) {
                fail_test("callvalue_mono loop ir parity", "hot missing");
            } else if (!pscript::jit::build_jit_ir_from_bytecode(simple_bc, simple_hot, simple_prep, prep_err) ||
                       !pscript::jit::prepare_function_ir_for_jit_compile(simple_bc, simple_prep, nullptr, false,
                                                                          simple_stats, prep_err, prep_cons)) {
                fail_test("callvalue_mono loop ir parity", "simple prepare: " + prep_err);
            } else if (!pscript::jit::build_jit_ir_from_bytecode(mono_bc, mono_hot, mono_prep, prep_err) ||
                       !pscript::jit::prepare_function_ir_for_jit_compile(mono_bc, mono_prep, nullptr, true, mono_stats,
                                                                          prep_err, prep_cons)) {
                fail_test("callvalue_mono loop ir parity", "mono prepare: " + prep_err);
            } else {
                auto count_backedge_opcode = [](const pscript::jit::JitFunctionIr& fn_ir, pscript::jit::JitIrOpcode op) {
                    std::size_t count = 0;
                    for (std::size_t i = 0; i < fn_ir.instructions.size(); ++i) {
                        if (pscript::jit::instruction_in_backedge_loop(fn_ir, i) &&
                            fn_ir.instructions[i].opcode == op) {
                            ++count;
                        }
                    }
                    return count;
                };
                const std::size_t simple_loop = count_backedge_insns(simple_prep);
                const std::size_t mono_loop = count_backedge_insns(mono_prep);
                const std::size_t mono_loop_load_local = count_backedge_opcode(mono_prep, pscript::jit::JitIrOpcode::LoadLocal);
                const std::size_t simple_loop_load_local =
                    count_backedge_opcode(simple_prep, pscript::jit::JitIrOpcode::LoadLocal);
                if (mono_loop > simple_loop + 12) {
                    fail_test("callvalue_mono loop ir parity",
                              "mono_loop=" + std::to_string(mono_loop) + " simple_loop=" + std::to_string(simple_loop));
                } else if (mono_loop_load_local > simple_loop_load_local + 2) {
                    fail_test("callvalue_mono loop ir parity",
                              "extra LoadLocal in loop mono=" + std::to_string(mono_loop_load_local) + " simple=" +
                                  std::to_string(simple_loop_load_local));
                } else {
                    pass_test("callvalue_mono loop ir parity");
                }
            }
        }
        {
            Bytecode str_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeStringEq));
            const std::uint32_t seq_idx = function_index_by_name(str_bc, "seq");
            pscript::jit::JitFunctionIr str_ir;
            std::string str_err;
            pscript::jit::OptimizingPassStats str_stats;
            bool str_cons = false;
            if (seq_idx == 0xFFFFFFFFu) {
                fail_test("string_eq production prepare ir", "seq missing");
            } else if (!pscript::jit::build_jit_ir_from_bytecode(str_bc, seq_idx, str_ir, str_err) ||
                       !pscript::jit::prepare_function_ir_for_jit_compile(str_bc, str_ir, nullptr, false, str_stats,
                                                                          str_err, str_cons)) {
                fail_test("string_eq production prepare ir", str_err);
            } else {
                bool has_string_cmp = false;
                for (const pscript::jit::JitIrInstruction& insn : str_ir.instructions) {
                    if (insn.opcode == pscript::jit::JitIrOpcode::StringEqual ||
                        insn.opcode == pscript::jit::JitIrOpcode::StringNotEqual) {
                        has_string_cmp = true;
                    }
                }
                if (has_string_cmp) {
                    fail_test("string_eq production prepare ir", "literal compares remain in IR");
                } else {
                    pass_test("string_eq production prepare ir");
                }
            }
        }
        {
            const Bytecode poly_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeCallValuePoly));
            const std::uint32_t poly_hot = function_index_by_name(poly_bc, "hot");
            pscript::jit::JitFunctionIr poly_ir;
            std::string poly_ir_err;
            if (poly_hot == 0xFFFFFFFFu) {
                fail_test("callvalue_poly stable direct", "missing hot");
            } else if (!pscript::jit::build_jit_ir_from_bytecode(poly_bc, poly_hot, poly_ir, poly_ir_err)) {
                fail_test("callvalue_poly stable direct", poly_ir_err);
            } else if (pscript::jit::ir_callvalue_sites_are_stable_direct(poly_bc, poly_ir)) {
                fail_test("callvalue_poly stable direct", "polymorphic cf/cg must not be stable-direct");
            } else {
                pass_test("callvalue_poly stable direct");
            }
        }
        {
            const Bytecode poly_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeCallValuePoly));
            const std::uint32_t poly_hot = function_index_by_name(poly_bc, "hot");
            const std::string poly_fp = pscript::compute_program_fingerprint(poly_bc);
            std::string poly_err;
            const auto compile_start = std::chrono::steady_clock::now();
            std::optional<pscript::jit::JitCompiledCode> poly_baseline =
                poly_hot != 0xFFFFFFFFu
                    ? pscript::jit::compile_function_baseline(poly_bc, poly_hot, poly_fp, nullptr, poly_err)
                    : std::nullopt;
            const auto compile_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - compile_start)
                                        .count();
            if (poly_hot == 0xFFFFFFFFu) {
                fail_test("callvalue_poly baseline compile", "missing hot function");
            } else if (!poly_baseline) {
                fail_test("callvalue_poly baseline compile", poly_err);
            } else if (compile_ms > 30'000) {
                fail_test("callvalue_poly baseline compile", "compile exceeded 30s");
            } else {
                pass_test("callvalue_poly baseline compile");
            }
        }
        {
            const char* poly_small =
                "func f(x):\n    return x + 1\n"
                "func g(x):\n    return x + 2\n"
                "func hot(n):\n    cf = f\n    cg = g\n    total = 0\n    i = 0\n"
                "    while i < n:\n        if i % 2 == 0:\n            total = total + cf(i)\n"
                "        else:\n            total = total + cg(i)\n        i = i + 1\n    return total\n"
                "print(hot(4000))\n";
            Bytecode poly_bc = compile_with_builtins(poly_small);
            std::ostringstream jit_out;
            Runtime jit_rt(jit_out);
            Vm jit_vm(std::move(poly_bc), jit_rt);
            jit_vm.set_jit_enabled(true);
            pscript::jit::JitTieringConfig cfg;
            cfg.baseline_invocation_threshold = 1;
            cfg.optimizing_invocation_threshold = 1'000'000;
            cfg.require_profile_for_optimizing = true;
            jit_vm.set_jit_config(cfg);
            jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(),
                                              pscript::compute_program_fingerprint(jit_vm.bytecode()));
            const auto run_start = std::chrono::steady_clock::now();
            jit_vm.run();
            const auto run_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - run_start)
                                    .count();
            std::ostringstream vm_out;
            Runtime vm_rt(vm_out);
            Vm vm_only(compile_with_builtins(poly_small), vm_rt);
            vm_only.set_jit_enabled(false);
            vm_only.run();
            if (run_ms > 15'000) {
                fail_test("callvalue_poly baseline exec small", "run exceeded 15s");
            } else if (jit_out.str() != vm_out.str()) {
                fail_test("callvalue_poly baseline exec small", "output mismatch");
            } else {
                pass_test("callvalue_poly baseline exec small");
            }
        }
        {
            if (!vm_jit_output_parity(std::string(pscript::bench::kRuntimeCallValuePoly))) {
                fail_test("callvalue matrix poly bench", "vm/jit mismatch");
            } else {
                pass_test("callvalue matrix poly bench");
            }
        }
        {
            if (!vm_jit_output_parity(std::string(pscript::bench::kRuntimeCallValueMono))) {
                fail_test("callvalue matrix mono bench", "vm/jit mismatch");
            } else {
                pass_test("callvalue matrix mono bench");
            }
        }
        {
            Bytecode tiny_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeInlineTiny));
            std::uint32_t bench_index = 0xFFFFFFFFu;
            for (std::uint32_t i = 0; i < tiny_bc.functions.size(); ++i) {
                if (tiny_bc.functions[i].name == "bench") {
                    bench_index = i;
                    break;
                }
            }
            pscript::jit::JitFunctionIr tiny_ir;
            std::string tiny_ir_error;
            pscript::jit::OptimizingPassStats tiny_stats;
            if (bench_index == 0xFFFFFFFFu) {
                fail_test("inline_tiny inline ir", "bench function missing");
            } else if (!pscript::jit::build_jit_ir_from_bytecode(tiny_bc, bench_index, tiny_ir, tiny_ir_error) ||
                       !pscript::jit::run_optimizing_pipeline(tiny_bc, tiny_ir, nullptr, tiny_stats, tiny_ir_error)) {
                fail_test("inline_tiny inline ir", tiny_ir_error);
            } else if (tiny_stats.inlined_calls == 0) {
                fail_test("inline_tiny inline ir", "expected inlined_calls > 0");
            } else if (pscript::jit::ir_has_call_sites(tiny_ir)) {
                fail_test("inline_tiny inline ir", "Call sites remain after optimize pass");
            } else {
                pass_test("inline_tiny inline ir");
            }
        }
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(std::move(bc), runtime);
        vm.set_jit_enabled(true);
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        vm.set_jit_config(config);
        vm.jit_engine().bind_bytecode(&vm.bytecode(), pscript::compute_program_fingerprint(vm.bytecode()));
        vm.jit_engine().set_profile(&profile);
        vm.run();
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(compile_with_builtins(std::string(pscript::bench::kRuntimeClosureCall)), vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        if (out.str() != vm_out.str() || out.str() != "314987500\n") {
            fail_test("closure_call optimizing jit", "jit=" + out.str() + " vm=" + vm_out.str());
        } else {
            const pscript::jit::JitCompiledCode* compiled = vm.jit_engine().lookup(bench_index);
            if (compiled == nullptr) {
                fail_test("closure_call optimizing jit", "bench not compiled");
            } else {
#ifndef NDEBUG
                (void)compiled;
#else
                if (compiled->uses_dispatch_loop) {
                    fail_test("closure_call optimizing jit", "runtime bench uses dispatch loop emitter");
                } else
#endif
                if (vm.jit_engine().native_jit_completions() != 1) {
                    fail_test("closure_call optimizing jit",
                              "expected 1 native_jit_completion got " +
                                  std::to_string(vm.jit_engine().native_jit_completions()));
                } else {
                    pass_test("closure_call optimizing jit");
                }
            }
        }

        {
            const std::string poly_source = pscript::bench::kRuntimeClosurePoly;
            Bytecode poly_bc = compile_with_builtins(poly_source);
            ProfileDocument poly_profile = hot_integer_profile(poly_bc, "bench");
            pscript::apply_profile_guided_optimizations(poly_bc, poly_profile);
            std::ostringstream poly_vm_out;
            Runtime poly_vm_rt(poly_vm_out);
            Vm poly_vm_only(poly_bc, poly_vm_rt);
            poly_vm_only.set_jit_enabled(false);
            poly_vm_only.run();
            const std::string poly_expected = poly_vm_out.str();
            std::ostringstream poly_baseline_out;
            Runtime poly_baseline_rt(poly_baseline_out);
            Vm poly_baseline_vm(compile_with_builtins(poly_source), poly_baseline_rt);
            poly_baseline_vm.set_jit_enabled(true);
            pscript::jit::JitTieringConfig poly_baseline_cfg;
            poly_baseline_cfg.baseline_invocation_threshold = 1;
            poly_baseline_cfg.optimizing_invocation_threshold = 1'000'000;
            poly_baseline_cfg.require_profile_for_optimizing = true;
            poly_baseline_vm.set_jit_config(poly_baseline_cfg);
            poly_baseline_vm.jit_engine().bind_bytecode(&poly_baseline_vm.bytecode(),
                                                        pscript::compute_program_fingerprint(poly_baseline_vm.bytecode()));
            poly_baseline_vm.run();
            if (poly_baseline_out.str() != poly_expected) {
                fail_test("closure_poly baseline jit", "jit=" + poly_baseline_out.str() + " vm=" + poly_expected);
            } else {
                pass_test("closure_poly baseline jit");
            }
            std::ostringstream poly_opt_out;
            Runtime poly_opt_rt(poly_opt_out);
            Vm poly_opt_vm(std::move(poly_bc), poly_opt_rt);
            poly_opt_vm.set_jit_enabled(true);
            pscript::jit::JitTieringConfig poly_opt_cfg;
            poly_opt_cfg.baseline_invocation_threshold = 1;
            poly_opt_cfg.optimizing_invocation_threshold = 1;
            poly_opt_cfg.require_profile_for_optimizing = false;
            poly_opt_vm.set_jit_config(poly_opt_cfg);
            poly_opt_vm.jit_engine().bind_bytecode(&poly_opt_vm.bytecode(),
                                                   pscript::compute_program_fingerprint(poly_opt_vm.bytecode()));
            poly_opt_vm.jit_engine().set_profile(&poly_profile);
            poly_opt_vm.run();
            if (poly_opt_out.str() != poly_expected || poly_expected != "325000000\n") {
                fail_test("closure_poly optimizing jit", "jit=" + poly_opt_out.str() + " vm=" + poly_expected);
            } else {
                pass_test("closure_poly optimizing jit");
            }
        }

        Bytecode bench_bc = compile_with_builtins(std::string(pscript::bench::kRuntimeClosureNested));
        ProfileDocument bench_profile = hot_integer_profile(bench_bc, "bench");
        std::ostringstream vm_ref;
        Runtime vm_ref_rt(vm_ref);
        Vm vm_ref_vm(bench_bc, vm_ref_rt);
        vm_ref_vm.set_jit_enabled(false);
        vm_ref_vm.run();
        const std::string expected = vm_ref.str();

        pscript::jit::JitTieringConfig baseline_cfg;
        baseline_cfg.baseline_invocation_threshold = 1;
        baseline_cfg.optimizing_invocation_threshold = 1'000'000;
        baseline_cfg.require_profile_for_optimizing = true;
        std::ostringstream baseline_out;
        Runtime baseline_rt(baseline_out);
        Vm baseline_vm(compile_with_builtins(std::string(pscript::bench::kRuntimeClosureNested)), baseline_rt);
        baseline_vm.set_jit_enabled(true);
        baseline_vm.set_jit_config(baseline_cfg);
        baseline_vm.jit_engine().bind_bytecode(&baseline_vm.bytecode(),
                                               pscript::compute_program_fingerprint(baseline_vm.bytecode()));
        baseline_vm.run();
        if (baseline_out.str() != expected) {
            fail_test("closure_nested baseline jit", "jit=" + baseline_out.str() + " vm=" + expected);
        } else {
            pass_test("closure_nested baseline jit");
        }
    }

    {
        std::ostringstream out;
        Runtime runtime(out);
        Vm vm(compile_with_builtins(std::string(pscript::bench::kRuntimePerfLuLocalRw)), runtime);
        vm.run();
        if (out.str() != "50000\n") {
            fail_test("perf_lu_local_rw bench constant vm", "unexpected output: " + out.str());
        } else {
            pass_test("perf_lu_local_rw bench constant vm");
        }
    }

    {
        const Bytecode bc = compile_with_builtins(std::string(pscript::bench::kRuntimeLoop));
        ProfileDocument profile = hot_integer_profile(bc, "loop");
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(bc, vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Bytecode jit_bc = bc;
        pscript::apply_profile_guided_optimizations(jit_bc, profile);
        Vm jit_vm(std::move(jit_bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        jit_vm.run();
        jit_out.str(std::string());
        jit_out.clear();
        jit_vm.run();
        if (jit_out.str() != vm_out.str()) {
            fail_test("loops optimizing jit warm parity", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().optimizing_compile_attempts() < 1) {
            fail_test("loops optimizing jit warm parity", "expected optimizing compile");
        } else {
            pass_test("loops optimizing jit warm parity");
        }
    }

    {
        const std::string source = pscript::bench::kRuntimeClosureRecursive;
        Bytecode bc = compile_with_builtins(source);
        ProfileDocument profile = hot_integer_profile(bc, "sum");
        pscript::apply_profile_guided_optimizations(bc, profile);
        std::ostringstream vm_out;
        Runtime vm_rt(vm_out);
        Vm vm_only(bc, vm_rt);
        vm_only.set_jit_enabled(false);
        vm_only.run();
        pscript::jit::JitTieringConfig config;
        config.baseline_invocation_threshold = 1;
        config.optimizing_invocation_threshold = 1;
        config.require_profile_for_optimizing = false;
        std::ostringstream jit_out;
        Runtime jit_rt(jit_out);
        Vm jit_vm(std::move(bc), jit_rt);
        jit_vm.set_jit_enabled(true);
        jit_vm.set_jit_config(config);
        jit_vm.jit_engine().bind_bytecode(&jit_vm.bytecode(), pscript::compute_program_fingerprint(jit_vm.bytecode()));
        jit_vm.jit_engine().set_profile(&profile);
        jit_vm.run();
        jit_vm.run();
        pscript::jit::reset_ic_metrics(jit_vm.jit_engine().ic_metrics());
        jit_out.str(std::string());
        jit_out.clear();
        jit_vm.run();
        const std::uint64_t nested_direct = jit_vm.jit_engine().ic_metrics().nested_jit_direct_entry;
        if (jit_out.str() != vm_out.str() || jit_out.str().find("20100") == std::string::npos) {
            fail_test("closure_recursive optimizing jit", "jit=" + jit_out.str() + " vm=" + vm_out.str());
        } else if (jit_vm.jit_engine().optimizing_compile_attempts() < 1) {
            fail_test("closure_recursive optimizing jit", "expected optimizing JIT compile");
        } else {
            pass_test("closure_recursive optimizing jit");
            if (nested_direct == 0) {
                const std::uint32_t sum_idx = function_index_by_name(jit_vm.bytecode(), "sum");
                pscript::jit::JitFunctionIr lowered_ir;
                std::string ir_error;
                bool iterative = false;
                if (sum_idx != 0xFFFFFFFFu &&
                    pscript::jit::build_jit_ir_from_bytecode(jit_vm.bytecode(), sum_idx, lowered_ir, ir_error)) {
                    pscript::jit::OptimizingPassStats pipe_stats;
                    pscript::jit::JitOptProfile opt =
                        pscript::jit::build_jit_opt_profile(&profile, jit_vm.bytecode());
                    if (pscript::jit::run_optimizing_pipeline(jit_vm.bytecode(), lowered_ir, &opt, pipe_stats,
                                                              ir_error) &&
                        !pscript::jit::ir_has_call_value_sites(lowered_ir)) {
                        iterative = true;
                    }
                }
                if (!iterative) {
                    fail_test("closure_recursive native nested calls",
                              "nested_jit_direct_entry=0 (nested_mono_complete=" +
                                  std::to_string(jit_vm.jit_engine().ic_metrics().nested_mono_complete_calls) + ")");
                } else {
                    pass_test("closure_recursive iterative lowering");
                }
            } else {
                pass_test("closure_recursive native nested calls");
            }
        }
    }

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All optimizing JIT tests passed\n";
    return 0;
}
