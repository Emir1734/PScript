#include "compiler/compiler.h"

namespace pscript {

namespace {

std::string type_pattern_base_name(const std::string& spec) {
    const std::size_t lt = spec.find('<');
    if (lt == std::string::npos) {
        return spec;
    }
    return spec.substr(0, lt);
}

}  // namespace

void Compiler::emit_bool_const(Register dest, bool value) {
    Constant constant;
    constant.kind = ConstantKind::Bool;
    constant.bool_value = value;
    emit(Instruction{Opcode::LoadConst, dest, intern_constant(std::move(constant)), 0});
}

Register Compiler::emit_bool_and(Register left, Register right) {
    const Register dest = alloc_register();
    const std::uint32_t left_false = emit_jump_if_false(left);
    const std::uint32_t right_false = emit_jump_if_false(right);
    emit_bool_const(dest, true);
    const std::uint32_t end = emit_jump();
    patch_jump(left_false, current_ip());
    emit_bool_const(dest, false);
    const std::uint32_t end_left = emit_jump();
    patch_jump(right_false, current_ip());
    emit_bool_const(dest, false);
    patch_jump(end, current_ip());
    patch_jump(end_left, current_ip());
    return dest;
}

Register Compiler::emit_bool_or(Register left, Register right) {
    const Register dest = alloc_register();
    const std::uint32_t left_true = emit_jump_if_true(left);
    const std::uint32_t right_true = emit_jump_if_true(right);
    emit_bool_const(dest, false);
    const std::uint32_t end = emit_jump();
    patch_jump(left_true, current_ip());
    emit_bool_const(dest, true);
    const std::uint32_t end_left = emit_jump();
    patch_jump(right_true, current_ip());
    emit_bool_const(dest, true);
    patch_jump(end, current_ip());
    patch_jump(end_left, current_ip());
    return dest;
}

bool Compiler::match_pattern_is_catch_all(const MatchPattern& pattern) const {
    if (pattern.kind == MatchPatternKind::Wildcard) {
        return true;
    }
    if (pattern.kind == MatchPatternKind::Binding) {
        return true;
    }
    return false;
}

std::string Compiler::match_pattern_key(const MatchPattern& pattern, const Location& loc) const {
    const auto literal_key = [](const Expr& expr) -> std::string {
        if (const auto* lit = dynamic_cast<const LiteralExpr*>(&expr)) {
            return std::to_string(static_cast<int>(lit->kind)) + "\n" + lit->lexeme;
        }
        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(&expr)) {
            if (const auto* ident = dynamic_cast<const IdentifierExpr*>(field->object.get())) {
                return "enum\n" + ident->name + "." + field->field;
            }
        }
        return "expr";
    };
    if (pattern.kind == MatchPatternKind::Literal && pattern.literal) {
        return literal_key(*pattern.literal);
    }
    if (pattern.kind == MatchPatternKind::Literals) {
        std::string combined = "multi";
        for (const auto& literal : pattern.literals) {
            if (!literal) {
                fail("invalid pattern", loc);
            }
            combined += "|" + literal_key(*literal);
        }
        return combined;
    }
    return "";
}

