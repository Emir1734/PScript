#include "compiler/type_parse.h"

#include "compiler/compiler_error.h"
#include "compiler/bytecode.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

namespace pscript {

namespace {

std::string trim_type_spec(std::string spec) {
    while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.front()))) {
        spec.erase(spec.begin());
    }
    while (!spec.empty() && std::isspace(static_cast<unsigned char>(spec.back()))) {
        spec.pop_back();
    }
    return spec;
}

std::string normalize_type_base(std::string base) {
    if (base == "List") {
        return "list";
    }
    if (base == "Dict" || base == "dict") {
        return "map";
    }
    if (base == "Task") {
        return "task";
    }
    if (base == "Tuple") {
        return "tuple";
    }
    if (base == "Set") {
        return "set";
    }
    return base;
}

bool types_exactly_equal_local(const TypeDescriptor& left, const TypeDescriptor& right);

const TypeAliasInfo* find_type_alias(const Bytecode& bytecode, const std::string& name) {
    for (const TypeAliasInfo& alias : bytecode.type_aliases) {
        if (alias.name == name) {
            return &alias;
        }
    }
    return nullptr;
}

bool is_type_name_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::string replace_type_identifier(std::string text, const std::string& param, const std::string& replacement) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        if (std::isalpha(static_cast<unsigned char>(text[i])) || text[i] == '_') {
            const std::size_t start = i;
            ++i;
            while (i < text.size() && is_type_name_char(text[i])) {
                ++i;
            }
            const std::string_view token(text.data() + start, i - start);
            if (token == param) {
                out += replacement;
            } else {
                out.append(text, start, i - start);
            }
            continue;
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

bool union_member_equal(const TypeDescriptor& left, const TypeDescriptor& right) {
    if (left.kind == StaticType::Union && right.kind == StaticType::Union) {
        if (left.type_args.size() != right.type_args.size()) {
            return false;
        }
        for (const TypeDescriptor& member : left.type_args) {
            if (!is_union_member(right, member)) {
                return false;
            }
        }
        return true;
    }
    return types_exactly_equal_local(left, right);
}

bool types_exactly_equal_local(const TypeDescriptor& left, const TypeDescriptor& right) {
    if (left.kind != right.kind) {
        return false;
    }
    if (left.kind == StaticType::TypeParam) {
        return left.type_param_name == right.type_param_name;
    }
    if (left.kind == StaticType::Struct && left.struct_index != right.struct_index) {
        return false;
    }
    if (left.kind == StaticType::Class && left.class_index != right.class_index) {
        return false;
    }
    if (left.kind == StaticType::Enum && left.enum_index != right.enum_index) {
        return false;
    }
    if (left.type_args.size() != right.type_args.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.type_args.size(); ++i) {
        if (!types_exactly_equal_local(left.type_args[i], right.type_args[i])) {
            return false;
        }
    }
    return true;
}

class TypeSpecParser {
public:
    TypeSpecParser(std::string spec, const Location& loc, const Bytecode& bytecode,
                   const std::vector<std::string>* active_type_params, std::vector<std::string>& alias_stack,
                   const std::unordered_map<std::string, TypeDescriptor>* type_substitutions = nullptr)
        : spec_(std::move(spec)),
          loc_(loc),
          bytecode_(bytecode),
          active_type_params_(active_type_params),
          alias_stack_(alias_stack),
          type_substitutions_(type_substitutions),
          pos_(0) {}

    TypeDescriptor parse() {
        TypeDescriptor desc = parse_union_type();
        if (!eof()) {
            fail("invalid type syntax");
        }
        return desc;
    }

private:
    std::string spec_;
    const Location& loc_;
    const Bytecode& bytecode_;
    const std::vector<std::string>* active_type_params_;
    std::vector<std::string>& alias_stack_;
    const std::unordered_map<std::string, TypeDescriptor>* type_substitutions_;
    std::size_t pos_;

    bool eof() const { return pos_ >= spec_.size(); }

    char peek_char() const {
        if (eof()) {
            return '\0';
        }
        return spec_[pos_];
    }

    void advance() { ++pos_; }

    [[noreturn]] void fail(const std::string& message) const {
        throw CompilerError(message, loc_.line, loc_.column);
    }

    void expect_char(char expected) {
        if (peek_char() != expected) {
            fail("invalid type syntax");
        }
        advance();
    }

    void skip_spaces() {
        while (!eof() && std::isspace(static_cast<unsigned char>(peek_char()))) {
            advance();
        }
    }

    std::string parse_base_name() {
        skip_spaces();
        std::size_t start = pos_;
        while (!eof()) {
            const char ch = peek_char();
            if (ch == '<' || ch == ',' || ch == '>' || ch == '|' || std::isspace(static_cast<unsigned char>(ch))) {
                break;
            }
            advance();
        }
        if (start == pos_) {
            fail("invalid type syntax");
        }
        return normalize_type_base(trim_type_spec(spec_.substr(start, pos_ - start)));
    }

    TypeDescriptor resolve_base(const std::string& base) {
        TypeDescriptor desc;
        if (base == "int") {
            desc.kind = StaticType::Integer;
            return desc;
        }
        if (base == "float") {
            desc.kind = StaticType::Float;
            return desc;
        }
        if (base == "bool") {
            desc.kind = StaticType::Bool;
            return desc;
        }
        if (base == "string" || base == "str") {
            desc.kind = StaticType::String;
            return desc;
        }
        if (base == "any") {
            desc.kind = StaticType::Any;
            return desc;
        }
        if (base == "task") {
            desc.kind = StaticType::Task;
            return desc;
        }
        if (base == "tuple") {
            desc.kind = StaticType::Tuple;
            return desc;
        }
        if (base == "set") {
            desc.kind = StaticType::Set;
            return desc;
        }
        if (base == "null") {
            fail("Type error: 'null' is not a variable type");
        }
        if (base == "list") {
            desc.kind = StaticType::List;
            return desc;
        }
        if (base == "map") {
            desc.kind = StaticType::Dict;
            return desc;
        }
        for (std::size_t i = 0; i < bytecode_.structs.size(); ++i) {
            if (bytecode_.structs[i].name == base) {
                desc.kind = StaticType::Struct;
                desc.struct_index = static_cast<std::uint32_t>(i);
                return desc;
            }
        }
        for (std::size_t i = 0; i < bytecode_.classes.size(); ++i) {
            if (bytecode_.classes[i].name == base) {
                desc.kind = StaticType::Class;
                desc.class_index = static_cast<std::uint32_t>(i);
                return desc;
            }
        }
        for (std::size_t i = 0; i < bytecode_.enums.size(); ++i) {
            if (bytecode_.enums[i].name == base) {
                desc.kind = StaticType::Enum;
                desc.enum_index = static_cast<std::uint32_t>(i);
                return desc;
            }
        }
        if (type_substitutions_) {
            const auto substituted = type_substitutions_->find(base);
            if (substituted != type_substitutions_->end()) {
                return substituted->second;
            }
        }
        if (active_type_params_) {
            for (const std::string& param : *active_type_params_) {
                if (param == base) {
                    desc.kind = StaticType::TypeParam;
                    desc.type_param_name = base;
                    return desc;
                }
            }
        }
        fail("Type error: unknown type '" + base + "'");
    }

    TypeDescriptor parse_union_type() {
        std::vector<TypeDescriptor> members;
        members.push_back(parse_primary_type());
        skip_spaces();
        while (peek_char() == '|') {
            advance();
            members.push_back(parse_primary_type());
            skip_spaces();
        }
        return normalize_union_type(std::move(members));
    }

    TypeDescriptor parse_type_alias(const TypeAliasInfo& alias) {
        if (std::find(alias_stack_.begin(), alias_stack_.end(), alias.name) != alias_stack_.end()) {
            fail("cyclic type alias");
        }
        alias_stack_.push_back(alias.name);
        std::vector<TypeDescriptor> args;
        skip_spaces();
        if (!alias.type_params.empty()) {
            if (peek_char() != '<') {
                fail("generic type alias requires type arguments");
            }
            advance();
            for (;;) {
                args.push_back(parse_union_type());
                skip_spaces();
                if (peek_char() == '>') {
                    advance();
                    break;
                }
                expect_char(',');
            }
            if (args.size() != alias.type_params.size()) {
                fail("wrong number of type arguments for type alias");
            }
        } else if (peek_char() == '<') {
            fail("type alias does not accept type arguments");
        }
        std::unordered_map<std::string, TypeDescriptor> substitutions;
        for (std::size_t i = 0; i < alias.type_params.size(); ++i) {
            substitutions.emplace(alias.type_params[i], args[i]);
        }
        TypeSpecParser nested(alias.target_type, loc_, bytecode_, active_type_params_, alias_stack_, &substitutions);
        const TypeDescriptor result = nested.parse();
        alias_stack_.pop_back();
        return result;
    }

    TypeDescriptor parse_primary_type() {
        const std::string base = parse_base_name();
        if (const TypeAliasInfo* alias = find_type_alias(bytecode_, base)) {
            return parse_type_alias(*alias);
        }
        TypeDescriptor desc = resolve_base(base);
        skip_spaces();
        if (peek_char() != '<') {
            return desc;
        }
        advance();
        if (desc.kind == StaticType::List || desc.kind == StaticType::Set) {
            desc.type_args.push_back(parse_union_type());
            skip_spaces();
            expect_char('>');
            return desc;
        }
        if (desc.kind == StaticType::Task) {
            desc.type_args.push_back(parse_union_type());
            skip_spaces();
            expect_char('>');
            return desc;
        }
        if (desc.kind == StaticType::Tuple) {
            if (peek_char() == '>') {
                advance();
                return desc;
            }
            for (;;) {
                desc.type_args.push_back(parse_union_type());
                skip_spaces();
                if (peek_char() == '>') {
                    advance();
                    break;
                }
                expect_char(',');
            }
            return desc;
        }
        if (desc.kind == StaticType::Dict) {
            TypeDescriptor key = parse_union_type();
            if (key.kind != StaticType::String) {
                fail("Type error: map key type must be string");
            }
            skip_spaces();
            expect_char(',');
            TypeDescriptor value = parse_union_type();
            desc.type_args.push_back(std::move(key));
            desc.type_args.push_back(std::move(value));
            skip_spaces();
            expect_char('>');
            return desc;
        }
        if (desc.kind == StaticType::Class || desc.kind == StaticType::Struct) {
            if (peek_char() == '>') {
                advance();
                return desc;
            }
            for (;;) {
                desc.type_args.push_back(parse_union_type());
                skip_spaces();
                if (peek_char() == '>') {
                    advance();
                    break;
                }
                expect_char(',');
            }
            return desc;
        }
        fail("invalid generic type");
    }
};

}  // namespace

