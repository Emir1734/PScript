#include "compiler/compiler.h"

#include "compiler/generic.h"
#include "compiler/type_compat.h"
#include "compiler/type_parse.h"
#include "parser/ast.h"

#include <cctype>

namespace pscript {

namespace {

std::string type_spec_base_name(const std::string& spec) {
    const std::size_t lt = spec.find('<');
    if (lt == std::string::npos) {
        return spec;
    }
    return spec.substr(0, lt);
}

std::vector<std::string> type_spec_type_arguments(const std::string& spec) {
    const std::size_t lt = spec.find('<');
    if (lt == std::string::npos) {
        return {};
    }
    const std::size_t gt = spec.rfind('>');
    if (gt == std::string::npos || gt <= lt) {
        return {};
    }
    std::vector<std::string> args;
    std::string inner = spec.substr(lt + 1, gt - lt - 1);
    std::size_t start = 0;
    int depth = 0;
    for (std::size_t i = 0; i < inner.size(); ++i) {
        const char ch = inner[i];
        if (ch == '<') {
            ++depth;
        } else if (ch == '>') {
            --depth;
        } else if (ch == ',' && depth == 0) {
            args.push_back(inner.substr(start, i - start));
            start = i + 1;
        }
    }
    args.push_back(inner.substr(start));
    for (std::string& arg : args) {
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.front()))) {
            arg.erase(arg.begin());
        }
        while (!arg.empty() && std::isspace(static_cast<unsigned char>(arg.back()))) {
            arg.pop_back();
        }
    }
    return args;
}