Register Compiler::emit_match_pattern_test(const MatchPattern& pattern, Register value_reg, const Location& loc) {
    if (pattern.kind == MatchPatternKind::Wildcard || pattern.kind == MatchPatternKind::Binding) {
        const Register dest = alloc_register();
        emit_bool_const(dest, true);
        return dest;
    }
    if (pattern.kind == MatchPatternKind::Literal) {
        if (!pattern.literal) {
            fail("invalid pattern", loc);
        }
        const Register expected = compile_expr(*pattern.literal);
        const Register dest = alloc_register();
        emit(Instruction{Opcode::Equal, dest, value_reg, expected});
        return dest;
    }
    if (pattern.kind == MatchPatternKind::Literals) {
        std::vector<std::uint32_t> hit_jumps;
        for (const auto& literal : pattern.literals) {
            if (!literal) {
                fail("invalid pattern", loc);
            }
            const Register expected = compile_expr(*literal);
            const Register eq = alloc_register();
            emit(Instruction{Opcode::Equal, eq, value_reg, expected});
            hit_jumps.push_back(emit_jump_if_true(eq));
        }
        const Register dest = alloc_register();
        const std::uint32_t miss = emit_jump();
        const std::uint32_t body_ip = current_ip();
        emit_bool_const(dest, true);
        const std::uint32_t end = current_ip();
        for (const std::uint32_t jump : hit_jumps) {
            patch_jump(jump, body_ip);
        }
        patch_jump(miss, end);
        emit_bool_const(dest, false);
        return dest;
    }
    if (pattern.kind == MatchPatternKind::TypeBind) {
        if (!pattern.type_name) {
            fail("invalid pattern", loc);
        }
        const std::string base_name = type_pattern_base_name(*pattern.type_name);
        std::uint32_t type_index = 0xFFFFFFFFu;
        std::uint32_t tag = 1u << 31;
        const std::uint32_t class_index = find_class_by_name(base_name);
        if (class_index != 0xFFFFFFFFu) {
            type_index = class_index;
        } else {
            type_index = resolve_struct_index(base_name, loc);
            tag = 0;
        }
        const Register dest = alloc_register();
        emit(Instruction{Opcode::TestInstanceOf, dest, value_reg, tag | type_index});
        return dest;
    }
    if (pattern.kind == MatchPatternKind::Or) {
        std::vector<std::uint32_t> hit_jumps;
        for (const auto& part : pattern.parts) {
            if (!part) {
                fail("invalid pattern", loc);
            }
            const Register part_test = emit_match_pattern_test(*part, value_reg, loc);
            hit_jumps.push_back(emit_jump_if_true(part_test));
        }
        const Register dest = alloc_register();
        const std::uint32_t miss = emit_jump();
        const std::uint32_t body_ip = current_ip();
        emit_bool_const(dest, true);
        const std::uint32_t end = current_ip();
        for (const std::uint32_t jump : hit_jumps) {
            patch_jump(jump, body_ip);
        }
        patch_jump(miss, end);
        emit_bool_const(dest, false);
        return dest;
    }
    if (pattern.kind == MatchPatternKind::And) {
        if (pattern.parts.empty() || !pattern.parts[0]) {
            fail("invalid pattern", loc);
        }
        Register dest = emit_match_pattern_test(*pattern.parts[0], value_reg, loc);
        for (std::size_t i = 1; i < pattern.parts.size(); ++i) {
            if (!pattern.parts[i]) {
                fail("invalid pattern", loc);
            }
            const Register part_test = emit_match_pattern_test(*pattern.parts[i], value_reg, loc);
            dest = emit_bool_and(dest, part_test);
        }
        return dest;
    }
    if (pattern.kind == MatchPatternKind::Destruct && pattern.destruct) {
        return emit_match_destruct_pattern_test(*pattern.destruct, value_reg, loc);
    }
    if (pattern.kind == MatchPatternKind::EnumVariant) {
        if (!pattern.enum_type || !pattern.enum_variant) {
            fail("invalid pattern", loc);
        }
        const Symbol* symbol = lookup_symbol(*pattern.enum_type);
        if (!symbol || symbol->kind != SymbolKind::Enum) {
            fail("unknown enum '" + *pattern.enum_type + "'", loc);
        }
        const std::uint32_t member_index =
            resolve_enum_member(symbol->enum_index, *pattern.enum_variant, loc);
        const Register dest = alloc_register();
        const std::uint32_t packed = (member_index << 16) | (symbol->enum_index & 0xFFFFu);
        emit(Instruction{Opcode::TestEnumMember, dest, value_reg, packed});
        return dest;
    }
    fail("invalid pattern", loc);
}