TypeDescriptor normalize_union_type(std::vector<TypeDescriptor>&& members) {
    std::vector<TypeDescriptor> flat;
    for (TypeDescriptor& member : members) {
        if (member.kind == StaticType::Union) {
            for (TypeDescriptor& inner : member.type_args) {
                flat.push_back(std::move(inner));
            }
        } else {
            flat.push_back(std::move(member));
        }
    }
    std::vector<TypeDescriptor> unique;
    for (TypeDescriptor& member : flat) {
        bool found = false;
        for (const TypeDescriptor& existing : unique) {
            if (union_member_equal(existing, member)) {
                found = true;
                break;
            }
        }
        if (!found) {
            unique.push_back(std::move(member));
        }
    }
    if (unique.size() == 1) {
        return unique.front();
    }
    TypeDescriptor union_desc;
    union_desc.kind = StaticType::Union;
    union_desc.type_args = std::move(unique);
    return union_desc;
}

bool is_union_member(const TypeDescriptor& union_type, const TypeDescriptor& member) {
    if (union_type.kind != StaticType::Union) {
        return union_member_equal(union_type, member);
    }
    for (const TypeDescriptor& part : union_type.type_args) {
        if (union_member_equal(part, member)) {
            return true;
        }
    }
    return false;
}

std::string type_descriptor_to_string(const TypeDescriptor& desc) {
    if (desc.kind == StaticType::Union) {
        std::ostringstream out;
        for (std::size_t i = 0; i < desc.type_args.size(); ++i) {
            if (i > 0) {
                out << '|';
            }
            out << type_descriptor_to_string(desc.type_args[i]);
        }
        return out.str();
    }
    if (desc.kind == StaticType::TypeParam) {
        return desc.type_param_name ? *desc.type_param_name : "T";
    }
    switch (desc.kind) {
        case StaticType::Integer:
            return "int";
        case StaticType::Float:
            return "float";
        case StaticType::Bool:
            return "bool";
        case StaticType::String:
            return "string";
        case StaticType::Null:
            return "null";
        case StaticType::Any:
            return "any";
        case StaticType::Unknown:
            return "unknown";
        case StaticType::Tuple:
            if (desc.type_args.empty()) {
                return "tuple";
            }
            {
                std::ostringstream out;
                out << "tuple<";
                for (std::size_t i = 0; i < desc.type_args.size(); ++i) {
                    if (i > 0) {
                        out << ',';
                    }
                    out << type_descriptor_to_string(desc.type_args[i]);
                }
                out << '>';
                return out.str();
            }
        case StaticType::Set:
            if (desc.type_args.empty()) {
                return "set";
            }
            return "set<" + type_descriptor_to_string(desc.type_args[0]) + ">";
        case StaticType::Task:
            if (desc.type_args.empty()) {
                return "task";
            }
            return "task<" + type_descriptor_to_string(desc.type_args[0]) + ">";
        case StaticType::Struct:
            return "struct";
        case StaticType::Class:
            if (desc.type_args.empty()) {
                return "class";
            }
            return "class<" + type_descriptor_to_string(desc.type_args[0]) + ">";
        case StaticType::Enum:
            return "enum";
        case StaticType::List:
            if (desc.type_args.empty()) {
                return "list";
            }
            return "list<" + type_descriptor_to_string(desc.type_args[0]) + ">";
        case StaticType::Dict:
            if (desc.type_args.size() < 2) {
                return "map";
            }
            return "map<" + type_descriptor_to_string(desc.type_args[0]) + "," +
                   type_descriptor_to_string(desc.type_args[1]) + ">";
        case StaticType::Union:
            break;
        case StaticType::TypeParam:
            if (desc.type_param_name) {
                return *desc.type_param_name;
            }
            return "typeparam";
    }
    return "unknown";
}

TypeDescriptor parse_type_spec(const std::string& spec, const Location& loc, const Bytecode& bytecode,
                             const std::vector<std::string>* active_type_params) {
    std::vector<std::string> alias_stack;
    TypeSpecParser parser(trim_type_spec(spec), loc, bytecode, active_type_params, alias_stack);
    return parser.parse();
}

}  // namespace pscript
