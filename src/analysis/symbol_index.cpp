#include "analysis/symbol_index.h"

namespace pscript {
namespace {

SourceRange range_from(const Location& loc) {
    SourceRange range;
    range.start = loc;
    range.end = loc;
    return range;
}

SymbolVisibility visibility_from(MemberAccess access) {
    switch (access) {
        case MemberAccess::Protected:
            return SymbolVisibility::Protected;
        case MemberAccess::Private:
            return SymbolVisibility::Private;
        case MemberAccess::Public:
        default:
            return SymbolVisibility::Public;
    }
}

void add_param_symbols(const std::vector<Parameter>& parameters, const std::string& prefix, const std::string& file,
                       SymbolIndex& index, std::uint32_t parent_id) {
    for (const Parameter& param : parameters) {
        if (param.name.empty()) {
            continue;
        }
        SymbolRecord record;
        record.kind = SymbolKind::Parameter;
        record.name = param.name;
        record.qualified_name = prefix + param.name;
        record.range = range_from(param.loc);
        record.parent_id = parent_id;
        record.file = file;
        if (param.type_name.has_value()) {
            record.type_signature = *param.type_name;
        }
        index.add(std::move(record));
    }
}

void collect_function_body_symbols(const std::vector<std::unique_ptr<Stmt>>& body, const std::string& prefix,
                                   const std::string& file, SymbolIndex& index, std::uint32_t parent_id);

void collect_stmt_symbols(const Stmt& stmt, const std::string& prefix, const std::string& file, SymbolIndex& index) {
    if (const auto* assign = dynamic_cast<const AssignStmt*>(&stmt)) {
        SymbolRecord record;
        record.kind = SymbolKind::Variable;
        record.name = assign->name;
        record.qualified_name = prefix + assign->name;
        record.range = range_from(assign->loc);
        record.visibility = visibility_from(assign->access);
        record.file = file;
        if (assign->type_name.has_value()) {
            record.type_signature = *assign->type_name;
        }
        index.add(std::move(record));
        return;
    }
    if (const auto* fn = dynamic_cast<const FuncStmt*>(&stmt)) {
        SymbolRecord record;
        record.kind = SymbolKind::Function;
        record.name = fn->name;
        record.qualified_name = prefix + fn->name;
        record.range = range_from(fn->loc);
        record.visibility = visibility_from(fn->access);
        record.file = file;
        if (fn->return_type_name.has_value()) {
            record.type_signature = *fn->return_type_name;
        }
        const std::uint32_t fn_id = index.add(std::move(record));
        add_param_symbols(fn->parameters, prefix + fn->name + ".", file, index, fn_id);
        collect_function_body_symbols(fn->body, prefix + fn->name + ".", file, index, fn_id);
        return;
    }
    if (const auto* import_stmt = dynamic_cast<const ImportStmt*>(&stmt)) {
        SymbolRecord record;
        record.kind = SymbolKind::Module;
        record.qualified_name = import_stmt->module;
        record.name = import_stmt->module_alias.value_or(import_stmt->module);
        record.range = range_from(import_stmt->loc);
        record.file = file;
        index.add(std::move(record));
        return;
    }
    if (const auto* struct_stmt = dynamic_cast<const StructStmt*>(&stmt)) {
        SymbolRecord record;
        record.kind = struct_stmt->is_record ? SymbolKind::Struct : SymbolKind::Struct;
        record.name = struct_stmt->name;
        record.qualified_name = struct_stmt->name;
        record.range = range_from(struct_stmt->loc);
        record.visibility = visibility_from(struct_stmt->access);
        record.file = file;
        index.add(std::move(record));
        return;
    }
    if (const auto* enum_stmt = dynamic_cast<const EnumStmt*>(&stmt)) {
        SymbolRecord record;
        record.kind = SymbolKind::Enum;
        record.name = enum_stmt->name;
        record.qualified_name = enum_stmt->name;
        record.range = range_from(enum_stmt->loc);
        record.visibility = visibility_from(enum_stmt->access);
        record.file = file;
        index.add(std::move(record));
        return;
    }
    if (const auto* iface = dynamic_cast<const InterfaceStmt*>(&stmt)) {
        SymbolRecord record;
        record.kind = SymbolKind::Interface;
        record.name = iface->name;
        record.qualified_name = iface->name;
        record.range = range_from(iface->loc);
        record.visibility = visibility_from(iface->access);
        record.file = file;
        index.add(std::move(record));
        for (const InterfaceMethodSig& method : iface->methods) {
            SymbolRecord method_record;
            method_record.kind = SymbolKind::Method;
            method_record.name = method.name;
            method_record.qualified_name = iface->name + "." + method.name;
            method_record.range = range_from(method.loc);
            method_record.parent_id = index.all().back().id;
            method_record.file = file;
            if (method.return_type_name.has_value()) {
                method_record.type_signature = *method.return_type_name;
            }
            index.add(std::move(method_record));
        }
        return;
    }
    if (const auto* class_stmt = dynamic_cast<const ClassStmt*>(&stmt)) {
        SymbolRecord record;
        record.kind = SymbolKind::Class;
        record.name = class_stmt->name;
        record.qualified_name = class_stmt->name;
        record.range = range_from(class_stmt->loc);
        record.visibility = visibility_from(class_stmt->access);
        record.file = file;
        index.add(std::move(record));
        const std::uint32_t class_id = index.all().back().id;
        for (const ClassProperty& property : class_stmt->properties) {
            SymbolRecord prop;
            prop.kind = SymbolKind::Property;
            prop.name = property.name;
            prop.qualified_name = class_stmt->name + "." + property.name;
            prop.range = range_from(class_stmt->loc);
            prop.parent_id = class_id;
            prop.visibility = visibility_from(property.getter_access);
            prop.file = file;
            if (property.type_name.has_value()) {
                prop.type_signature = *property.type_name;
            }
            index.add(std::move(prop));
        }
        for (const StructField& field : class_stmt->fields) {
            SymbolRecord field_record;
            field_record.kind = SymbolKind::Property;
            field_record.name = field.name;
            field_record.qualified_name = class_stmt->name + "." + field.name;
            field_record.range = range_from(field.loc);
            field_record.parent_id = class_id;
            field_record.visibility = visibility_from(field.access);
            field_record.file = file;
            if (field.type_name.has_value()) {
                field_record.type_signature = *field.type_name;
            }
            index.add(std::move(field_record));
        }
        if (class_stmt->constructor) {
            SymbolRecord ctor;
            ctor.kind = SymbolKind::Constructor;
            ctor.name = "constructor";
            ctor.qualified_name = class_stmt->name + ".constructor";
            ctor.range = range_from(class_stmt->constructor->loc);
            ctor.parent_id = class_id;
            ctor.file = file;
            index.add(std::move(ctor));
            add_param_symbols(class_stmt->constructor->parameters, class_stmt->name + ".constructor.", file, index,
                              class_id);
        }
        for (const auto& method : class_stmt->methods) {
            if (!method) {
                continue;
            }
            SymbolRecord method_record;
            method_record.kind = SymbolKind::Method;
            method_record.name = method->name;
            method_record.qualified_name = class_stmt->name + "." + method->name;
            method_record.range = range_from(method->loc);
            method_record.parent_id = class_id;
            method_record.file = file;
            if (method->return_type_name.has_value()) {
                method_record.type_signature = *method->return_type_name;
            }
            index.add(std::move(method_record));
            add_param_symbols(method->parameters, class_stmt->name + "." + method->name + ".", file, index, class_id);
        }
    }
}

void collect_function_body_symbols(const std::vector<std::unique_ptr<Stmt>>& body, const std::string& prefix,
                                   const std::string& file, SymbolIndex& index, std::uint32_t parent_id) {
    for (const auto& stmt : body) {
        if (!stmt) {
            continue;
        }
        collect_stmt_symbols(*stmt, prefix, file, index);
        (void)parent_id;
    }
}

}  // namespace

void SymbolIndex::clear() {
    symbols_.clear();
    next_id_ = 1;
}

std::uint32_t SymbolIndex::add(SymbolRecord symbol) {
    symbol.id = next_id_++;
    symbols_.push_back(std::move(symbol));
    return symbols_.back().id;
}

const SymbolRecord* SymbolIndex::find_by_qualified_name(const std::string& qualified) const {
    for (const SymbolRecord& symbol : symbols_) {
        if (symbol.qualified_name == qualified) {
            return &symbol;
        }
    }
    return nullptr;
}

const SymbolRecord* SymbolIndex::find_at_position(const std::string& file, int line, int column) const {
    const SymbolRecord* best = nullptr;
    for (const SymbolRecord& symbol : symbols_) {
        if (symbol.file != file) {
            continue;
        }
        if (symbol.range.start.line != line) {
            continue;
        }
        if (symbol.range.start.column > column) {
            continue;
        }
        if (!best || symbol.range.start.column > best->range.start.column) {
            best = &symbol;
        }
    }
    return best;
}

std::vector<const SymbolRecord*> SymbolIndex::find_by_name(std::string_view simple_name) const {
    std::vector<const SymbolRecord*> out;
    for (const SymbolRecord& symbol : symbols_) {
        if (symbol.name == simple_name) {
            out.push_back(&symbol);
        }
    }
    return out;
}

void build_symbol_index(const Program& program, const std::string& file, SymbolIndex& index) {
    index.clear();
    for (const auto& stmt : program.statements) {
        if (!stmt) {
            continue;
        }
        collect_stmt_symbols(*stmt, "", file, index);
    }
}

}  // namespace pscript