Register Compiler::emit_match_destruct_pattern_test(const DestructPattern& destruct, Register value_reg,
                                                  const Location& loc) {
        if (destruct.kind == DestructPatternKind::List || destruct.kind == DestructPatternKind::Tuple) {
            const std::uint32_t kind = destruct.kind == DestructPatternKind::List ? 0u : 1u;
            const Register ok = alloc_register();
            const std::uint32_t packed =
                (static_cast<std::uint32_t>(destruct.elements.size()) << 8) | kind;
            emit(Instruction{Opcode::TestDestructShape, ok, value_reg, packed});
            Register result = ok;
            for (std::size_t i = 0; i < destruct.elements.size(); ++i) {
                const DestructPattern& element = *destruct.elements[i];
                if (element.kind == DestructPatternKind::Literal && element.literal_expr) {
                    const Register index = alloc_register();
                    compile_integer_into(index, static_cast<std::int64_t>(i));
                    const Register item = alloc_register();
                    emit(Instruction{Opcode::GetIndex, item, value_reg, index});
                    const Register expected = compile_expr(*element.literal_expr);
                    const Register eq = alloc_register();
                    emit(Instruction{Opcode::Equal, eq, item, expected});
                    result = emit_bool_and(result, eq);
                } else if (element.kind == DestructPatternKind::List || element.kind == DestructPatternKind::Tuple ||
                           element.kind == DestructPatternKind::Dict || element.kind == DestructPatternKind::Struct ||
                           element.kind == DestructPatternKind::Positional) {
                    const Register index = alloc_register();
                    compile_integer_into(index, static_cast<std::int64_t>(i));
                    const Register item = alloc_register();
                    emit(Instruction{Opcode::GetIndex, item, value_reg, index});
                    const Register nested_ok = emit_match_destruct_pattern_test(element, item, loc);
                    result = emit_bool_and(result, nested_ok);
                }
            }
            return result;
        }
        if (destruct.kind == DestructPatternKind::Struct || destruct.kind == DestructPatternKind::Positional) {
            std::uint32_t type_index = 0xFFFFFFFFu;
            std::uint32_t tag = 1u << 31;
            const std::string base_name = type_pattern_base_name(destruct.struct_type_name);
            const std::uint32_t class_index = find_class_by_name(base_name);
            if (class_index != 0xFFFFFFFFu) {
                type_index = class_index;
            } else {
                type_index = resolve_struct_index(base_name, loc);
                tag = 0;
            }
            Register ok = alloc_register();
            emit(Instruction{Opcode::TestInstanceOf, ok, value_reg, tag | type_index});
            if (destruct.kind == DestructPatternKind::Positional) {
                const ClassInfo* class_info = class_index != 0xFFFFFFFFu ? &bytecode_.classes[class_index] : nullptr;
                const StructInfo* struct_info =
                    class_info ? nullptr : &bytecode_.structs[type_index];
                for (std::size_t i = 0; i < destruct.elements.size(); ++i) {
                    const DestructPattern& element = *destruct.elements[i];
                    std::uint32_t field_index = static_cast<std::uint32_t>(i);
                    if (class_info && i < class_info->layout.size()) {
                        field_index = static_cast<std::uint32_t>(i);
                    } else if (struct_info && i < struct_info->fields.size()) {
                        field_index = static_cast<std::uint32_t>(i);
                    }
                    const Register item = alloc_register();
                    emit(Instruction{Opcode::GetField, item, value_reg, field_index});
                    if (element.kind == DestructPatternKind::Wildcard) {
                        continue;
                    }
                    if (element.kind == DestructPatternKind::Identifier ||
                        element.kind == DestructPatternKind::Wildcard) {
                        continue;
                    }
                    const Register nested_ok = emit_match_destruct_pattern_test(element, item, loc);
                    ok = emit_bool_and(ok, nested_ok);
                }
            }
            return ok;
        }
        fail("unsupported match pattern", loc);
}

void Compiler::emit_match_pattern_bindings(const MatchPattern& pattern, Register value_reg, const Location& loc) {
    if (pattern.kind == MatchPatternKind::Binding && pattern.binding) {
        intern_binding(*pattern.binding, loc);
        emit_store_name(*pattern.binding, value_reg, loc);
        return;
    }
    if (pattern.kind == MatchPatternKind::TypeBind && pattern.binding) {
        intern_binding(*pattern.binding, loc);
        emit_store_name(*pattern.binding, value_reg, loc);
        return;
    }
    if (pattern.kind == MatchPatternKind::And) {
        for (const auto& part : pattern.parts) {
            if (part) {
                emit_match_pattern_bindings(*part, value_reg, loc);
            }
        }
        return;
    }
    if (pattern.kind == MatchPatternKind::Or) {
        return;
    }
    if (pattern.kind == MatchPatternKind::Destruct && pattern.destruct) {
        emit_match_destruct_bindings(*pattern.destruct, value_reg, loc);
    }
    if (pattern.kind == MatchPatternKind::EnumVariant) {
        for (std::size_t i = 0; i < pattern.enum_bindings.size(); ++i) {
            const std::string& binding = pattern.enum_bindings[i];
            intern_binding(binding, loc);
            const Register payload = alloc_register();
            emit(Instruction{Opcode::GetEnumPayload, payload, value_reg, 0});
            if (pattern.enum_bindings.size() == 1) {
                emit_store_name(binding, payload, loc);
                continue;
            }
            const Register index = alloc_register();
            compile_integer_into(index, static_cast<std::int64_t>(i));
            const Register item = alloc_register();
            emit(Instruction{Opcode::GetIndex, item, payload, index});
            emit_store_name(binding, item, loc);
        }
    }
}

