#pragma once

#include "compiler/bytecode.h"
#include "compiler/types.h"
#include "parser/ast.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pscript {

bool substitute_type_descriptor(TypeDescriptor& out, const TypeDescriptor& in,
                              const std::unordered_map<std::string, TypeDescriptor>& subst);

bool unify_generic_types(const TypeDescriptor& param_type, const TypeDescriptor& arg_type,
                         std::unordered_map<std::string, TypeDescriptor>& subst);

bool apply_explicit_type_arguments(const std::vector<std::string>& generic_param_names,
                                   const std::vector<std::string>& type_argument_specs,
                                   const Location& loc, const Bytecode& bytecode,
                                   const std::vector<std::string>* active_type_params,
                                   std::unordered_map<std::string, TypeDescriptor>& subst);

struct OverloadCandidate {
    std::uint32_t function_index = 0;
    int score = 0;
    std::unordered_map<std::string, TypeDescriptor> generic_subst;
};

std::optional<OverloadCandidate> pick_best_overload(
    const std::vector<std::uint32_t>& candidates,
    const std::unordered_map<std::uint32_t, const std::vector<Parameter>*>& function_parameters,
    const std::vector<TypeDescriptor>& arg_types, const std::vector<std::string>& explicit_type_args,
    const Bytecode& bytecode, const std::vector<std::string>* active_type_params, bool allow_defaults,
    std::uint32_t provided_argc, const Bytecode* function_bytecode = nullptr);

bool function_signatures_conflict(const FunctionInfo& left, const FunctionInfo& right);

using TypeConstraintPredicate = std::function<bool(const TypeDescriptor&, const std::string& constraint_name)>;

bool validate_generic_constraints(const std::vector<std::string>& generic_param_names,
                                  const std::vector<std::string>& generic_param_constraints,
                                  const std::unordered_map<std::string, TypeDescriptor>& subst,
                                  const TypeConstraintPredicate& satisfies_constraint);

}  // namespace pscript
