#pragma once

#include "jit/jit_ir.h"
#include "jit/jit_opt_profile.h"

#include <string>

namespace pscript::jit {

struct OptimizingPassStats {
    std::size_t constant_folds = 0;
    std::size_t dead_removed = 0;
    std::size_t cse_hits = 0;
    std::size_t specialized_ops = 0;
    std::size_t inlined_calls = 0;
    std::size_t licm_moves = 0;
    std::size_t guard_dedup = 0;
    std::size_t escape_scalarized = 0;
    std::size_t allocs_eliminated = 0;
};

void run_escape_and_scalar_replacement(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats);

void apply_stable_call_value_lowering(const Bytecode& bytecode, JitFunctionIr& ir);

void apply_baseline_loop_integer_ops(const Bytecode& bytecode, JitFunctionIr& ir);

void buffer_integer_locals(const Bytecode& bytecode, JitFunctionIr& ir, OptimizingPassStats& stats);

bool try_lower_accumulating_self_recursion(const Bytecode& bytecode, JitFunctionIr& ir);

bool run_optimizing_pipeline(const Bytecode& bytecode, JitFunctionIr& ir, const JitOptProfile* profile,
                             OptimizingPassStats& stats, std::string& error);

bool finish_stable_direct_callvalue_loop_compile(const Bytecode& bytecode, JitFunctionIr& ir,
                                                 const JitOptProfile* profile, OptimizingPassStats& stats,
                                                 std::string& error);

bool ir_has_call_value_sites(const JitFunctionIr& ir);

bool ir_has_call_sites(const JitFunctionIr& ir);

bool ir_may_use_baseline_numeric_loop_fast_path(const Bytecode& bytecode, const JitFunctionIr& ir);

bool bytecode_function_returns_single_closure(const Bytecode& bytecode, std::uint32_t function_index,
                                              std::uint32_t& inner_function_index);

void jit_common_subexpression_elimination(JitFunctionIr& ir, OptimizingPassStats& stats);

}  // namespace pscript::jit