void Compiler::emit_match_destruct_bindings(const DestructPattern& pattern, Register source, const Location& loc) {
    if (pattern.kind == DestructPatternKind::Identifier) {
        intern_binding(pattern.name, loc);
        emit_store_name(pattern.name, source, loc);
        return;
    }
    if (pattern.kind == DestructPatternKind::Wildcard || pattern.kind == DestructPatternKind::Literal ||
        pattern.kind == DestructPatternKind::Rest) {
        return;
    }
    if (pattern.kind == DestructPatternKind::List || pattern.kind == DestructPatternKind::Tuple) {
        for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
            if (!pattern.elements[i]) {
                continue;
            }
            const Register index = alloc_register();
            compile_integer_into(index, static_cast<std::int64_t>(i));
            const Register item = alloc_register();
            emit(Instruction{Opcode::GetIndex, item, source, index});
            emit_match_destruct_bindings(*pattern.elements[i], item, loc);
        }
        return;
    }
    if (pattern.kind == DestructPatternKind::Dict) {
        for (const DestructDictEntry& entry : pattern.dict_entries) {
            if (!entry.pattern) {
                continue;
            }
            const Register item = compile_dict_key_index(source, entry.key, loc);
            emit_match_destruct_bindings(*entry.pattern, item, loc);
        }
        if (pattern.dict_rest) {
            const Register rest_value = alloc_register();
            emit_destruct_dict_rest(rest_value, source, {});
            resolve_assign(pattern.dict_rest->name, loc);
            emit_store_name(pattern.dict_rest->name, rest_value, loc);
        }
        return;
    }
    if (pattern.kind == DestructPatternKind::Struct || pattern.kind == DestructPatternKind::Positional) {
        const std::string base_name = type_pattern_base_name(pattern.struct_type_name);
        const std::uint32_t class_index = find_class_by_name(base_name);
        if (class_index != 0xFFFFFFFFu) {
            for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
                if (!pattern.elements[i]) {
                    continue;
                }
                const Register item = alloc_register();
                emit(Instruction{Opcode::GetField, item, source, static_cast<std::uint32_t>(i)});
                emit_match_destruct_bindings(*pattern.elements[i], item, loc);
            }
            return;
        }
        const std::uint32_t struct_index = resolve_struct_index(base_name, loc);
        for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
            if (!pattern.elements[i]) {
                continue;
            }
            std::string field_name;
            if (pattern.elements[i]->kind == DestructPatternKind::Identifier) {
                field_name = pattern.elements[i]->name;
            }
            if (field_name.empty() && pattern.elements[i]->kind != DestructPatternKind::Wildcard &&
                pattern.elements[i]->kind != DestructPatternKind::Literal) {
                continue;
            }
            if (!field_name.empty()) {
                const std::uint32_t field_index = resolve_field_index(struct_index, field_name, loc);
                const Register item = alloc_register();
                emit(Instruction{Opcode::GetField, item, source, field_index});
                emit_match_destruct_bindings(*pattern.elements[i], item, loc);
            }
        }
    }
}