bool method_signature_matches(const FunctionInfo& impl, const InterfaceMethodInfo& required,
                              const std::unordered_map<std::string, TypeDescriptor>& subst,
                              const Bytecode& bytecode, const std::vector<std::string>* active_type_params) {
    if (impl.is_async != required.is_async) {
        return false;
    }
    if (impl.parameter_type_names.size() != required.parameter_type_names.size()) {
        return false;
    }
    for (std::size_t i = 0; i < required.parameter_type_names.size(); ++i) {
        if (!required.parameter_type_names[i] || required.parameter_type_names[i]->empty()) {
            continue;
        }
        if (!impl.parameter_type_names[i] || impl.parameter_type_names[i]->empty()) {
            return false;
        }
        TypeDescriptor expected = parse_type_spec(*required.parameter_type_names[i], Location{1, 1}, bytecode,
                                                  active_type_params);
        TypeDescriptor actual = parse_type_spec(*impl.parameter_type_names[i], Location{1, 1}, bytecode,
                                                active_type_params);
        TypeDescriptor concrete_expected;
        TypeDescriptor concrete_actual;
        if (!substitute_type_descriptor(concrete_expected, expected, subst)) {
            concrete_expected = expected;
        }
        if (!substitute_type_descriptor(concrete_actual, actual, subst)) {
            concrete_actual = actual;
        }
        if (!types_exactly_equal(concrete_expected, concrete_actual)) {
            return false;
        }
    }
    if (required.return_type_name && !required.return_type_name->empty()) {
        if (!impl.return_type_name || impl.return_type_name->empty()) {
            return false;
        }
        TypeDescriptor expected = parse_type_spec(*required.return_type_name, Location{1, 1}, bytecode, active_type_params);
        TypeDescriptor actual = parse_type_spec(*impl.return_type_name, Location{1, 1}, bytecode, active_type_params);
        TypeDescriptor concrete_expected;
        TypeDescriptor concrete_actual;
        if (!substitute_type_descriptor(concrete_expected, expected, subst)) {
            concrete_expected = expected;
        }
        if (!substitute_type_descriptor(concrete_actual, actual, subst)) {
            concrete_actual = actual;
        }
        if (!types_exactly_equal(concrete_expected, concrete_actual)) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<AnnotationInfo> Compiler::copy_annotations(const std::vector<Annotation>& annotations) const {
    std::vector<AnnotationInfo> out;
    for (const Annotation& annotation : annotations) {
        AnnotationInfo info;
        info.name = annotation.name;
        info.loc.line = static_cast<std::uint32_t>(annotation.loc.line);
        info.loc.column = static_cast<std::uint32_t>(annotation.loc.column);
        for (const AnnotationArg& arg : annotation.args) {
            std::string value_text;
            if (arg.value) {
                if (const auto* literal = dynamic_cast<const LiteralExpr*>(arg.value.get())) {
                    value_text = literal->lexeme;
                }
            }
            info.args.emplace_back(arg.key, value_text);
        }
        out.push_back(std::move(info));
    }
    return out;
}

void Compiler::declare_interface(const InterfaceStmt& stmt) {
    if (!in_function_) {
        check_module_level_access(stmt.access, stmt.loc);
    }
    if (current_scope_defines(stmt.name)) {
        fail("duplicate declaration of '" + stmt.name + "'", stmt.loc);
    }
    InterfaceInfo info;
    info.name = stmt.name;
    info.extends = stmt.extends;
    info.annotations = copy_annotations(stmt.annotations);
    for (const GenericTypeParam& param : stmt.type_params) {
        info.generic_type_params.push_back(param.name);
    }
    for (const InterfaceMethodSig& method : stmt.methods) {
        InterfaceMethodInfo method_info;
        method_info.name = method.name;
        method_info.is_async = method.is_async;
        method_info.return_type_name = method.return_type_name;
        for (const Parameter& param : method.parameters) {
            method_info.parameter_type_names.push_back(param.type_name);
        }
        info.methods.push_back(std::move(method_info));
    }
    for (const InterfacePropertySig& property : stmt.properties) {
        InterfacePropertyInfo property_info;
        property_info.name = property.name;
        property_info.type_name = property.type_name;
        property_info.has_getter = property.has_getter;
        property_info.has_setter = property.has_setter;
        info.properties.push_back(std::move(property_info));
    }
    const auto index = static_cast<std::uint32_t>(bytecode_.interfaces.size());
    bytecode_.interfaces.push_back(std::move(info));

    Symbol symbol;
    symbol.name = stmt.name;
    symbol.kind = SymbolKind::Interface;
    symbol.access = stmt.access;
    symbol.interface_index = index;
    symbol.owner_function = current_function();
    scope_stack_.back().symbols.emplace(stmt.name, std::move(symbol));
}

std::uint32_t Compiler::find_interface_by_name(const std::string& name) const {
    for (std::size_t i = 0; i < bytecode_.interfaces.size(); ++i) {
        if (bytecode_.interfaces[i].name == name) {
            return static_cast<std::uint32_t>(i);
        }
    }
    return kInvalidIndex;
}

bool Compiler::class_implements_interface(std::uint32_t class_index, std::uint32_t interface_index) const {
    if (class_index >= bytecode_.classes.size() || interface_index >= bytecode_.interfaces.size()) {
        return false;
    }
    const ClassInfo& info = bytecode_.classes[class_index];
    for (const std::uint32_t iface : info.interface_indices) {
        if (iface == interface_index) {
            return true;
        }
    }
    if (info.parent_index != kInvalidIndex) {
        return class_implements_interface(info.parent_index, interface_index);
    }
    return false;
}

bool Compiler::type_satisfies_constraint(const TypeDescriptor& type, const std::string& constraint_spec,
                                           const Location& loc) const {
    const std::string base = type_spec_base_name(constraint_spec);
    const std::uint32_t interface_index = find_interface_by_name(base);
    if (interface_index != kInvalidIndex) {
        if (type.kind != StaticType::Class || !type.class_index) {
            return false;
        }
        return class_implements_interface(*type.class_index, interface_index);
    }
    TypeDescriptor constraint_type = parse_type_descriptor(constraint_spec, loc);
    const AssignabilityResult result = is_assignable(constraint_type, type, [this](std::uint32_t child, std::uint32_t parent) {
        return is_class_or_subclass(child, parent);
    });
    return result.ok;
}

void Compiler::resolve_class_bases(const ClassStmt& stmt, ClassInfo& info) {
    bool parent_set = false;
    for (std::size_t i = 0; i < stmt.base_type_specs.size(); ++i) {
        const std::string& spec = stmt.base_type_specs[i];
        const std::string base_name = type_spec_base_name(spec);
        const std::uint32_t interface_index = find_interface_by_name(base_name);
        if (interface_index != kInvalidIndex) {
            info.interface_indices.push_back(interface_index);
            continue;
        }
        const std::uint32_t class_index = find_class_by_name(base_name);
        if (class_index != kInvalidIndex) {
            if (parent_set) {
                fail("multiple inheritance is not supported", stmt.loc);
            }
            if (i != 0) {
                fail("parent class must be listed first in inheritance list", stmt.loc);
            }
            const std::uint32_t self_index = find_class_by_name(stmt.name);
            if (class_index == self_index) {
                fail("class cannot inherit from itself", stmt.loc);
            }
            info.parent_index = class_index;
            info.parent_type_argument_specs = type_spec_type_arguments(spec);
            parent_set = true;
            continue;
        }
        fail("unknown base type '" + base_name + "'", stmt.loc);
    }
    if (!stmt.parent_name && parent_set) {
        // parent_name legacy field kept in sync by parser when bases non-empty
    }
}

void Compiler::validate_class_interfaces(const ClassStmt& stmt, ClassInfo& info) {
    for (const std::uint32_t interface_index : info.interface_indices) {
        if (interface_index >= bytecode_.interfaces.size()) {
            continue;
        }
        const InterfaceInfo& iface = bytecode_.interfaces[interface_index];
        std::unordered_map<std::string, TypeDescriptor> subst;
        if (!iface.generic_type_params.empty()) {
            std::string iface_spec = iface.name;
            for (const std::string& spec : stmt.base_type_specs) {
                if (type_spec_base_name(spec) == iface.name) {
                    iface_spec = spec;
                    break;
                }
            }
            const std::vector<std::string> args = type_spec_type_arguments(iface_spec);
            if (args.size() != iface.generic_type_params.size()) {
                fail("interface '" + iface.name + "' expects " + std::to_string(iface.generic_type_params.size()) +
                         " type arguments",
                     stmt.loc);
            }
            for (std::size_t i = 0; i < iface.generic_type_params.size(); ++i) {
                subst[iface.generic_type_params[i]] = parse_type_descriptor(args[i], stmt.loc);
            }
        }
        const std::uint32_t class_index = find_class_by_name(stmt.name);
        for (const InterfaceMethodInfo& required : iface.methods) {
            const auto method = find_class_method(class_index, required.name);
            if (!method) {
                fail("class '" + stmt.name + "' does not implement interface method '" + required.name + "'",
                     stmt.loc);
            }
            const ClassInfo& owner = bytecode_.classes[method->class_index];
            const ClassMethodInfo& method_info = owner.methods[method->method_index];
            const FunctionInfo& impl =
                owner.module_index != 0xFFFFFFFFu
                    ? module_bytecode_for(owner.module_index, stmt.loc).functions[method_info.function_index]
                    : bytecode_.functions[method_info.function_index];
            if (!method_signature_matches(impl, required, subst, bytecode_, &iface.generic_type_params)) {
                fail("class '" + stmt.name + "' has incompatible implementation for interface method '" +
                         required.name + "'",
                     stmt.loc);
            }
        }
        for (const InterfacePropertyInfo& required : iface.properties) {
            bool found = false;
            for (const ClassPropertyInfo& prop : info.properties) {
                if (prop.name != required.name) {
                    continue;
                }
                found = true;
                if (required.has_getter && prop.getter_function_index == 0xFFFFFFFFu) {
                    fail("class '" + stmt.name + "' missing getter for interface property '" + required.name + "'",
                         stmt.loc);
                }
                if (required.type_name && prop.type_name) {
                    TypeDescriptor expected = parse_type_spec(*required.type_name, stmt.loc, bytecode_, &info.generic_type_params);
                    TypeDescriptor actual = parse_type_spec(*prop.type_name, stmt.loc, bytecode_, &info.generic_type_params);
                    TypeDescriptor concrete_expected;
                    TypeDescriptor concrete_actual;
                    if (!substitute_type_descriptor(concrete_expected, expected, subst)) {
                        concrete_expected = expected;
                    }
                    if (!substitute_type_descriptor(concrete_actual, actual, subst)) {
                        concrete_actual = actual;
                    }
                    if (!types_exactly_equal(concrete_expected, concrete_actual)) {
                        fail("class '" + stmt.name + "' has incompatible type for interface property '" +
                                 required.name + "'",
                             stmt.loc);
                    }
                }
                break;
            }
            if (!found) {
                fail("class '" + stmt.name + "' does not implement interface property '" + required.name + "'",
                     stmt.loc);
            }
        }
    }
}

const FunctionInfo& Compiler::function_info_for_symbol(const Symbol& symbol, const Location& loc) const {
    if (symbol.import_module_index != 0xFFFFFFFFu) {
        const Bytecode& module_bc = module_bytecode_for(symbol.import_module_index, loc);
        if (symbol.function_index >= module_bc.functions.size()) {
            fail("invalid imported function", loc);
        }
        return module_bc.functions[symbol.function_index];
    }
    if (symbol.function_index >= bytecode_.functions.size()) {
        fail("invalid function", loc);
    }
    return bytecode_.functions[symbol.function_index];
}

const std::vector<Parameter>& Compiler::parameters_for_function(std::uint32_t function_index,
                                                                 std::uint32_t import_module_index,
                                                                 const Location& loc) {
    if (import_module_index == 0xFFFFFFFFu) {
        const auto it = function_parameters_.find(function_index);
        if (it == function_parameters_.end() || it->second == nullptr) {
            fail("invalid function parameters", loc);
        }
        return *it->second;
    }
    const std::uint64_t key =
        (static_cast<std::uint64_t>(import_module_index) << 32) | static_cast<std::uint64_t>(function_index);
    const auto it = imported_function_parameters_.find(key);
    if (it != imported_function_parameters_.end()) {
        return it->second;
    }
    const Bytecode& module_bc = module_bytecode_for(import_module_index, loc);
    if (function_index >= module_bc.functions.size()) {
        fail("invalid imported function", loc);
    }
    const FunctionInfo& fn = module_bc.functions[function_index];
    std::vector<Parameter> params;
    params.reserve(fn.parameter_type_names.size());
    for (const std::optional<std::string>& type_name : fn.parameter_type_names) {
        Parameter param;
        param.type_name = type_name;
        params.push_back(std::move(param));
    }
    imported_function_parameters_[key] = std::move(params);
    return imported_function_parameters_[key];
}

void Compiler::compile_generic_type_call(Register dest, const CallExpr& call, const Symbol& symbol) {
    std::unordered_map<std::string, TypeDescriptor> subst;
    if (symbol.kind == SymbolKind::Struct) {
        const StructInfo& st = bytecode_.structs[symbol.struct_index];
        push_type_param_scope(st.generic_type_params);
        if (!call.type_arguments.empty()) {
            if (!apply_explicit_type_arguments(st.generic_type_params, call.type_arguments, call.loc, bytecode_,
                                               active_type_params(), subst)) {
                fail("invalid type arguments for '" + call.name + "'", call.loc);
            }
        } else if (!st.generic_type_params.empty()) {
            if (call.arguments.size() != st.fields.size()) {
                fail("'" + call.name + "' expects " + std::to_string(st.fields.size()) + " arguments", call.loc);
            }
            for (std::size_t i = 0; i < st.fields.size(); ++i) {
                if (!st.fields[i].type_name) {
                    continue;
                }
                TypeDescriptor field_type =
                    parse_type_spec(*st.fields[i].type_name, call.loc, bytecode_, active_type_params());
                const TypeDescriptor arg_type = infer_type_descriptor(*call.arguments[i]);
                if (!unify_generic_types(field_type, arg_type, subst)) {
                    fail("invalid argument for '" + call.name + "'", call.arguments[i]->loc);
                }
            }
        }
        if (!st.generic_type_params.empty() &&
            !validate_generic_constraints(st.generic_type_params, st.generic_type_param_constraints, subst,
                                          [this, &call](const TypeDescriptor& type, const std::string& constraint) {
                                              return type_satisfies_constraint(type, constraint, call.loc);
                                          })) {
            fail("type argument does not satisfy generic constraint for '" + call.name + "'", call.loc);
        }
        emit(Instruction{Opcode::MakeStruct, dest, symbol.struct_index, 0});
        for (std::size_t i = 0; i < call.arguments.size(); ++i) {
            const Register value = compile_expr(*call.arguments[i]);
            emit(Instruction{Opcode::SetField, dest, static_cast<std::uint32_t>(i),
                             static_cast<std::uint32_t>(value)});
        }
        pop_type_param_scope();
        return;
    }
    if (symbol.kind == SymbolKind::Class) {
        const ClassInfo& cls = bytecode_.classes[symbol.class_index];
        push_type_param_scope(cls.generic_type_params);
        if (!call.type_arguments.empty()) {
            if (!apply_explicit_type_arguments(cls.generic_type_params, call.type_arguments, call.loc, bytecode_,
                                               active_type_params(), subst)) {
                fail("invalid type arguments for '" + call.name + "'", call.loc);
            }
        } else if (!cls.generic_type_params.empty() && cls.constructor_index != 0xFFFFFFFFu) {
            const FunctionInfo& ctor = bytecode_.functions[cls.constructor_index];
            const std::vector<Parameter>& params = parameters_for_function(cls.constructor_index, 0xFFFFFFFFu, call.loc);
            if (call.arguments.size() > params.size()) {
                fail("'" + call.name + "' expects " + std::to_string(ctor.arity) + " arguments", call.loc);
            }
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                if (!params[i].type_name || params[i].type_name->empty()) {
                    continue;
                }
                TypeDescriptor param_type =
                    parse_type_spec(*params[i].type_name, call.loc, bytecode_, active_type_params());
                const TypeDescriptor arg_type = infer_type_descriptor(*call.arguments[i]);
                if (!unify_generic_types(param_type, arg_type, subst)) {
                    fail("invalid argument for '" + call.name + "'", call.arguments[i]->loc);
                }
            }
        } else if (!cls.generic_type_params.empty() && cls.constructor_index == 0xFFFFFFFFu &&
                   call.type_arguments.empty() && !call.arguments.empty()) {
            if (call.arguments.size() != cls.fields.size()) {
                fail("'" + call.name + "' expects " + std::to_string(cls.fields.size()) + " arguments", call.loc);
            }
            for (std::size_t i = 0; i < cls.fields.size(); ++i) {
                if (!cls.fields[i].type_name) {
                    continue;
                }
                TypeDescriptor field_type =
                    parse_type_spec(*cls.fields[i].type_name, call.loc, bytecode_, active_type_params());
                const TypeDescriptor arg_type = infer_type_descriptor(*call.arguments[i]);
                if (!unify_generic_types(field_type, arg_type, subst)) {
                    fail("invalid argument for '" + call.name + "'", call.arguments[i]->loc);
                }
            }
        }
        if (!cls.generic_type_params.empty() &&
            !validate_generic_constraints(cls.generic_type_params, cls.generic_type_param_constraints, subst,
                                          [this, &call](const TypeDescriptor& type, const std::string& constraint) {
                                              return type_satisfies_constraint(type, constraint, call.loc);
                                          })) {
            fail("type argument does not satisfy generic constraint for '" + call.name + "'", call.loc);
        }
        const std::uint32_t argc = static_cast<std::uint32_t>(call.arguments.size());
        if (cls.constructor_index == 0xFFFFFFFFu && argc > 0) {
            if (argc != cls.fields.size()) {
                fail("'" + call.name + "' expects " + std::to_string(cls.fields.size()) + " arguments", call.loc);
            }
            const std::uint32_t info =
                intern_class_call(ClassCallKind::Instance, symbol.class_index, 0xFFFFFFFFu, 0);
            emit(Instruction{Opcode::InvokeClass, dest, 0, info});
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                if (!call.arguments[i]) {
                    fail("invalid expression", call.loc);
                }
                const Register value = compile_expr(*call.arguments[i]);
                emit(Instruction{Opcode::SetField, dest, static_cast<std::uint32_t>(i), value});
            }
            pop_type_param_scope();
            return;
        }
        Register first = 0;
        if (!call.arguments.empty()) {
            first = next_register_;
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                alloc_register();
            }
            for (std::size_t i = 0; i < call.arguments.size(); ++i) {
                compile_expr_into(*call.arguments[i], first + static_cast<Register>(i));
            }
        }
        const std::uint32_t info = intern_class_call(ClassCallKind::Instance, symbol.class_index, cls.constructor_index,
                                                     argc);
        emit(Instruction{Opcode::InvokeClass, dest, first, info});
        pop_type_param_scope();
        return;
    }
    fail("invalid generic type call", call.loc);
}

}  // namespace pscript
