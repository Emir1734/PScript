#include "compiler/generic.h"

#include "compiler/bytecode.h"
#include "compiler/compiler_error.h"
#include "compiler/type_compat.h"
#include "compiler/type_parse.h"
#include "parser/ast.h"

#include <sstream>

namespace pscript {

namespace {

std::vector<std::string> split_constraint_spec(const std::string& spec) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < spec.size()) {
        const std::size_t pos = spec.find(" & ", start);
        if (pos == std::string::npos) {
            parts.push_back(spec.substr(start));
            break;
        }
        parts.push_back(spec.substr(start, pos - start));
        start = pos + 3;
    }
    if (parts.empty() && !spec.empty()) {
        parts.push_back(spec);
    }
    return parts;
}

}  // namespace

bool validate_generic_constraints(const std::vector<std::string>& generic_param_names,
                                  const std::vector<std::string>& generic_param_constraints,
                                  const std::unordered_map<std::string, TypeDescriptor>& subst,
                                  const TypeConstraintPredicate& satisfies_constraint) {
    for (std::size_t i = 0; i < generic_param_names.size(); ++i) {
        if (i >= generic_param_constraints.size() || generic_param_constraints[i].empty()) {
            continue;
        }
        const auto found = subst.find(generic_param_names[i]);
        if (found == subst.end()) {
            continue;
        }
        const std::vector<std::string> parts = split_constraint_spec(generic_param_constraints[i]);
        for (const std::string& part : parts) {
            if (!satisfies_constraint(found->second, part)) {
                return false;
            }
        }
    }
    return true;
}

bool substitute_type_descriptor(TypeDescriptor& out, const TypeDescriptor& in,
                              const std::unordered_map<std::string, TypeDescriptor>& subst) {
    out = in;
    if (in.kind == StaticType::TypeParam && in.type_param_name) {
        const auto found = subst.find(*in.type_param_name);
        if (found == subst.end()) {
            return false;
        }
        out = found->second;
        return true;
    }
    for (TypeDescriptor& arg : out.type_args) {
        TypeDescriptor replaced;
        if (!substitute_type_descriptor(replaced, arg, subst)) {
            return false;
        }
        arg = std::move(replaced);
    }
    return true;
}

bool unify_generic_types(const TypeDescriptor& param_type, const TypeDescriptor& arg_type,
                         std::unordered_map<std::string, TypeDescriptor>& subst) {
    if (param_type.kind == StaticType::TypeParam && param_type.type_param_name) {
        const auto found = subst.find(*param_type.type_param_name);
        if (found == subst.end()) {
            subst[*param_type.type_param_name] = arg_type;
            return true;
        }
        return types_exactly_equal(found->second, arg_type);
    }
    if (param_type.kind != arg_type.kind) {
        if (param_type.kind == StaticType::Float && arg_type.kind == StaticType::Integer) {
            return true;
        }
        return false;
    }
    if (param_type.kind == StaticType::Struct && param_type.struct_index != arg_type.struct_index) {
        return false;
    }
    if (param_type.kind == StaticType::Class && param_type.class_index != arg_type.class_index) {
        return false;
    }
    if (param_type.kind == StaticType::Enum && param_type.enum_index != arg_type.enum_index) {
        return false;
    }
    if (param_type.type_args.size() != arg_type.type_args.size()) {
        return false;
    }
    for (std::size_t i = 0; i < param_type.type_args.size(); ++i) {
        if (!unify_generic_types(param_type.type_args[i], arg_type.type_args[i], subst)) {
            return false;
        }
    }
    return true;
}

bool apply_explicit_type_arguments(const std::vector<std::string>& generic_param_names,
                                   const std::vector<std::string>& type_argument_specs, const Location& loc,
                                   const Bytecode& bytecode, const std::vector<std::string>* active_type_params,
                                   std::unordered_map<std::string, TypeDescriptor>& subst) {
    if (type_argument_specs.size() != generic_param_names.size()) {
        return false;
    }
    for (std::size_t i = 0; i < generic_param_names.size(); ++i) {
        subst[generic_param_names[i]] =
            parse_type_spec(type_argument_specs[i], loc, bytecode, active_type_params);
    }
    return true;
}

static int assignability_score(const TypeDescriptor& target, const TypeDescriptor& source) {
    const AssignabilityResult result = is_assignable(target, source, nullptr);
    if (!result.ok) {
        return -1;
    }
    if (types_exactly_equal(target, source)) {
        return 100;
    }
    return 50;
}

static std::uint32_t effective_min_arity(const FunctionInfo& fn, const std::vector<Parameter>& params) {
    if (fn.min_arity != 0 || fn.variadic) {
        return fn.min_arity;
    }
    std::uint32_t min = 0;
    for (const Parameter& param : params) {
        if (param.is_variadic || param.default_value) {
            break;
        }
        ++min;
    }
    return min;
}