std::optional<Compiler::ClassPropertyLookup> Compiler::find_class_property(std::uint32_t class_index,
                                                                           const std::string& name) const {
    for (std::uint32_t current = class_index; current != 0xFFFFFFFFu;
         current = bytecode_.classes[current].parent_index) {
        const ClassInfo& info = bytecode_.classes[current];
        for (std::size_t i = 0; i < info.properties.size(); ++i) {
            if (info.properties[i].name == name) {
                ClassPropertyLookup lookup;
                lookup.class_index = current;
                lookup.property_index = static_cast<std::uint32_t>(i);
                lookup.getter_access = info.properties[i].getter_access;
                lookup.setter_access = info.properties[i].setter_access;
                lookup.getter_function_index = info.properties[i].getter_function_index;
                lookup.setter_function_index = info.properties[i].setter_function_index;
                lookup.has_getter = lookup.getter_function_index != 0xFFFFFFFFu;
                lookup.has_setter = lookup.setter_function_index != 0xFFFFFFFFu;
                return lookup;
            }
        }
    }
    return std::nullopt;
}

void Compiler::check_class_property_access(const ClassPropertyLookup& property, bool is_write,
                                           const Location& loc) const {
    const MemberAccess access = is_write ? property.setter_access : property.getter_access;
    if (access == MemberAccess::Public) {
        return;
    }
    if (!class_context_) {
        fail(std::string("cannot access ") + member_access_name(access) + " property", loc);
    }
    if (access == MemberAccess::Private && class_context_->class_index != property.class_index) {
        fail("cannot access private property", loc);
    }
    if (access == MemberAccess::Protected && !is_class_or_subclass(class_context_->class_index, property.class_index)) {
        fail("cannot access protected property", loc);
    }
}

void Compiler::mark_enum_match_coverage(const MatchPattern& pattern, std::vector<bool>& covered,
                                          const Location& loc) const {
    if (pattern.kind == MatchPatternKind::Wildcard || pattern.kind == MatchPatternKind::Binding) {
        std::fill(covered.begin(), covered.end(), true);
        return;
    }
    if (pattern.kind == MatchPatternKind::EnumVariant) {
        if (!pattern.enum_type || !pattern.enum_variant) {
            return;
        }
        const Symbol* symbol = lookup_symbol(*pattern.enum_type);
        if (!symbol || symbol->kind != SymbolKind::Enum) {
            return;
        }
        const std::uint32_t member_index = resolve_enum_member(symbol->enum_index, *pattern.enum_variant, loc);
        if (member_index < covered.size()) {
            covered[member_index] = true;
        }
        return;
    }
    if (pattern.kind == MatchPatternKind::Literal && pattern.literal) {
        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(pattern.literal.get())) {
            if (const auto* ident = dynamic_cast<const IdentifierExpr*>(field->object.get())) {
                if (const Symbol* symbol = lookup_symbol(ident->name)) {
                    if (symbol->kind == SymbolKind::Enum) {
                        const std::uint32_t member_index =
                            resolve_enum_member(symbol->enum_index, field->field, loc);
                        if (member_index < covered.size()) {
                            covered[member_index] = true;
                        }
                    }
                }
            }
        }
        return;
    }
    if (pattern.kind == MatchPatternKind::Or) {
        for (const auto& part : pattern.parts) {
            if (part) {
                mark_enum_match_coverage(*part, covered, loc);
            }
        }
        return;
    }
    if (pattern.kind == MatchPatternKind::And) {
        for (const auto& part : pattern.parts) {
            if (part) {
                mark_enum_match_coverage(*part, covered, loc);
            }
        }
    }
}

