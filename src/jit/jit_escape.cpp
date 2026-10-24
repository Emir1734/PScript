#include "jit/jit_opt.h"

#include <unordered_map>
#include <vector>

namespace pscript::jit {
namespace {

struct StructSite {
    std::uint32_t struct_reg = 0;
    bool escaped = false;
    std::unordered_map<std::uint32_t, std::uint32_t> field_value_regs;
};

bool register_used_as_call_arg(const JitIrInstruction& insn, std::uint32_t reg) {
    if (insn.opcode == JitIrOpcode::Call) {
        return insn.b <= reg;
    }
    if (insn.opcode == JitIrOpcode::CallValue) {
        return insn.b == reg || (insn.b + 1 <= reg && reg < insn.b + 1 + insn.c);
    }
    return false;
}

bool reg_holds_struct(std::uint32_t reg, std::uint32_t canon,
                      const std::unordered_map<std::uint32_t, std::uint32_t>& reg_to_canonical) {
    const auto it = reg_to_canonical.find(reg);
    return it != reg_to_canonical.end() && it->second == canon;
}

bool insn_escapes_struct(const JitIrInstruction& insn, std::uint32_t canon,
                         const std::unordered_map<std::uint32_t, std::uint32_t>& reg_to_canonical) {
    switch (insn.opcode) {
        case JitIrOpcode::Return:
            return reg_holds_struct(insn.a, canon, reg_to_canonical);
        case JitIrOpcode::MoveReg:
            return false;
        case JitIrOpcode::StoreLocal:
            return false;
        case JitIrOpcode::LoadLocal:
            return reg_holds_struct(insn.a, canon, reg_to_canonical);
        case JitIrOpcode::Equal:
        case JitIrOpcode::NotEqual:
        case JitIrOpcode::IntEqual:
            return reg_holds_struct(insn.b, canon, reg_to_canonical) ||
                   reg_holds_struct(insn.c, canon, reg_to_canonical);
        case JitIrOpcode::GetIndex:
            return reg_holds_struct(insn.b, canon, reg_to_canonical);
        case JitIrOpcode::Call:
        case JitIrOpcode::CallValue:
            return register_used_as_call_arg(insn, canon) ||
                   reg_holds_struct(insn.b, canon, reg_to_canonical);
        default:
            return false;
    }
}

}  // namespace

void run_escape_and_scalar_replacement(const Bytecode& /*bytecode*/, JitFunctionIr& ir,
                                       OptimizingPassStats& stats) {
    std::unordered_map<std::uint32_t, StructSite> sites;
    std::unordered_map<std::uint32_t, std::uint32_t> reg_to_canonical;
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::MakeStruct) {
            StructSite site;
            site.struct_reg = insn.a;
            sites[insn.a] = site;
            reg_to_canonical[insn.a] = insn.a;
        }
    }
    if (sites.empty()) {
        return;
    }

    std::unordered_map<std::uint32_t, std::uint32_t> local_to_canonical;
    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode == JitIrOpcode::StoreLocal) {
            const auto reg_it = reg_to_canonical.find(insn.b);
            if (reg_it != reg_to_canonical.end()) {
                local_to_canonical[insn.a] = reg_it->second;
            }
        }
        if (insn.opcode == JitIrOpcode::LoadLocal) {
            const auto local_it = local_to_canonical.find(insn.b);
            if (local_it != local_to_canonical.end()) {
                reg_to_canonical[insn.a] = local_it->second;
            }
        }
        if (insn.opcode == JitIrOpcode::MoveReg) {
            const auto reg_it = reg_to_canonical.find(insn.b);
            if (reg_it != reg_to_canonical.end()) {
                reg_to_canonical[insn.a] = reg_it->second;
            }
        }
    }

    for (const JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::SetField) {
            continue;
        }
        const auto canon_it = reg_to_canonical.find(insn.a);
        if (canon_it == reg_to_canonical.end()) {
            continue;
        }
        StructSite& site = sites[canon_it->second];
        if (site.escaped) {
            continue;
        }
        site.field_value_regs[insn.b] = insn.c;
    }

    for (JitIrInstruction& insn : ir.instructions) {
        if (insn.opcode != JitIrOpcode::GetField) {
            continue;
        }
        const auto canon_it = reg_to_canonical.find(insn.b);
        if (canon_it == reg_to_canonical.end()) {
            continue;
        }
        StructSite& site = sites[canon_it->second];
        if (site.escaped) {
            continue;
        }
        const auto field_it = site.field_value_regs.find(insn.c);
        if (field_it == site.field_value_regs.end()) {
            site.escaped = true;
            continue;
        }
        insn.opcode = JitIrOpcode::MoveReg;
        insn.b = field_it->second;
        ++stats.escape_scalarized;
    }

    for (const JitIrInstruction& insn : ir.instructions) {
        for (auto& [canon, site] : sites) {
            if (site.escaped) {
                continue;
            }
            if (insn.opcode == JitIrOpcode::LoadLocal && reg_holds_struct(insn.a, canon, reg_to_canonical)) {
                const auto local_it = local_to_canonical.find(insn.b);
                if (local_it == local_to_canonical.end() || local_it->second != canon) {
                    site.escaped = true;
                }
            } else if (insn_escapes_struct(insn, canon, reg_to_canonical)) {
                site.escaped = true;
            }
        }
    }
}

}  // namespace pscript::jit
