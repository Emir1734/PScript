#pragma once

#include "jit/jit_ir.h"

#include <cstdint>
#include <vector>

namespace pscript::jit {

struct JitExecState;

enum class JitGuardKind : std::uint8_t {
    IntegerType = 1,
    IntegerBinary = 2,
    ValueKind = 3,
};

struct JitDeoptSite {
    std::uint32_t ir_insn_index = 0;
    std::uint32_t bytecode_ip = 0;
    std::uint32_t resume_bytecode_ip = 0;
    JitGuardKind guard_kind = JitGuardKind::IntegerType;
    std::uint32_t guarded_register = 0;
    std::uint32_t inline_depth = 0;
    std::uint32_t inline_function_index = 0xFFFFFFFFu;
    std::uint32_t caller_function_index = 0xFFFFFFFFu;
};

struct JitInlineDeoptFrame {
    std::uint32_t function_index = 0;
    std::uint32_t resume_bytecode_ip = 0;
    std::uint32_t local_count = 0;
};

struct JitDeoptContext {
    std::vector<JitDeoptSite> sites;
    std::vector<JitInlineDeoptFrame> inline_frames;
};

struct JitOsrEntry {
    std::uint32_t loop_bytecode_ip = 0;
    std::uint32_t ir_insn_index = 0;
};

struct JitCompiledMetadata {
    JitDeoptContext deopt;
    std::vector<JitOsrEntry> osr_entries;
    std::uint32_t optimization_generation = 1;
    bool invalidated = false;
    std::uint64_t assumption_mask = 0;
};

enum class JitExecuteResult : std::uint8_t {
    Completed,
    Fallback,
    Deoptimized,
};

void build_deopt_metadata(const JitFunctionIr& ir, JitCompiledMetadata& metadata);

void jit_request_deopt(JitExecState* state, std::uint32_t site_id);
void jit_request_deopt_at_ir_index(JitExecState* state, std::uint32_t ir_insn_index);

void assign_deopt_site_ids(JitFunctionIr& ir, const JitCompiledMetadata& metadata);

}  // namespace pscript::jit