static std::optional<OverloadCandidate> try_match_function(
    std::uint32_t function_index, const FunctionInfo& fn, const std::vector<Parameter>& params,
    const std::vector<TypeDescriptor>& arg_types, const std::vector<std::string>& explicit_type_args,
    const Bytecode& bytecode, const Bytecode& function_bytecode,
    const std::vector<std::string>* active_type_params, std::uint32_t provided_argc) {
    const std::uint32_t min_arity = effective_min_arity(fn, params);
    if (fn.variadic) {
        if (provided_argc < min_arity) {
            return std::nullopt;
        }
    } else if (provided_argc < min_arity || provided_argc > fn.arity) {
        return std::nullopt;
    }

    std::unordered_map<std::string, TypeDescriptor> subst;
    if (!fn.generic_type_params.empty() && !explicit_type_args.empty()) {
        if (!apply_explicit_type_arguments(fn.generic_type_params, explicit_type_args, Location{1, 1}, function_bytecode,
                                           active_type_params, subst)) {
            return std::nullopt;
        }
    }

    int total_score = 0;
    std::size_t arg_index = 0;
    for (std::size_t param_index = 0; param_index < params.size(); ++param_index) {
        const Parameter& param = params[param_index];
        if (param.is_variadic) {
            TypeDescriptor element_type;
            element_type.kind = StaticType::Any;
            if (param.type_name && !param.type_name->empty()) {
                element_type = parse_type_spec(*param.type_name, Location{1, 1}, bytecode, &fn.generic_type_params);
                TypeDescriptor substituted;
                if (!substitute_type_descriptor(substituted, element_type, subst)) {
                    if (!subst.empty()) {
                        return std::nullopt;
                    }
                } else {
                    element_type = substituted;
                }
            }
            for (std::size_t v = arg_index; v < arg_types.size(); ++v) {
                if (!fn.generic_type_params.empty() && subst.empty()) {
                    if (!unify_generic_types(element_type, arg_types[v], subst)) {
                        return std::nullopt;
                    }
                    TypeDescriptor concrete;
                    if (!substitute_type_descriptor(concrete, element_type, subst)) {
                        return std::nullopt;
                    }
                    const int score = assignability_score(concrete, arg_types[v]);
                    if (score < 0) {
                        return std::nullopt;
                    }
                    total_score += score;
                } else {
                    const int score = assignability_score(element_type, arg_types[v]);
                    if (score < 0) {
                        return std::nullopt;
                    }
                    total_score += score;
                }
            }
            return OverloadCandidate{function_index, total_score, subst};
        }
        if (arg_index >= arg_types.size()) {
            if (param.default_value) {
                continue;
            }
            return std::nullopt;
        }
        if (!param.type_name || param.type_name->empty()) {
            if (!fn.generic_type_params.empty() && subst.empty()) {
                return std::nullopt;
            }
            ++arg_index;
            continue;
        }
        TypeDescriptor param_type = parse_type_spec(*param.type_name, Location{1, 1}, bytecode, &fn.generic_type_params);
        if (!fn.generic_type_params.empty() && subst.empty() && !explicit_type_args.empty()) {
            return std::nullopt;
        }
        if (!fn.generic_type_params.empty() && subst.empty()) {
            if (!unify_generic_types(param_type, arg_types[arg_index], subst)) {
                return std::nullopt;
            }
        }
        TypeDescriptor concrete_param;
        if (!substitute_type_descriptor(concrete_param, param_type, subst)) {
            concrete_param = param_type;
        }
        const int score = assignability_score(concrete_param, arg_types[arg_index]);
        if (score < 0) {
            return std::nullopt;
        }
        total_score += score;
        ++arg_index;
    }
    if (arg_index != arg_types.size()) {
        return std::nullopt;
    }
    return OverloadCandidate{function_index, total_score, subst};
}

std::optional<OverloadCandidate> pick_best_overload(
    const std::vector<std::uint32_t>& candidates,
    const std::unordered_map<std::uint32_t, const std::vector<Parameter>*>& function_parameters,
    const std::vector<TypeDescriptor>& arg_types, const std::vector<std::string>& explicit_type_args,
    const Bytecode& bytecode, const std::vector<std::string>* active_type_params, bool /*allow_defaults*/,
    std::uint32_t provided_argc, const Bytecode* function_bytecode) {
    const Bytecode& fn_bc = function_bytecode ? *function_bytecode : bytecode;
    std::optional<OverloadCandidate> best;
    for (const std::uint32_t index : candidates) {
        if (index >= fn_bc.functions.size()) {
            continue;
        }
        const FunctionInfo& fn = fn_bc.functions[index];
        const auto params_it = function_parameters.find(index);
        if (params_it == function_parameters.end() || params_it->second == nullptr) {
            continue;
        }
        const auto match = try_match_function(index, fn, *params_it->second, arg_types, explicit_type_args, bytecode,
                                            fn_bc, active_type_params, provided_argc);
        if (!match) {
            continue;
        }
        if (!best || match->score > best->score) {
            best = *match;
        } else if (best && match->score == best->score) {
            return std::nullopt;
        }
    }
    return best;
}

bool function_signatures_conflict(const FunctionInfo& left, const FunctionInfo& right) {
    if (left.arity != right.arity) {
        return false;
    }
    if (left.generic_type_params != right.generic_type_params) {
        return false;
    }
    if (left.parameter_type_names.size() != right.parameter_type_names.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.parameter_type_names.size(); ++i) {
        if (left.parameter_type_names[i] != right.parameter_type_names[i]) {
            return false;
        }
    }
    return true;
}

}  // namespace pscript
