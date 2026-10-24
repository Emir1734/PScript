#include "jit/jit_deopt.h"

#include "jit/jit_exec_state.h"

namespace pscript::jit {
namespace {

std::uint32_t next_bytecode_ip(const JitFunctionIr& ir, std::uint32_t insn_index) {
    if (insn_index + 1 < ir.instructions.size()) {
        return ir.instructions[insn_index + 1].bytecode_ip;
    }
    if (!ir.instructions.empty()) {
        return ir.instructions.back().bytecode_ip;
    }
    return 0;
}

}  // namespace

void build_deopt_metadata(const JitFunctionIr& ir, JitCompiledMetadata& metadata) {
    metadata.deopt.sites.clear();
    metadata.osr_entries.clear();
    for (std::size_t i = 0; i < ir.instructions.size(); ++i) {
        const JitIrInstruction& insn = ir.instructions[i];
        if (insn.opcode == JitIrOpcode::GuardInteger) {
            JitDeoptSite site;
            site.ir_insn_index = static_cast<std::uint32_t>(i);
            site.bytecode_ip = insn.bytecode_ip;
            site.resume_bytecode_ip = next_bytecode_ip(ir, static_cast<std::uint32_t>(i));
            site.guard_kind = JitGuardKind::IntegerType;
            site.guarded_register = insn.a;
            metadata.deopt.sites.push_back(site);
        } else if (insn.opcode == JitIrOpcode::GuardValueKind || insn.opcode == JitIrOpcode::GuardFieldObject) {
            JitDeoptSite site;
            site.ir_insn_index = static_cast<std::uint32_t>(i);
            site.bytecode_ip = insn.bytecode_ip;
            site.resume_bytecode_ip = next_bytecode_ip(ir, static_cast<std::uint32_t>(i));
            site.guard_kind = JitGuardKind::ValueKind;
            site.guarded_register = insn.a;
            metadata.deopt.sites.push_back(site);
        } else if (insn.opcode == JitIrOpcode::IntAdd || insn.opcode == JitIrOpcode::IntSub ||
                   insn.opcode == JitIrOpcode::IntMul || insn.opcode == JitIrOpcode::IntMod ||
                   insn.opcode == JitIrOpcode::IntDiv || insn.opcode == JitIrOpcode::IntLess ||
                   insn.opcode == JitIrOpcode::IntEqual || insn.opcode == JitIrOpcode::FloatAdd ||
                   insn.opcode == JitIrOpcode::FloatSub || insn.opcode == JitIrOpcode::FloatMul ||
                   insn.opcode == JitIrOpcode::FloatDiv || insn.opcode == JitIrOpcode::FloatLess ||
                   insn.opcode == JitIrOpcode::FloatEqual) {
            if ((insn.opcode == JitIrOpcode::IntAdd || insn.opcode == JitIrOpcode::IntSub ||
                 insn.opcode == JitIrOpcode::IntMul || insn.opcode == JitIrOpcode::IntMod ||
                 insn.opcode == JitIrOpcode::IntDiv || insn.opcode == JitIrOpcode::IntLess ||
                 insn.opcode == JitIrOpcode::IntEqual) &&
                ir.buffered_integer_locals &&
                (ir.profile_stable_integer_ops || ir.baseline_numeric_loop_fast_path)) {
                continue;
            }
            JitDeoptSite site;
            site.ir_insn_index = static_cast<std::uint32_t>(i);
            site.bytecode_ip = insn.bytecode_ip;
            site.resume_bytecode_ip = insn.bytecode_ip;
            site.guard_kind = JitGuardKind::IntegerBinary;
            metadata.deopt.sites.push_back(site);
        }
    }
    for (const JitCfgBlock& block : ir.cfg) {
        if (block.loop_header && block.first_insn < ir.instructions.size()) {
            JitOsrEntry entry;
            entry.loop_bytecode_ip = ir.instructions[block.first_insn].bytecode_ip;
            entry.ir_insn_index = block.first_insn;
            metadata.osr_entries.push_back(entry);
        }
    }
}

void assign_deopt_site_ids(JitFunctionIr& ir, const JitCompiledMetadata& metadata) {
    for (std::size_t i = 0; i < metadata.deopt.sites.size(); ++i) {
        const std::uint32_t ir_index = metadata.deopt.sites[i].ir_insn_index;
        if (ir_index < ir.instructions.size()) {
            ir.instructions[ir_index].deopt_site_id = static_cast<std::uint32_t>(i);
        }
    }
}

void jit_request_deopt(JitExecState* state, std::uint32_t site_id) {
    if (!state) {
        return;
    }
    state->deopt_requested = true;
    state->fallback = false;
    state->deopt_site_id = site_id;
    if (state->deopt_context && site_id < state->deopt_context->sites.size()) {
        const JitDeoptSite& site = state->deopt_context->sites[site_id];
        state->deopt_resume_bytecode_ip = site.resume_bytecode_ip;
        state->deopt_ir_insn_index = site.ir_insn_index;
    }
}

void jit_request_deopt_at_ir_index(JitExecState* state, std::uint32_t ir_insn_index) {
    if (!state || !state->deopt_context) {
        if (state) {
            state->fallback = true;
        }
        return;
    }
    for (std::uint32_t i = 0; i < state->deopt_context->sites.size(); ++i) {
        if (state->deopt_context->sites[i].ir_insn_index == ir_insn_index) {
            jit_request_deopt(state, i);
            return;
        }
    }
    state->fallback = true;
}

}  // namespace pscript::jit