void Compiler::compile_match(const MatchStmt& stmt) {
    if (!stmt.value) {
        fail("invalid statement", stmt.loc);
    }
    if (stmt.cases.empty()) {
        fail("empty match", stmt.loc);
    }
    const Register value_reg = compile_expr(*stmt.value);
    std::vector<std::uint32_t> end_jumps;
    bool exhaustive = false;
    std::vector<std::string> seen_keys;
    std::optional<std::vector<bool>> enum_coverage;
    const TypeDescriptor scrutinee_type = infer_type_descriptor(*stmt.value);
    if (scrutinee_type.kind == StaticType::Enum && scrutinee_type.enum_index &&
        *scrutinee_type.enum_index < bytecode_.enums.size()) {
        enum_coverage.emplace(bytecode_.enums[*scrutinee_type.enum_index].members.size(), false);
    }
    for (const MatchCase& branch : stmt.cases) {
        if (exhaustive) {
            fail("unreachable match case", branch.loc);
        }
        if (!branch.pattern) {
            fail("invalid pattern", branch.loc);
        }
        if (branch.pattern->kind == MatchPatternKind::Wildcard && branch.guard) {
            fail("wildcard cannot have a guard", branch.loc);
        }
        const Register matched = emit_match_pattern_test(*branch.pattern, value_reg, branch.loc);
        const std::uint32_t skip = emit_jump_if_false(matched);
        push_scope();
        emit_match_pattern_bindings(*branch.pattern, value_reg, branch.loc);
        if (branch.guard) {
            const Register guard = compile_expr(*branch.guard);
            const std::uint32_t guard_fail = emit_jump_if_false(guard);
            compile_scoped_block(branch.body, branch.loc);
            end_jumps.push_back(emit_jump());
            pop_scope();
            patch_jump(guard_fail, current_ip());
            patch_jump(skip, current_ip());
            continue;
        }
        compile_scoped_block(branch.body, branch.loc);
        end_jumps.push_back(emit_jump());
        pop_scope();
        patch_jump(skip, current_ip());
        if (!branch.guard && match_pattern_is_catch_all(*branch.pattern)) {
            exhaustive = true;
        }
        if (enum_coverage && branch.pattern) {
            mark_enum_match_coverage(*branch.pattern, *enum_coverage, branch.loc);
        }
        const std::string key = match_pattern_key(*branch.pattern, branch.loc);
        if (!key.empty()) {
            for (const std::string& seen : seen_keys) {
                if (seen == key) {
                    fail("duplicate match pattern", branch.loc);
                }
            }
            seen_keys.push_back(key);
        }
    }
    const std::uint32_t end = current_ip();
    for (const std::uint32_t jump : end_jumps) {
        patch_jump(jump, end);
    }
    if (enum_coverage && !exhaustive) {
        for (bool covered : *enum_coverage) {
            if (!covered) {
                fail("non-exhaustive match on enum", stmt.loc);
            }
        }
    }
}

void Compiler::compile_match_expr_into(const MatchExpr& expr, Register dest) {
    if (!expr.value) {
        fail("invalid expression", expr.loc);
    }
    if (expr.cases.empty()) {
        fail("empty match", expr.loc);
    }
    const Register value_reg = compile_expr(*expr.value);
    std::vector<std::uint32_t> end_jumps;
    bool exhaustive = false;
    std::vector<std::string> seen_keys;
    for (const MatchCase& branch : expr.cases) {
        if (exhaustive) {
            fail("unreachable match case", branch.loc);
        }
        if (!branch.pattern) {
            fail("invalid pattern", branch.loc);
        }
        if (branch.pattern->kind == MatchPatternKind::Wildcard && branch.guard) {
            fail("wildcard cannot have a guard", branch.loc);
        }
        const Register matched = emit_match_pattern_test(*branch.pattern, value_reg, branch.loc);
        std::uint32_t skip = emit_jump_if_false(matched);
        push_scope();
        emit_match_pattern_bindings(*branch.pattern, value_reg, branch.loc);
        if (branch.guard) {
            const Register guard = compile_expr(*branch.guard);
            const std::uint32_t guard_fail = emit_jump_if_false(guard);
            if (!branch.result_expr) {
                fail("invalid match expression", branch.loc);
            }
            compile_expr_into(*branch.result_expr, dest);
            end_jumps.push_back(emit_jump());
            pop_scope();
            patch_jump(guard_fail, current_ip());
            patch_jump(skip, current_ip());
            continue;
        }
        if (!branch.result_expr) {
            fail("invalid match expression", branch.loc);
        }
        compile_expr_into(*branch.result_expr, dest);
        end_jumps.push_back(emit_jump());
        pop_scope();
        patch_jump(skip, current_ip());
        if (!branch.guard && match_pattern_is_catch_all(*branch.pattern)) {
            exhaustive = true;
        }
        const std::string key = match_pattern_key(*branch.pattern, branch.loc);
        if (!key.empty()) {
            for (const std::string& seen : seen_keys) {
                if (seen == key) {
                    fail("duplicate match pattern", branch.loc);
                }
            }
            seen_keys.push_back(key);
        }
    }
    const std::uint32_t end = current_ip();
    for (const std::uint32_t jump : end_jumps) {
        patch_jump(jump, end);
    }
}

}  // namespace pscript
