#include "parser/parser.h"

#include "lexer/keywords.h"

#include "lexer/lexer.h"

#include <algorithm>
#include <unordered_set>
#include <utility>

namespace pscript {

namespace {

void collect_pattern_names(const DestructPattern& pattern, std::vector<std::string>& names) {
    if (pattern.kind == DestructPatternKind::Wildcard) {
        return;
    }
    if (pattern.kind == DestructPatternKind::Identifier || pattern.kind == DestructPatternKind::Rest) {
        names.push_back(pattern.name);
        return;
    }
    if (pattern.kind == DestructPatternKind::Dict) {
        for (const DestructDictEntry& entry : pattern.dict_entries) {
            if (entry.pattern) {
                collect_pattern_names(*entry.pattern, names);
            }
        }
        if (pattern.dict_rest) {
            collect_pattern_names(*pattern.dict_rest, names);
        }
        return;
    }
    for (const auto& element : pattern.elements) {
        if (element) {
            collect_pattern_names(*element, names);
        }
    }
}

int declaration_header_column(int access_column, int keyword_column) {
    if (access_column < 0) {
        return keyword_column;
    }
    return std::min(access_column, keyword_column);
}

}  // namespace

ParserError::ParserError(std::string message, int line, int column)
    : std::runtime_error(message),
      message_(std::move(message)),
      line_(line),
      column_(column) {}

Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
    if (tokens_.empty()) {
        tokens_.push_back(Token{TokenKind::Eof, 1, 1, {}});
    }
}

Program Parser::parse() {
    Program program;
    if (!tokens_.empty()) {
        program.loc = location_from(tokens_.front());
    }
    while (!is_at_end()) {
        program.statements.push_back(parse_statement());
    }
    return program;
}

const Token& Parser::peek(std::size_t ahead) const {
    const std::size_t index = pos_ + ahead;
    if (index >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[index];
}

bool Parser::check(TokenKind kind, std::size_t ahead) const {
    return peek(ahead).kind == kind;
}

bool Parser::is_at_end() const {
    return peek().kind == TokenKind::Eof;
}

Token Parser::advance() {
    Token token = peek();
    if (!is_at_end()) {
        pos_ += 1;
    }
    return token;
}

Token Parser::expect(TokenKind kind, std::string_view what) {
    if (check(kind)) {
        return advance();
    }
    fail("missing expected token '" + std::string(what) + "'");
}

std::unique_ptr<Stmt> Parser::parse_statement() {
    if (check(TokenKind::If)) {
        return parse_if_statement();
    }
    if (check(TokenKind::While)) {
        return parse_while_statement();
    }
    if (check(TokenKind::For)) {
        return parse_for_statement();
    }
    if (check(TokenKind::Break)) {
        return parse_break_statement();
    }
    if (check(TokenKind::Continue)) {
        return parse_continue_statement();
    }
    if (check(TokenKind::Match)) {
        return parse_match_statement();
    }
    if (check(TokenKind::Defer)) {
        return parse_defer_statement();
    }
    if (check(TokenKind::Import)) {
        return parse_import_statement();
    }
    if (check(TokenKind::From)) {
        return parse_import_statement();
    }
    if (check(TokenKind::At)) {
        std::vector<Annotation> leading = parse_annotations();
        MemberAccess access = MemberAccess::Public;
        int access_column = -1;
        if (const std::optional<ModuleAccessPrefix> module_access = try_parse_module_access_modifier()) {
            access = module_access->access;
            access_column = module_access->column;
        }
        if (check(TokenKind::Async) || check(TokenKind::Func)) {
            return parse_function_statement(access, std::move(leading), access_column);
        }
        if (check(TokenKind::Struct) || check(TokenKind::Record)) {
            return parse_struct_statement(access, std::move(leading), access_column);
        }
        if (check(TokenKind::Class)) {
            return parse_class_statement(access, std::move(leading), access_column);
        }
        if (check(TokenKind::Enum)) {
            return parse_enum_statement(access, std::move(leading), access_column);
        }
        if (check(TokenKind::TypeAlias)) {
            return parse_typealias_statement(access, std::move(leading), access_column);
        }
        if (check(TokenKind::Interface) || check(TokenKind::Protocol)) {
            return parse_interface_statement(access, std::move(leading), access_column);
        }
        fail("annotations must precede a declaration");
    }
    if (const std::optional<ModuleAccessPrefix> module_access = try_parse_module_access_modifier()) {
        if (check(TokenKind::Async) || check(TokenKind::Func)) {
            return parse_function_statement(module_access->access, {}, module_access->column);
        }
        if (check(TokenKind::Struct) || check(TokenKind::Record)) {
            return parse_struct_statement(module_access->access, {}, module_access->column);
        }
        if (check(TokenKind::Class)) {
            return parse_class_statement(module_access->access, {}, module_access->column);
        }
        if (check(TokenKind::Enum)) {
            return parse_enum_statement(module_access->access, {}, module_access->column);
        }
        if (looks_like_type_prefix() || check(TokenKind::Identifier)) {
            auto stmt = parse_assign_statement();
            if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
                assign->access = module_access->access;
            }
            return stmt;
        }
        fail("access modifier cannot be used here");
    }
    if (check(TokenKind::Async)) {
        if (check(TokenKind::For, 1)) {
            return parse_async_for_statement();
        }
        if (check(TokenKind::With, 1)) {
            return parse_async_with_statement();
        }
        if (check(TokenKind::Func, 1)) {
            return parse_function_statement();
        }
        fail("invalid async statement");
    }
    if (check(TokenKind::Func)) {
        return parse_function_statement();
    }
    if (check(TokenKind::Struct) || check(TokenKind::Record)) {
        return parse_struct_statement();
    }
    if (check(TokenKind::Class)) {
        return parse_class_statement();
    }
    if (check(TokenKind::Enum)) {
        return parse_enum_statement();
    }
    if (check(TokenKind::TypeAlias)) {
        return parse_typealias_statement();
    }
    if (check(TokenKind::Interface) || check(TokenKind::Protocol)) {
        return parse_interface_statement();
    }
    if (check(TokenKind::Const)) {
        advance();
        auto stmt = parse_assign_statement();
        if (auto* assign = dynamic_cast<AssignStmt*>(stmt.get())) {
            assign->is_const = true;
        } else {
            fail("const requires a variable declaration");
        }
        return stmt;
    }
    if (check(TokenKind::Return)) {
        return parse_return_statement();
    }
    if (check(TokenKind::Try)) {
        return parse_try_statement();
    }
    if (check(TokenKind::Throw)) {
        return parse_throw_statement();
    }
    if (check(TokenKind::Pass)) {
        const Token token = advance();
        auto stmt = std::make_unique<PassStmt>();
        stmt->loc = location_from(token);
        return stmt;
    }
    if (is_elif()) {
        fail("invalid elif");
    }
    if (check(TokenKind::Else)) {
        fail("invalid else");
    }
    if (check(TokenKind::Identifier) && peek().lexeme == "print" && !check(TokenKind::Equal, 1) &&
        !check(TokenKind::Colon, 1) && !check(TokenKind::LeftBracket, 1) && !check(TokenKind::Dot, 1)) {
        return parse_print_statement();
    }
    if (looks_like_destructure_dict_pattern() || looks_like_destructure_struct_pattern() ||
        looks_like_destructure_list_pattern() || looks_like_destructure_tuple_pattern()) {
        return parse_expression_statement();
    }
    if (looks_like_type_prefix()) {
        return parse_assign_statement();
    }
    if (check(TokenKind::Identifier) && check(TokenKind::Colon, 1) && peek().line == peek(1).line) {
        return parse_assign_statement();
    }
    if (can_start_expression()) {
        return parse_expression_statement();
    }
    fail("unexpected token");
}

bool Parser::can_start_expression() const {
    return can_start_expression_kind(peek().kind);
}

bool Parser::is_whitespace_call_callee(std::string_view name) const {
    return is_whitespace_call_builtin(name);
}

bool Parser::can_start_whitespace_call_argument() const {
    if (!can_start_expression_kind(peek().kind)) {
        return false;
    }
    if (check(TokenKind::Equal) || check(TokenKind::Colon) || check(TokenKind::LeftBrace) ||
        check(TokenKind::RightBrace) || check(TokenKind::RightParen) || check(TokenKind::RightBracket) ||
        check(TokenKind::Comma)) {
        return false;
    }
    return true;
}

bool Parser::can_start_expression_kind(TokenKind kind) const {
    switch (kind) {
        case TokenKind::Integer:
        case TokenKind::Float:
        case TokenKind::Duration:
        case TokenKind::String:
        case TokenKind::True:
        case TokenKind::False:
        case TokenKind::Null:
        case TokenKind::Identifier:
        case TokenKind::This:
        case TokenKind::Super:
        case TokenKind::Parent:
        case TokenKind::Input:
        case TokenKind::Not:
        case TokenKind::Bang:
        case TokenKind::Tilde:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::PlusPlus:
        case TokenKind::MinusMinus:
        case TokenKind::Await:
        case TokenKind::LeftParen:
        case TokenKind::LeftBracket:
        case TokenKind::LeftBrace:
            return true;
        default:
            return false;
    }
}

bool Parser::skip_destructure_struct_body_token(std::size_t& index) const {
    if (check(TokenKind::Star, index)) {
        ++index;
        if (!check(TokenKind::Identifier, index)) {
            return false;
        }
        ++index;
        return true;
    }
    if (check(TokenKind::Identifier, index)) {
        ++index;
        if (check(TokenKind::Colon, index)) {
            ++index;
            return skip_destructure_pattern_token(index);
        }
        return true;
    }
    return skip_destructure_pattern_token(index);
}

bool Parser::skip_destructure_dict_pattern_token(std::size_t& index) const {
    if (!check(TokenKind::LeftBrace, index)) {
        return false;
    }
    ++index;
    if (check(TokenKind::RightBrace, index)) {
        return false;
    }
    for (;;) {
        if (check(TokenKind::Star, index)) {
            ++index;
            if (!check(TokenKind::Identifier, index)) {
                return false;
            }
            ++index;
            if (!check(TokenKind::RightBrace, index)) {
                return false;
            }
            ++index;
            return true;
        }
        if (!check(TokenKind::String, index)) {
            return false;
        }
        ++index;
        if (!check(TokenKind::Colon, index)) {
            return false;
        }
        ++index;
        if (!skip_destructure_pattern_token(index)) {
            return false;
        }
        if (check(TokenKind::Comma, index)) {
            ++index;
            if (check(TokenKind::RightBrace, index)) {
                return false;
            }
            continue;
        }
        if (check(TokenKind::RightBrace, index)) {
            ++index;
            return true;
        }
        return false;
    }
}

bool Parser::skip_destructure_pattern_token(std::size_t& index) const {
    if (check(TokenKind::LeftBrace, index)) {
        return skip_destructure_dict_pattern_token(index);
    }
    if (check(TokenKind::Identifier, index) && check(TokenKind::LeftBrace, index + 1)) {
        ++index;
        ++index;
        if (check(TokenKind::RightBrace, index)) {
            return false;
        }
        for (;;) {
            if (!skip_destructure_struct_body_token(index)) {
                return false;
            }
            if (check(TokenKind::Comma, index)) {
                ++index;
                if (check(TokenKind::RightBrace, index)) {
                    return false;
                }
                continue;
            }
            if (check(TokenKind::RightBrace, index)) {
                ++index;
                return true;
            }
            return false;
        }
    }
    if (check(TokenKind::Star, index)) {
        ++index;
        if (!check(TokenKind::Identifier, index)) {
            return false;
        }
        ++index;
        return true;
    }
    if (check(TokenKind::Identifier, index)) {
        ++index;
        return true;
    }
    if (check(TokenKind::LeftBracket, index)) {
        ++index;
        if (check(TokenKind::RightBracket, index)) {
            return false;
        }
        for (;;) {
            if (!skip_destructure_pattern_token(index)) {
                return false;
            }
            if (check(TokenKind::Comma, index)) {
                ++index;
                if (check(TokenKind::RightBracket, index)) {
                    return false;
                }
                continue;
            }
            if (check(TokenKind::RightBracket, index)) {
                ++index;
                return true;
            }
            return false;
        }
    }
    if (check(TokenKind::LeftParen, index)) {
        ++index;
        if (check(TokenKind::RightParen, index)) {
            return false;
        }
        for (;;) {
            if (!skip_destructure_pattern_token(index)) {
                return false;
            }
            if (check(TokenKind::Comma, index)) {
                ++index;
                if (check(TokenKind::RightParen, index)) {
                    return false;
                }
                continue;
            }
            if (check(TokenKind::RightParen, index)) {
                ++index;
                return true;
            }
            return false;
        }
    }
    return false;
}

bool Parser::looks_like_destructure_list_pattern() const {
    if (!check(TokenKind::LeftBracket)) {
        return false;
    }
    std::size_t i = 1;
    if (check(TokenKind::RightBracket, i)) {
        return false;
    }
    for (;;) {
        if (!skip_destructure_pattern_token(i)) {
            return false;
        }
        if (check(TokenKind::Comma, i)) {
            ++i;
            if (check(TokenKind::RightBracket, i)) {
                return false;
            }
            continue;
        }
        if (check(TokenKind::RightBracket, i)) {
            ++i;
            break;
        }
        return false;
    }
    return check(TokenKind::Equal, i);
}

bool Parser::looks_like_destructure_tuple_pattern() const {
    if (!check(TokenKind::LeftParen)) {
        return false;
    }
    std::size_t i = 1;
    if (check(TokenKind::RightParen, i)) {
        return false;
    }
    for (;;) {
        if (!skip_destructure_pattern_token(i)) {
            return false;
        }
        if (check(TokenKind::Comma, i)) {
            ++i;
            if (check(TokenKind::RightParen, i)) {
                return false;
            }
            continue;
        }
        if (check(TokenKind::RightParen, i)) {
            ++i;
            break;
        }
        return false;
    }
    return check(TokenKind::Equal, i);
}

bool Parser::looks_like_destructure_dict_pattern() const {
    if (!check(TokenKind::LeftBrace)) {
        return false;
    }
    std::size_t i = 0;
    if (!skip_destructure_dict_pattern_token(i)) {
        return false;
    }
    return check(TokenKind::Equal, i);
}

bool Parser::looks_like_destructure_struct_pattern() const {
    if (!check(TokenKind::Identifier)) {
        return false;
    }
    std::size_t i = 1;
    if (check(TokenKind::Less, i)) {
        ++i;
        int depth = 1;
        while (depth > 0) {
            if (pos_ + i >= tokens_.size()) {
                return false;
            }
            const TokenKind kind = peek(i).kind;
            if (kind == TokenKind::Less) {
                ++depth;
            } else if (kind == TokenKind::Greater || kind == TokenKind::GreaterGreater) {
                --depth;
            }
            ++i;
        }
    }
    if (!check(TokenKind::LeftBrace, i)) {
        return false;
    }
    ++i;
    if (check(TokenKind::RightBrace, i)) {
        return false;
    }
    for (;;) {
        if (!skip_destructure_struct_body_token(i)) {
            return false;
        }
        if (check(TokenKind::Comma, i)) {
            ++i;
            if (check(TokenKind::RightBrace, i)) {
                return false;
            }
            continue;
        }
        if (check(TokenKind::RightBrace, i)) {
            ++i;
            break;
        }
        return false;
    }
    return check(TokenKind::Equal, i);
}

void Parser::validate_destructure_pattern(const DestructPattern& pattern) {
    if (pattern.kind == DestructPatternKind::Dict) {
        if (pattern.dict_rest) {
            if (pattern.dict_rest->kind != DestructPatternKind::Rest) {
                fail("invalid destructuring pattern", pattern.loc);
            }
        }
        for (const DestructDictEntry& entry : pattern.dict_entries) {
            if (!entry.pattern) {
                fail("invalid destructuring pattern", entry.loc);
            }
            validate_destructure_pattern(*entry.pattern);
        }
        return;
    }
    if (pattern.kind == DestructPatternKind::Struct) {
        bool saw_rest = false;
        for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
            const auto& element = pattern.elements[i];
            if (!element) {
                fail("invalid destructuring pattern", pattern.loc);
            }
            if (element->kind == DestructPatternKind::Rest) {
                if (saw_rest) {
                    fail("multiple rest patterns in destructuring assignment", element->loc);
                }
                saw_rest = true;
                if (i + 1 != pattern.elements.size()) {
                    fail("rest pattern must be last in destructuring assignment", element->loc);
                }
            } else {
                validate_destructure_pattern(*element);
            }
        }
        return;
    }
    if (pattern.kind != DestructPatternKind::List && pattern.kind != DestructPatternKind::Tuple) {
        return;
    }
    bool saw_rest = false;
    for (std::size_t i = 0; i < pattern.elements.size(); ++i) {
        const auto& element = pattern.elements[i];
        if (!element) {
            fail("invalid destructuring pattern", pattern.loc);
        }
        if (element->kind == DestructPatternKind::Rest) {
            if (saw_rest) {
                fail("multiple rest patterns in destructuring assignment", element->loc);
            }
            saw_rest = true;
            if (i + 1 != pattern.elements.size()) {
                fail("rest pattern must be last in destructuring assignment", element->loc);
            }
        }
        if (element->kind == DestructPatternKind::List || element->kind == DestructPatternKind::Tuple ||
            element->kind == DestructPatternKind::Dict || element->kind == DestructPatternKind::Struct) {
            validate_destructure_pattern(*element);
        }
    }
}

std::unique_ptr<DestructPattern> Parser::parse_destructure_dict_pattern() {
    const Token open = expect(TokenKind::LeftBrace, "{");
    auto pattern = std::make_unique<DestructPattern>();
    pattern->kind = DestructPatternKind::Dict;
    pattern->loc = location_from(open);
    if (check(TokenKind::RightBrace)) {
        fail("invalid destructuring pattern", open);
    }
    for (;;) {
        if (check(TokenKind::Star)) {
            const Token star = advance();
            if (!check(TokenKind::Identifier)) {
                fail("invalid destructuring pattern", star);
            }
            const Token name = advance();
            if (name.lexeme == "_") {
                fail("invalid destructuring pattern", name);
            }
            auto rest = std::make_unique<DestructPattern>();
            rest->kind = DestructPatternKind::Rest;
            rest->loc = location_from(star);
            rest->name = name.lexeme;
            pattern->dict_rest = std::move(rest);
            break;
        }
        if (!check(TokenKind::String)) {
            fail("invalid destructuring pattern", open);
        }
        const Token key = advance();
        expect(TokenKind::Colon, ":");
        DestructDictEntry entry;
        entry.key = key.lexeme;
        entry.loc = location_from(key);
        entry.pattern = parse_destructure_pattern_element();
        pattern->dict_entries.push_back(std::move(entry));
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::RightBrace)) {
            fail("invalid destructuring pattern", comma);
        }
        if (pattern->dict_rest) {
            fail("rest pattern must be last in destructuring assignment", comma);
        }
    }
    expect(TokenKind::RightBrace, "}");
    validate_destructure_pattern(*pattern);
    return pattern;
}

std::unique_ptr<DestructPattern> Parser::parse_destructure_struct_pattern_element() {
    if (check(TokenKind::Star)) {
        return parse_destructure_pattern_element();
    }
    if (check(TokenKind::Identifier)) {
        const Token field = advance();
        if (field.lexeme == "_") {
            fail("invalid destructuring pattern", field);
        }
        if (check(TokenKind::Colon)) {
            advance();
            auto nested = parse_destructure_pattern_element();
            nested->struct_field_name = field.lexeme;
            return nested;
        }
        auto leaf = std::make_unique<DestructPattern>();
        leaf->kind = DestructPatternKind::Identifier;
        leaf->loc = location_from(field);
        leaf->name = field.lexeme;
        return leaf;
    }
    return parse_destructure_pattern_element();
}

std::unique_ptr<DestructPattern> Parser::parse_destructure_struct_pattern(const std::string& type_name,
                                                                          const Location& loc) {
    expect(TokenKind::LeftBrace, "{");
    auto pattern = std::make_unique<DestructPattern>();
    pattern->kind = DestructPatternKind::Struct;
    pattern->loc = loc;
    pattern->struct_type_name = type_name;
    if (check(TokenKind::RightBrace)) {
        fail("invalid destructuring pattern", loc);
    }
    for (;;) {
        pattern->elements.push_back(parse_destructure_struct_pattern_element());
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::RightBrace)) {
            fail("invalid destructuring pattern", comma);
        }
        if (pattern->elements.back() && pattern->elements.back()->kind == DestructPatternKind::Rest) {
            fail("rest pattern must be last in destructuring assignment", comma);
        }
    }
    expect(TokenKind::RightBrace, "}");
    validate_destructure_pattern(*pattern);
    return pattern;
}

std::unique_ptr<DestructPattern> Parser::parse_parameter_destructure_pattern() {
    if (check(TokenKind::LeftBrace)) {
        return parse_destructure_dict_pattern();
    }
    if (check(TokenKind::LeftBracket)) {
        return parse_destructure_list_pattern();
    }
    if (check(TokenKind::LeftParen)) {
        return parse_destructure_tuple_pattern();
    }
    if (check(TokenKind::Identifier)) {
        const Token type = advance();
        if (check(TokenKind::LeftBrace) && type.line == peek().line) {
            return parse_destructure_struct_pattern(type.lexeme, location_from(type));
        }
        fail("invalid destructuring pattern", type);
    }
    fail("invalid destructuring pattern");
}

std::unique_ptr<DestructPattern> Parser::parse_destructure_pattern_element() {
    if (check(TokenKind::LeftBrace)) {
        return parse_destructure_dict_pattern();
    }
    if (check(TokenKind::Identifier) &&
        (check(TokenKind::LeftBrace, 1) || (check(TokenKind::Less, 1) && peek(1).line == peek().line)) &&
        peek().line == peek(1).line) {
        const Location type_loc = location_from(peek());
        std::string type_name;
        if (check(TokenKind::Less, 1)) {
            type_name = parse_type_annotation_primary();
        } else {
            type_name = advance().lexeme;
        }
        return parse_destructure_struct_pattern(type_name, type_loc);
    }
    if (check(TokenKind::LeftParen)) {
        return parse_destructure_tuple_pattern();
    }
    if (check(TokenKind::LeftBracket)) {
        return parse_destructure_list_pattern();
    }
    if (check(TokenKind::Star)) {
        const Token star = advance();
        if (!check(TokenKind::Identifier)) {
            fail("invalid destructuring pattern", star);
        }
        const Token name = advance();
        if (name.lexeme == "_") {
            fail("invalid destructuring pattern", name);
        }
        auto leaf = std::make_unique<DestructPattern>();
        leaf->kind = DestructPatternKind::Rest;
        leaf->loc = location_from(star);
        leaf->name = name.lexeme;
        return leaf;
    }
    if (check(TokenKind::Integer) || check(TokenKind::Float) || check(TokenKind::String) || check(TokenKind::True) ||
        check(TokenKind::False) || check(TokenKind::Null)) {
        auto leaf = std::make_unique<DestructPattern>();
        leaf->kind = DestructPatternKind::Literal;
        leaf->loc = location_from(peek());
        leaf->literal_expr = parse_match_literal();
        return leaf;
    }
    if (check(TokenKind::Identifier)) {
        const Token name = advance();
        auto leaf = std::make_unique<DestructPattern>();
        leaf->loc = location_from(name);
        if (name.lexeme == "_") {
            leaf->kind = DestructPatternKind::Wildcard;
            return leaf;
        }
        leaf->kind = DestructPatternKind::Identifier;
        leaf->name = name.lexeme;
        return leaf;
    }
    fail("invalid destructuring pattern");
}

std::unique_ptr<DestructPattern> Parser::parse_destructure_list_pattern() {
    const Token open = expect(TokenKind::LeftBracket, "[");
    auto pattern = std::make_unique<DestructPattern>();
    pattern->kind = DestructPatternKind::List;
    pattern->loc = location_from(open);
    if (check(TokenKind::RightBracket)) {
        fail("invalid destructuring pattern", open);
    }
    for (;;) {
        pattern->elements.push_back(parse_destructure_pattern_element());
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::RightBracket)) {
            fail("invalid destructuring pattern", comma);
        }
        if (pattern->elements.back() && pattern->elements.back()->kind == DestructPatternKind::Rest) {
            fail("rest pattern must be last in destructuring assignment", comma);
        }
    }
    expect(TokenKind::RightBracket, "]");
    validate_destructure_pattern(*pattern);
    return pattern;
}

std::unique_ptr<DestructPattern> Parser::parse_destructure_tuple_pattern() {
    const Token open = expect(TokenKind::LeftParen, "(");
    auto pattern = std::make_unique<DestructPattern>();
    pattern->kind = DestructPatternKind::Tuple;
    pattern->loc = location_from(open);
    if (check(TokenKind::RightParen)) {
        fail("invalid destructuring pattern", open);
    }
    for (;;) {
        pattern->elements.push_back(parse_destructure_pattern_element());
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::RightParen)) {
            fail("invalid destructuring pattern", comma);
        }
        if (pattern->elements.back() && pattern->elements.back()->kind == DestructPatternKind::Rest) {
            fail("rest pattern must be last in destructuring assignment", comma);
        }
    }
    expect(TokenKind::RightParen, ")");
    validate_destructure_pattern(*pattern);
    return pattern;
}

std::unique_ptr<Stmt> Parser::parse_expression_statement() {
    if (looks_like_destructure_dict_pattern()) {
        auto pattern = parse_destructure_dict_pattern();
        expect(TokenKind::Equal, "=");
        auto stmt = std::make_unique<DestructureAssignStmt>();
        stmt->loc = pattern->loc;
        stmt->pattern = std::move(pattern);
        stmt->value = parse_expression();
        return stmt;
    }
    if (looks_like_destructure_struct_pattern()) {
        const Location type_loc = location_from(peek());
        std::string type_name;
        if (check(TokenKind::Less, 1) && peek(1).line == peek().line) {
            type_name = parse_type_annotation_primary();
        } else {
            type_name = advance().lexeme;
        }
        auto pattern = parse_destructure_struct_pattern(type_name, type_loc);
        expect(TokenKind::Equal, "=");
        auto stmt = std::make_unique<DestructureAssignStmt>();
        stmt->loc = pattern->loc;
        stmt->pattern = std::move(pattern);
        stmt->value = parse_expression();
        return stmt;
    }
    if (looks_like_destructure_list_pattern()) {
        auto pattern = parse_destructure_list_pattern();
        expect(TokenKind::Equal, "=");
        auto stmt = std::make_unique<DestructureAssignStmt>();
        stmt->loc = pattern->loc;
        stmt->pattern = std::move(pattern);
        stmt->value = parse_expression();
        return stmt;
    }
    if (looks_like_destructure_tuple_pattern()) {
        auto pattern = parse_destructure_tuple_pattern();
        expect(TokenKind::Equal, "=");
        auto stmt = std::make_unique<DestructureAssignStmt>();
        stmt->loc = pattern->loc;
        stmt->pattern = std::move(pattern);
        stmt->value = parse_expression();
        return stmt;
    }
    auto expr = parse_expression();
    if (auto* assign = dynamic_cast<AssignExpr*>(expr.get())) {
        if (assign->op == TokenKind::Equal) {
            if (const auto* ident = dynamic_cast<IdentifierExpr*>(assign->target.get())) {
                auto stmt = std::make_unique<AssignStmt>();
                stmt->loc = ident->loc;
                stmt->name = ident->name;
                stmt->value = std::move(assign->value);
                return stmt;
            }
            if (dynamic_cast<const FieldAccessExpr*>(assign->target.get())) {
                auto stmt = std::make_unique<ExprStmt>();
                stmt->loc = expr->loc;
                stmt->expression = std::move(expr);
                return stmt;
            }
            auto* index = dynamic_cast<IndexExpr*>(assign->target.get());
            if (!index || !index->object || !index->index || !assign->value) {
                fail("invalid assignment");
            }
            auto stmt = std::make_unique<IndexAssignStmt>();
            stmt->loc = expr->loc;
            stmt->object = std::move(index->object);
            stmt->index = std::move(index->index);
            stmt->value = std::move(assign->value);
            return stmt;
        }
        auto stmt = std::make_unique<ExprStmt>();
        stmt->loc = expr->loc;
        stmt->expression = std::move(expr);
        return stmt;
    }
    if (dynamic_cast<UpdateExpr*>(expr.get()) || dynamic_cast<MethodCallExpr*>(expr.get()) ||
        dynamic_cast<CallExpr*>(expr.get()) || dynamic_cast<AwaitExpr*>(expr.get()) ||
        function_depth_ > 0) {
        auto stmt = std::make_unique<ExprStmt>();
        stmt->loc = expr->loc;
        stmt->expression = std::move(expr);
        return stmt;
    }
    Token loc_tok;
    loc_tok.line = expr->loc.line;
    loc_tok.column = expr->loc.column;
    fail("invalid statement", loc_tok);
}

std::unique_ptr<Stmt> Parser::parse_if_statement() {
    const Token if_tok = advance();
    auto stmt = std::make_unique<IfStmt>();
    stmt->loc = location_from(if_tok);
    stmt->condition = parse_if_condition();
    stmt->body = parse_block(if_tok.column);

    while (is_elif()) {
        if (!can_continue_branch(if_tok.column)) {
            fail("invalid indent");
        }
        const Token branch_tok = advance();
        ElifBranch branch;
        branch.loc = location_from(branch_tok);
        branch.condition = parse_if_condition();
        branch.body = parse_block(if_tok.column);
        stmt->elif_branches.push_back(std::move(branch));
    }

    if (check(TokenKind::Else)) {
        if (!can_continue_branch(if_tok.column)) {
            fail("invalid indent");
        }
        advance();
        if (check(TokenKind::If)) {
            fail("else if is not supported");
        }
        stmt->else_body = parse_block(if_tok.column);
    }
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_while_statement() {
    const Token while_tok = advance();
    auto stmt = std::make_unique<WhileStmt>();
    stmt->loc = location_from(while_tok);
    stmt->condition = parse_if_condition();
    stmt->body = parse_block(while_tok.column);
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_for_statement() {
    const Token for_tok = advance();
    auto stmt = std::make_unique<ForStmt>();
    stmt->loc = location_from(for_tok);

    const bool parenthesized = check(TokenKind::LeftParen);
    if (parenthesized) {
        advance();
    }
    if (!check(TokenKind::Identifier)) {
        fail("missing expected token 'loop variable'");
    }
    stmt->item = advance().lexeme;
    expect(TokenKind::In, "in");
    if (check(TokenKind::Colon) || check(TokenKind::RightParen) || is_at_end() ||
        (check(TokenKind::LeftBrace) && !looks_like_dict())) {
        fail("missing collection");
    }
    stmt->iterable = parse_expression();
    if (parenthesized) {
        expect(TokenKind::RightParen, ")");
    }
    stmt->body = parse_block(for_tok.column);
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_async_for_statement() {
    const Token async_tok = advance();
    const Token for_tok = expect(TokenKind::For, "for");
    auto stmt = std::make_unique<AsyncForStmt>();
    stmt->loc = location_from(async_tok);

    const bool parenthesized = check(TokenKind::LeftParen);
    if (parenthesized) {
        advance();
    }
    if (!check(TokenKind::Identifier)) {
        fail("missing expected token 'loop variable'");
    }
    stmt->item = advance().lexeme;
    expect(TokenKind::In, "in");
    if (check(TokenKind::Colon) || check(TokenKind::RightParen) || is_at_end() ||
        (check(TokenKind::LeftBrace) && !looks_like_dict())) {
        fail("missing collection");
    }
    stmt->iterable = parse_expression();
    if (parenthesized) {
        expect(TokenKind::RightParen, ")");
    }
    stmt->body = parse_block(async_tok.column);
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_async_with_statement() {
    const Token async_tok = advance();
    const Token with_tok = expect(TokenKind::With, "with");
    auto stmt = std::make_unique<AsyncWithStmt>();
    stmt->loc = location_from(async_tok);
    stmt->manager = parse_expression();
    if (check(TokenKind::As)) {
        advance();
        stmt->binding = expect(TokenKind::Identifier, "identifier").lexeme;
    }
    stmt->body = parse_block(async_tok.column);
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_break_statement() {
    const Token token = advance();
    auto stmt = std::make_unique<BreakStmt>();
    stmt->loc = location_from(token);
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_continue_statement() {
    const Token token = advance();
    auto stmt = std::make_unique<ContinueStmt>();
    stmt->loc = location_from(token);
    return stmt;
}

bool Parser::is_module_declaration_after_access(std::size_t ahead) const {
    if (pos_ + ahead >= tokens_.size()) {
        return false;
    }
    const TokenKind kind = tokens_[pos_ + ahead].kind;
    return kind == TokenKind::Func || kind == TokenKind::Class || kind == TokenKind::Struct ||
           kind == TokenKind::Record || kind == TokenKind::Enum || kind == TokenKind::Identifier;
}

std::optional<ModuleAccessPrefix> Parser::try_parse_module_access_modifier() {
    if (!is_access_modifier(peek().kind) || !is_module_declaration_after_access(1)) {
        return std::nullopt;
    }
    const int column = peek().column;
    const std::optional<MemberAccess> access = parse_member_access_modifier();
    if (!access) {
        return std::nullopt;
    }
    return ModuleAccessPrefix{*access, column};
}

std::unique_ptr<Stmt> Parser::parse_struct_statement(MemberAccess access,
                                                     std::vector<Annotation> leading_annotations, int access_column) {
    std::vector<Annotation> annotations =
        leading_annotations.empty() ? parse_annotations() : std::move(leading_annotations);
    const bool is_record = check(TokenKind::Record);
    const Token keyword = advance();
    auto stmt = std::make_unique<StructStmt>();
    stmt->loc = location_from(keyword);
    stmt->access = access;
    stmt->is_record = is_record;
    stmt->annotations = std::move(annotations);
    if (!check(TokenKind::Identifier)) {
        fail("invalid struct syntax");
    }
    stmt->name = advance().lexeme;
    stmt->type_params = parse_generic_type_params();
    stmt->fields = parse_struct_body(declaration_header_column(access_column, keyword.column));
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_class_statement(MemberAccess access,
                                                      std::vector<Annotation> leading_annotations,
                                                      int access_column) {
    std::vector<Annotation> annotations =
        leading_annotations.empty() ? parse_annotations() : std::move(leading_annotations);
    const Token keyword = advance();
    auto stmt = std::make_unique<ClassStmt>();
    stmt->loc = location_from(keyword);
    stmt->access = access;
    stmt->annotations = std::move(annotations);
    if (!check(TokenKind::Identifier)) {
        fail("invalid class syntax");
    }
    stmt->name = advance().lexeme;
    stmt->type_params = parse_generic_type_params();
    if (check(TokenKind::Colon) && check(TokenKind::Identifier, 1) && peek().line == peek(1).line) {
        if ((check(TokenKind::Colon, 2) && peek(2).line == peek().line) ||
            (check(TokenKind::Less, 2) && peek(2).line == peek().line) ||
            (check(TokenKind::LeftBrace, 2) && peek(2).line == peek().line) ||
            (check(TokenKind::Comma, 2) && peek(2).line == peek().line)) {
            advance();
            const Token first_base = peek();
            for (;;) {
                stmt->base_type_specs.push_back(parse_type_annotation_string());
                if (!check(TokenKind::Comma) || peek().line != first_base.line) {
                    break;
                }
                advance();
            }
            if (!stmt->base_type_specs.empty()) {
                stmt->parent_name = stmt->base_type_specs.front();
            }
        }
    }
    if (check(TokenKind::Identifier) && peek().lexeme == "implements") {
        advance();
        for (;;) {
            stmt->implements.push_back(expect(TokenKind::Identifier, "interface name").lexeme);
            stmt->base_type_specs.push_back(stmt->implements.back());
            if (!check(TokenKind::Comma)) {
                break;
            }
            advance();
        }
    }
    parse_class_body(*stmt, declaration_header_column(access_column, keyword.column));
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_enum_statement(MemberAccess access,
                                                   std::vector<Annotation> leading_annotations, int access_column) {
    std::vector<Annotation> annotations =
        leading_annotations.empty() ? parse_annotations() : std::move(leading_annotations);
    const Token keyword = advance();
    auto stmt = std::make_unique<EnumStmt>();
    stmt->loc = location_from(keyword);
    stmt->access = access;
    stmt->annotations = std::move(annotations);
    if (!check(TokenKind::Identifier)) {
        fail("invalid enum syntax");
    }
    stmt->name = advance().lexeme;
    parse_enum_body(declaration_header_column(access_column, keyword.column), stmt->members, stmt->member_payloads,
                    stmt->member_annotations);
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_typealias_statement(MemberAccess access,
                                                        std::vector<Annotation> leading_annotations, int access_column) {
    std::vector<Annotation> annotations =
        leading_annotations.empty() ? parse_annotations() : std::move(leading_annotations);
    const Token keyword = advance();
    auto stmt = std::make_unique<TypeAliasStmt>();
    stmt->loc = location_from(keyword);
    stmt->access = access;
    stmt->annotations = std::move(annotations);
    if (!check(TokenKind::Identifier)) {
        fail("invalid typealias syntax");
    }
    stmt->name = advance().lexeme;
    stmt->type_params = parse_generic_type_params();
    expect(TokenKind::Equal, "=");
    stmt->target_type = parse_type_annotation_string();
    return stmt;
}

StructField Parser::parse_class_field(std::unordered_set<std::string>& seen) {
    StructField field;
    field.loc = location_from(peek());
    if (const std::optional<MemberAccess> access = parse_member_access_modifier()) {
        field.access = *access;
    } else {
        field.access = MemberAccess::Private;
    }
    if (looks_like_type_prefix()) {
        field.type_name = parse_type_annotation_string();
        field.name = expect(TokenKind::Identifier, "identifier").lexeme;
    } else if (check(TokenKind::Identifier) && check(TokenKind::Colon, 1) && peek().line == peek(1).line) {
        field.name = advance().lexeme;
        advance();
        field.type_name = parse_type_annotation_string();
    } else if (check(TokenKind::Identifier)) {
        field.name = advance().lexeme;
    } else {
        fail("invalid class field");
    }
    if (!seen.insert(field.name).second) {
        fail("duplicate field '" + field.name + "'", field.loc);
    }
    return field;
}

bool Parser::looks_like_property_member() const {
    std::size_t index = pos_;
    if (index >= tokens_.size()) {
        return false;
    }
    if (is_access_modifier(tokens_[index].kind)) {
        ++index;
    }
    if (index >= tokens_.size() || tokens_[index].kind != TokenKind::Identifier) {
        return false;
    }
    const std::string& word = tokens_[index].lexeme;
    return word == "get" || word == "set";
}

ClassProperty Parser::parse_class_property(std::unordered_set<std::string>& /*seen_properties*/) {
    ClassProperty prop;
    const Location property_loc = location_from(peek());
    MemberAccess access = MemberAccess::Private;
    if (const std::optional<MemberAccess> modifier = parse_member_access_modifier()) {
        access = *modifier;
    }
    if (!check(TokenKind::Identifier)) {
        fail("invalid property");
    }
    const std::string kind = advance().lexeme;
    if (kind != "get" && kind != "set") {
        fail("invalid property");
    }
    const bool is_getter = kind == "get";
    prop.name = expect(TokenKind::Identifier, "property name").lexeme;
    const int block_column = property_loc.column;
    expect(TokenKind::LeftParen, "(");
    if (!is_getter) {
        auto setter = std::make_unique<FuncStmt>();
        setter->loc = property_loc;
        setter->name = prop.name;
        setter->access = access;
        Parameter param;
        param.loc = location_from(peek());
        param.name = expect(TokenKind::Identifier, "setter parameter").lexeme;
        if (check(TokenKind::Colon)) {
            advance();
            param.type_name = parse_type_annotation_string();
        }
        setter->parameters.push_back(std::move(param));
        expect(TokenKind::RightParen, ")");
        prop.setter_access = access;
        ++function_depth_;
        setter->body = parse_block(block_column);
        --function_depth_;
        prop.setter = std::move(setter);
        return prop;
    }
    expect(TokenKind::RightParen, ")");
    prop.getter_access = access;
    auto getter = std::make_unique<FuncStmt>();
    getter->loc = property_loc;
    getter->name = prop.name;
    getter->access = access;
    ++function_depth_;
    getter->body = parse_block(block_column);
    --function_depth_;
    prop.getter = std::move(getter);
    return prop;
}

std::unique_ptr<FuncStmt> Parser::parse_class_constructor(int /*header_column*/) {
    if (check(TokenKind::Async)) {
        fail("async constructor is not supported");
    }
    const Token ctor_tok = advance();
    auto stmt = std::make_unique<FuncStmt>();
    stmt->loc = location_from(ctor_tok);
    stmt->name = "constructor";
    expect(TokenKind::LeftParen, "(");
    stmt->parameters = parse_parameters();
    expect(TokenKind::RightParen, ")");
    ++function_depth_;
    stmt->body = parse_block(ctor_tok.column);
    --function_depth_;
    return stmt;
}

std::unique_ptr<FuncStmt> Parser::parse_class_method(int /*header_column*/) {
    bool is_async = false;
    const int block_column = peek().column;
    MemberAccess access = MemberAccess::Private;
    if (const std::optional<MemberAccess> modifier = parse_member_access_modifier()) {
        access = *modifier;
    }
    if (check(TokenKind::Async)) {
        is_async = true;
        advance();
    }
    const Token func_tok = advance();
    auto stmt = std::make_unique<FuncStmt>();
    stmt->loc = location_from(func_tok);
    stmt->is_async = is_async;
    stmt->access = access;
    if (!check(TokenKind::Identifier)) {
        fail("invalid method syntax");
    }
    stmt->name = advance().lexeme;
    expect(TokenKind::LeftParen, "(");
    stmt->parameters = parse_parameters();
    expect(TokenKind::RightParen, ")");
    if (check(TokenKind::MinusGreater)) {
        advance();
        stmt->return_type_name = parse_type_annotation_string();
    }
    ++function_depth_;
    if (is_async) {
        ++async_function_depth_;
    }
    stmt->body = parse_block(block_column);
    if (is_async) {
        --async_function_depth_;
    }
    --function_depth_;
    return stmt;
}

void Parser::parse_class_body(ClassStmt& stmt, int header_column) {
    std::unordered_set<std::string> seen_fields;
    std::unordered_set<std::string> seen_methods;
    std::unordered_set<std::string> seen_properties;
    bool has_constructor = false;

    if (check(TokenKind::Colon)) {
        const Token colon = advance();
        if (is_at_end() || peek().line <= colon.line) {
            fail("missing indent");
        }
        const int member_indent = peek().column;
        while (!is_at_end()) {
            const int column = peek().column;
            if (column < member_indent) {
                break;
            }
            if (column > member_indent) {
                fail("unexpected indent");
            }
            if (check(TokenKind::Constructor)) {
                if (has_constructor) {
                    fail("duplicate constructor");
                }
                stmt.constructor = parse_class_constructor(header_column);
                has_constructor = true;
                continue;
            }
            if (check(TokenKind::Async) && check(TokenKind::Constructor, 1)) {
                fail("async constructor is not supported");
            }
            if (check(TokenKind::Func) || check(TokenKind::Async)) {
                auto method = parse_class_method(header_column);
                if (!seen_methods.insert(method->name).second) {
                    fail("duplicate method '" + method->name + "'", method->loc);
                }
                stmt.methods.push_back(std::move(method));
                continue;
            }
            if (check(TokenKind::Pass)) {
                advance();
                continue;
            }
            if (looks_like_property_member()) {
                stmt.properties.push_back(parse_class_property(seen_properties));
                continue;
            }
            if (is_access_modifier(peek().kind) || check(TokenKind::Identifier)) {
                StructField field = parse_class_field(seen_fields);
                stmt.fields.push_back(std::move(field));
                continue;
            }
            fail("invalid class member");
        }
        return;
    }
    if (check(TokenKind::LeftBrace)) {
        if (looks_like_dict()) {
            fail("invalid class syntax");
        }
        const Token brace = advance();
        if (is_at_end()) {
            fail("missing class members");
        }
        const int member_indent = peek().column;
        while (!check(TokenKind::RightBrace)) {
            const int column = peek().column;
            if (column != member_indent) {
                if (column > member_indent) {
                    fail("unexpected indent");
                }
                fail("invalid indent");
            }
            if (check(TokenKind::Constructor)) {
                if (has_constructor) {
                    fail("duplicate constructor");
                }
                stmt.constructor = parse_class_constructor(header_column);
                has_constructor = true;
                continue;
            }
            if (check(TokenKind::Async) && check(TokenKind::Constructor, 1)) {
                fail("async constructor is not supported");
            }
            if (check(TokenKind::Func) || check(TokenKind::Async)) {
                auto method = parse_class_method(header_column);
                if (!seen_methods.insert(method->name).second) {
                    fail("duplicate method '" + method->name + "'", method->loc);
                }
                stmt.methods.push_back(std::move(method));
                continue;
            }
            if (check(TokenKind::Pass)) {
                advance();
                continue;
            }
            if (looks_like_property_member()) {
                stmt.properties.push_back(parse_class_property(seen_properties));
                continue;
            }
            if (is_access_modifier(peek().kind) || check(TokenKind::Identifier)) {
                StructField field = parse_class_field(seen_fields);
                stmt.fields.push_back(std::move(field));
                continue;
            }
            fail("invalid class member");
        }
        expect(TokenKind::RightBrace, "}");
        return;
    }
    fail("missing block");
}

bool Parser::is_access_modifier(TokenKind kind) const {
    return kind == TokenKind::Public || kind == TokenKind::Pub || kind == TokenKind::Private ||
           kind == TokenKind::Priv || kind == TokenKind::Protected || kind == TokenKind::Prot;
}

std::optional<MemberAccess> Parser::parse_member_access_modifier() {
    if (check(TokenKind::Public) || check(TokenKind::Pub)) {
        advance();
        return MemberAccess::Public;
    }
    if (check(TokenKind::Private) || check(TokenKind::Priv)) {
        advance();
        return MemberAccess::Private;
    }
    if (check(TokenKind::Protected) || check(TokenKind::Prot)) {
        advance();
        return MemberAccess::Protected;
    }
    return std::nullopt;
}

StructField Parser::parse_struct_field(std::unordered_set<std::string>& seen) {
    StructField field;
    field.loc = location_from(peek());
    if (const std::optional<MemberAccess> access = parse_member_access_modifier()) {
        field.access = *access;
    }
    if (looks_like_type_prefix()) {
        field.type_name = parse_type_annotation_string();
        field.name = expect(TokenKind::Identifier, "identifier").lexeme;
    } else if (check(TokenKind::Identifier) && check(TokenKind::Colon, 1) && peek().line == peek(1).line) {
        field.name = advance().lexeme;
        advance();
        field.type_name = parse_type_annotation_string();
    } else if (check(TokenKind::Identifier)) {
        field.name = advance().lexeme;
    } else {
        fail("invalid struct field");
    }
    if (!seen.insert(field.name).second) {
        fail("duplicate field '" + field.name + "'", field.loc);
    }
    return field;
}

std::vector<StructField> Parser::parse_struct_body(int header_column) {
    std::vector<StructField> fields;
    std::unordered_set<std::string> seen;
    if (check(TokenKind::Colon)) {
        const Token colon = advance();
        if (is_at_end() || peek().line <= colon.line) {
            fail("missing indent");
        }
        const int field_indent = peek().column;
        while (!is_at_end()) {
            const int column = peek().column;
            if (column < field_indent) {
                break;
            }
            if (column > field_indent) {
                fail("unexpected indent");
            }
            fields.push_back(parse_struct_field(seen));
        }
        return fields;
    }
    if (check(TokenKind::LeftBrace)) {
        if (looks_like_dict()) {
            fail("invalid struct syntax");
        }
        const Token brace = advance();
        if (is_at_end()) {
            fail("missing struct fields");
        }
        const int field_indent = peek().column;
        while (!check(TokenKind::RightBrace)) {
            const int column = peek().column;
            if (column != field_indent) {
                if (column > field_indent) {
                    fail("unexpected indent");
                }
                fail("invalid indent");
            }
            fields.push_back(parse_struct_field(seen));
        }
        expect(TokenKind::RightBrace, "}");
        return fields;
    }
    fail("missing block");
}

std::unique_ptr<Stmt> Parser::parse_defer_statement() {
    const Token keyword = advance();
    auto stmt = std::make_unique<DeferStmt>();
    stmt->loc = location_from(keyword);
    stmt->expression = parse_expression();
    return stmt;
}

std::vector<std::string> Parser::parse_enum_member_payload() {
    std::vector<std::string> fields;
    if (!check(TokenKind::LeftParen)) {
        return fields;
    }
    advance();
    if (check(TokenKind::RightParen)) {
        fail("empty enum payload");
    }
    while (!check(TokenKind::RightParen)) {
        if (!check(TokenKind::Identifier)) {
            fail("invalid enum payload field");
        }
        fields.push_back(advance().lexeme);
        if (check(TokenKind::Comma)) {
            advance();
        } else if (!check(TokenKind::RightParen)) {
            fail("invalid enum payload");
        }
    }
    expect(TokenKind::RightParen, ")");
    return fields;
}

void Parser::parse_enum_body(int header_column, std::vector<std::string>& members,
                             std::vector<std::vector<std::string>>& member_payloads,
                             std::vector<std::vector<Annotation>>& member_annotations) {
    members.clear();
    member_payloads.clear();
    member_annotations.clear();
    std::unordered_set<std::string> seen;
    const auto push_member = [&](const Token& member, std::vector<Annotation>&& annotations) {
        if (!seen.insert(member.lexeme).second) {
            fail("duplicate enum member '" + member.lexeme + "'", member);
        }
        members.push_back(member.lexeme);
        member_payloads.push_back(parse_enum_member_payload());
        member_annotations.push_back(std::move(annotations));
    };
    if (check(TokenKind::Colon)) {
        const Token colon = advance();
        if (is_at_end() || peek().line <= colon.line) {
            fail("missing indent");
        }
        const int member_indent = peek().column;
        while (!is_at_end()) {
            const int column = peek().column;
            if (column < member_indent) {
                break;
            }
            if (column > member_indent) {
                fail("unexpected indent");
            }
            if (!check(TokenKind::Identifier) && !check(TokenKind::At)) {
                fail("invalid enum member");
            }
            std::vector<Annotation> annotations;
            if (check(TokenKind::At)) {
                annotations = parse_annotations();
            }
            if (!check(TokenKind::Identifier)) {
                fail("invalid enum member");
            }
            push_member(advance(), std::move(annotations));
        }
        return;
    }
    if (check(TokenKind::LeftBrace)) {
        if (looks_like_dict()) {
            fail("invalid enum syntax");
        }
        const Token brace = advance();
        if (is_at_end()) {
            fail("missing enum members");
        }
        const int member_indent = peek().column;
        while (!check(TokenKind::RightBrace)) {
            const int column = peek().column;
            if (column != member_indent) {
                if (column > member_indent) {
                    fail("unexpected indent");
                }
                fail("invalid indent");
            }
            if (!check(TokenKind::Identifier) && !check(TokenKind::At)) {
                fail("invalid enum member");
            }
            std::vector<Annotation> annotations;
            if (check(TokenKind::At)) {
                annotations = parse_annotations();
            }
            if (!check(TokenKind::Identifier)) {
                fail("invalid enum member");
            }
            push_member(advance(), std::move(annotations));
        }
        expect(TokenKind::RightBrace, "}");
        return;
    }
    fail("missing block");
}

std::unique_ptr<Stmt> Parser::parse_function_statement(MemberAccess access,
                                                       std::vector<Annotation> leading_annotations, int access_column) {
    std::vector<Annotation> annotations =
        leading_annotations.empty() ? parse_annotations() : std::move(leading_annotations);
    bool is_async = false;
    int block_column = access_column >= 0 ? access_column : peek().column;
    if (check(TokenKind::Async)) {
        is_async = true;
        block_column = std::min(block_column, peek().column);
        advance();
    }
    const Token func_tok = advance();
    block_column = std::min(block_column, func_tok.column);
    auto stmt = std::make_unique<FuncStmt>();
    stmt->annotations = std::move(annotations);
    stmt->loc = location_from(func_tok);
    stmt->access = access;
    stmt->is_async = is_async;
    if (!check(TokenKind::Identifier)) {
        fail("invalid function syntax");
    }
    stmt->name = advance().lexeme;
    stmt->type_params = parse_generic_type_params();
    expect(TokenKind::LeftParen, "(");
    stmt->parameters = parse_parameters();
    expect(TokenKind::RightParen, ")");
    if (check(TokenKind::MinusGreater)) {
        advance();
        stmt->return_type_name = parse_type_annotation_string();
    }
    ++function_depth_;
    if (is_async) {
        ++async_function_depth_;
    }
    stmt->body = parse_block(block_column);
    if (is_async) {
        --async_function_depth_;
    }
    --function_depth_;
    return stmt;
}

std::vector<Parameter> Parser::parse_parameters() {
    std::vector<Parameter> params;
    if (check(TokenKind::RightParen)) {
        return params;
    }
    if (check(TokenKind::Comma)) {
        fail("invalid function syntax");
    }
    std::unordered_set<std::string> seen;
    std::unordered_set<std::string> pattern_names;
    for (;;) {
        Parameter param;
        param.loc = location_from(peek());
        param.annotations = parse_annotations();
        if (check(TokenKind::LeftBrace) || check(TokenKind::LeftBracket) || check(TokenKind::LeftParen) ||
            (check(TokenKind::Identifier) && check(TokenKind::LeftBrace, 1) && peek().line == peek(1).line)) {
            param.pattern = parse_parameter_destructure_pattern();
            param.name = "__arg" + std::to_string(params.size());
        } else if (looks_like_type_prefix()) {
            param.type_name = parse_type_annotation_string();
            param.name = expect(TokenKind::Identifier, "identifier").lexeme;
        } else if (!check(TokenKind::Identifier)) {
            fail("invalid function syntax");
        } else {
            param.name = advance().lexeme;
            if (check(TokenKind::Colon)) {
                advance();
                param.type_name = parse_type_annotation_string();
            }
        }
        if (!seen.insert(param.name).second) {
            fail("duplicate parameter '" + param.name + "'", param.loc);
        }
        if (check(TokenKind::Ellipsis)) {
            param.is_variadic = true;
            advance();
        }
        if (check(TokenKind::Equal)) {
            advance();
            param.default_value = parse_expression();
        }
        if (param.pattern) {
            std::vector<std::string> names;
            collect_pattern_names(*param.pattern, names);
            for (const std::string& name : names) {
                if (!pattern_names.insert(name).second) {
                    fail("duplicate binding '" + name + "'", param.loc);
                }
            }
        }
        params.push_back(std::move(param));
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::Comma) || check(TokenKind::RightParen) || is_at_end()) {
            fail("invalid function syntax", comma);
        }
    }
    return params;
}

std::unique_ptr<Stmt> Parser::parse_return_statement() {
    const Token token = advance();
    if (function_depth_ == 0) {
        fail("return outside function", token);
    }
    auto stmt = std::make_unique<ReturnStmt>();
    stmt->loc = location_from(token);
    if (!is_at_end() && peek().line == token.line && can_start_expression()) {
        stmt->value = parse_expression();
    }
    return stmt;
}

bool Parser::try_handler_belongs(const Token& try_tok) const {
    if (peek().column == try_tok.column) {
        return true;
    }
    if (pos_ > 0) {
        const Token& previous = tokens_[pos_ - 1];
        if (previous.kind == TokenKind::RightBrace && peek().line == previous.line &&
            peek().column > try_tok.column) {
            return true;
        }
    }
    return false;
}

std::unique_ptr<Stmt> Parser::parse_try_statement() {
    const Token try_tok = advance();
    auto stmt = std::make_unique<TryStmt>();
    stmt->loc = location_from(try_tok);
    stmt->try_body = parse_block(try_tok.column);

    while (check(TokenKind::Catch) && try_handler_belongs(try_tok)) {
        const Token catch_tok = advance();
        CatchClause clause;
        clause.loc = location_from(catch_tok);
        if (!check(TokenKind::Identifier)) {
            fail("expected catch variable name", catch_tok);
        }
        const std::string first = advance().lexeme;
        if (check(TokenKind::Identifier)) {
            clause.type_name = first;
            clause.binding = advance().lexeme;
        } else {
            clause.binding = first;
        }
        clause.body = parse_block(catch_tok.column);
        stmt->catches.push_back(std::move(clause));
    }

    if (check(TokenKind::Finally) && try_handler_belongs(try_tok)) {
        const Token finally_tok = advance();
        stmt->finally_body = parse_block(finally_tok.column);
    }

    if (stmt->catches.empty() && !stmt->finally_body) {
        fail("try requires catch or finally", try_tok);
    }
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_throw_statement() {
    const Token token = advance();
    if (!can_start_expression()) {
        fail("expected expression after throw", token);
    }
    auto stmt = std::make_unique<ThrowStmt>();
    stmt->loc = location_from(token);
    stmt->value = parse_expression();
    return stmt;
}

std::string Parser::parse_module_path() {
    Token first = expect(TokenKind::Identifier, "module name");
    std::string path = first.lexeme;
    while (check(TokenKind::Dot)) {
        advance();
        Token part = expect(TokenKind::Identifier, "module name");
        path += '.';
        path += part.lexeme;
    }
    return path;
}

std::unique_ptr<Stmt> Parser::parse_import_statement() {
    auto stmt = std::make_unique<ImportStmt>();
    if (check(TokenKind::From)) {
        stmt->from_import = true;
        const Token from_tok = advance();
        stmt->loc = location_from(from_tok);
        stmt->module = parse_module_path();
        expect(TokenKind::Import, "import");
        if (check(TokenKind::Identifier)) {
            do {
                ImportBinding binding;
                binding.loc = location_from(peek());
                binding.name = expect(TokenKind::Identifier, "import name").lexeme;
                if (check(TokenKind::As)) {
                    advance();
                    binding.alias = expect(TokenKind::Identifier, "alias name").lexeme;
                }
                stmt->bindings.push_back(std::move(binding));
                if (!check(TokenKind::Comma)) {
                    break;
                }
                advance();
            } while (true);
        } else {
            fail("expected import name");
        }
        return stmt;
    }

    const Token import_tok = advance();
    stmt->loc = location_from(import_tok);
    stmt->module = parse_module_path();
    if (check(TokenKind::As)) {
        advance();
        stmt->module_alias = expect(TokenKind::Identifier, "alias name").lexeme;
    }
    return stmt;
}

std::unique_ptr<Stmt> Parser::parse_match_statement() {
    const Token match_tok = advance();
    auto stmt = std::make_unique<MatchStmt>();
    stmt->loc = location_from(match_tok);
    if (check(TokenKind::Colon) || (check(TokenKind::LeftBrace) && !looks_like_dict()) || is_at_end()) {
        fail("missing expression");
    }
    stmt->value = parse_expression();
    if (check(TokenKind::Colon)) {
        const Token colon = advance();
        if (check(TokenKind::LeftBrace)) {
            fail("cannot mix ':' and '{'");
        }
        stmt->cases = parse_match_cases_indented(match_tok.column, colon.line, false);
    } else if (check(TokenKind::LeftBrace)) {
        const Token brace = advance();
        stmt->cases = parse_match_cases_brace(match_tok.column, brace.line, false);
    } else {
        fail("missing block");
    }
    if (stmt->cases.empty()) {
        fail("empty match");
    }
    return stmt;
}

std::unique_ptr<Expr> Parser::parse_match_expression() {
    const Token match_tok = advance();
    auto expr = std::make_unique<MatchExpr>();
    expr->loc = location_from(match_tok);
    if (check(TokenKind::Colon) || (check(TokenKind::LeftBrace) && !looks_like_dict()) || is_at_end()) {
        fail("missing expression");
    }
    expr->value = parse_expression();
    if (check(TokenKind::Colon)) {
        const Token colon = advance();
        if (check(TokenKind::LeftBrace)) {
            fail("cannot mix ':' and '{'");
        }
        expr->cases = parse_match_cases_indented(match_tok.column, colon.line, true);
    } else if (check(TokenKind::LeftBrace)) {
        const Token brace = advance();
        expr->cases = parse_match_cases_brace(match_tok.column, brace.line, true);
    } else {
        fail("missing block");
    }
    if (expr->cases.empty()) {
        fail("empty match");
    }
    return expr;
}

std::vector<MatchCase> Parser::parse_match_cases_indented(int header_column, int opener_line, bool expression_form) {
    if (is_at_end() || peek().line <= opener_line || peek().column <= header_column) {
        fail("missing indent");
    }
    const int case_indent = peek().column;
    std::vector<MatchCase> cases;
    while (!is_at_end()) {
        const int column = peek().column;
        if (column < case_indent) {
            if (column <= header_column) {
                break;
            }
            fail("invalid indent");
        }
        if (column > case_indent) {
            fail("unexpected indent");
        }
        cases.push_back(parse_match_case(case_indent, expression_form));
    }
    if (cases.empty()) {
        fail("missing indent");
    }
    return cases;
}

std::vector<MatchCase> Parser::parse_match_cases_brace(int header_column, int opener_line, bool expression_form) {
    if (check(TokenKind::RightBrace)) {
        advance();
        return {};
    }
    if (is_at_end() || peek().line <= opener_line || peek().column <= header_column) {
        fail("missing indent");
    }
    const int case_indent = peek().column;
    std::vector<MatchCase> cases;
    while (!is_at_end() && !check(TokenKind::RightBrace)) {
        const int column = peek().column;
        if (column != case_indent) {
            if (column > case_indent) {
                fail("unexpected indent");
            }
            fail("invalid indent");
        }
        cases.push_back(parse_match_case(case_indent, expression_form));
    }
    expect(TokenKind::RightBrace, "}");
    return cases;
}

MatchCase Parser::parse_match_case(int case_column, bool expression_form) {
    if (check(TokenKind::Else)) {
        fail("match uses '_' as the fallback, not else");
    }
    MatchCase branch;
    branch.loc = location_from(peek());
    branch.pattern = parse_match_pattern_or();
    if (check(TokenKind::If)) {
        advance();
        if (check(TokenKind::Colon) || check(TokenKind::LeftBrace) || is_at_end()) {
            fail("missing guard");
        }
        branch.guard = parse_expression();
    }
    if (expression_form) {
        if (check(TokenKind::Colon)) {
            const Token colon = advance();
            if (!is_at_end() && peek().line > colon.line && peek().column > case_column) {
                const std::vector<std::unique_ptr<Stmt>> block = parse_block(case_column);
                if (block.size() != 1 || !block[0]) {
                    fail("match expression case must be a single expression", branch.loc);
                }
                auto* expr_stmt = dynamic_cast<ExprStmt*>(block[0].get());
                if (!expr_stmt || !expr_stmt->expression) {
                    fail("match expression case must be a single expression", branch.loc);
                }
                branch.result_expr = std::move(expr_stmt->expression);
            } else {
                branch.result_expr = parse_expression();
            }
        } else if (check(TokenKind::LeftBrace)) {
            advance();
            branch.result_expr = parse_expression();
            expect(TokenKind::RightBrace, "}");
        } else {
            fail("missing block");
        }
        if (!branch.result_expr) {
            fail("match expression case must be a single expression", branch.loc);
        }
        return branch;
    }
    if (check(TokenKind::Colon) || check(TokenKind::LeftBrace)) {
        branch.body = parse_block(case_column);
    } else {
        fail("missing block");
    }
    if (branch.body.empty()) {
        fail("empty match case");
    }
    return branch;
}

std::unique_ptr<MatchPattern> Parser::parse_match_pattern_or() {
    auto left = parse_match_pattern_and();
    while (check(TokenKind::Or) || check(TokenKind::Pipe)) {
        advance();
        auto right = parse_match_pattern_and();
        if (!right) {
            fail("invalid pattern");
        }
        if (left->kind == MatchPatternKind::Or) {
            left->parts.push_back(std::move(right));
        } else {
            auto combined = std::make_unique<MatchPattern>();
            combined->kind = MatchPatternKind::Or;
            combined->loc = left->loc;
            combined->parts.push_back(std::move(left));
            combined->parts.push_back(std::move(right));
            left = std::move(combined);
        }
    }
    return left;
}

std::unique_ptr<MatchPattern> Parser::parse_match_pattern_and() {
    auto left = parse_match_pattern_primary();
    while (check(TokenKind::And) || check(TokenKind::Amp)) {
        advance();
        auto right = parse_match_pattern_primary();
        if (!right) {
            fail("invalid pattern");
        }
        if (left->kind == MatchPatternKind::And) {
            left->parts.push_back(std::move(right));
        } else {
            auto combined = std::make_unique<MatchPattern>();
            combined->kind = MatchPatternKind::And;
            combined->loc = left->loc;
            combined->parts.push_back(std::move(left));
            combined->parts.push_back(std::move(right));
            left = std::move(combined);
        }
    }
    return left;
}

std::unique_ptr<MatchPattern> Parser::parse_match_pattern_primary() {
    if (check(TokenKind::Identifier) && peek().lexeme == "_") {
        const Token wildcard = advance();
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Wildcard;
        pattern->loc = location_from(wildcard);
        return pattern;
    }
    if (check(TokenKind::Identifier) && check(TokenKind::Dot, 1) && check(TokenKind::Identifier, 2) &&
        check(TokenKind::LeftParen, 3)) {
        auto pattern = std::make_unique<MatchPattern>();
        pattern->loc = location_from(peek());
        pattern->kind = MatchPatternKind::EnumVariant;
        pattern->enum_type = advance().lexeme;
        advance();
        pattern->enum_variant = advance().lexeme;
        advance();
        while (!check(TokenKind::RightParen)) {
            if (!check(TokenKind::Identifier)) {
                fail("invalid enum match pattern");
            }
            pattern->enum_bindings.push_back(advance().lexeme);
            if (check(TokenKind::Comma)) {
                advance();
            } else if (!check(TokenKind::RightParen)) {
                fail("invalid enum match pattern");
            }
        }
        expect(TokenKind::RightParen, ")");
        return pattern;
    }
    if (check(TokenKind::LeftBracket)) {
        auto destruct = parse_destructure_list_pattern();
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Destruct;
        pattern->loc = destruct->loc;
        pattern->destruct = std::move(destruct);
        return pattern;
    }
    if (check(TokenKind::LeftParen)) {
        auto destruct = parse_destructure_tuple_pattern();
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Destruct;
        pattern->loc = destruct->loc;
        pattern->destruct = std::move(destruct);
        return pattern;
    }
    if (check(TokenKind::LeftBrace) && looks_like_dict()) {
        auto destruct = parse_destructure_dict_pattern();
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Destruct;
        pattern->loc = destruct->loc;
        pattern->destruct = std::move(destruct);
        return pattern;
    }
    if (check(TokenKind::Identifier) && pattern_type_followed_by(1, TokenKind::LeftParen)) {
        const Location type_loc = location_from(peek());
        std::string type_name = parse_type_name_for_pattern();
        auto destruct = parse_destructure_positional_pattern(type_name, type_loc);
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Destruct;
        pattern->loc = destruct->loc;
        pattern->destruct = std::move(destruct);
        return pattern;
    }
    if (check(TokenKind::Identifier) && pattern_type_followed_by(1, TokenKind::LeftBrace)) {
        const Location type_loc = location_from(peek());
        std::string type_name = parse_type_name_for_pattern();
        auto destruct = parse_destructure_struct_pattern(type_name, type_loc);
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Destruct;
        pattern->loc = destruct->loc;
        pattern->destruct = std::move(destruct);
        return pattern;
    }
    if (check(TokenKind::Identifier)) {
        const int line = peek().line;
        const bool generic_type = check(TokenKind::Less, 1) && peek(1).line == line;
        const bool simple_type_bind = check(TokenKind::Identifier, 1) && peek(1).line == line &&
                                      !check(TokenKind::LeftParen, 2) && !check(TokenKind::LeftBrace, 2) &&
                                      !check(TokenKind::Dot, 1) && !generic_type;
        if (generic_type || simple_type_bind) {
            std::string type_spec;
            if (generic_type) {
                type_spec = parse_type_annotation_primary();
            } else {
                type_spec = advance().lexeme;
            }
            if (check(TokenKind::Identifier) && peek().line == line) {
                auto pattern = std::make_unique<MatchPattern>();
                pattern->kind = MatchPatternKind::TypeBind;
                pattern->loc = location_from(peek());
                pattern->type_name = std::move(type_spec);
                pattern->binding = advance().lexeme;
                return pattern;
            }
        }
    }
    if (check(TokenKind::Identifier) && check(TokenKind::Dot, 1)) {
        std::vector<std::unique_ptr<Expr>> literals;
        for (;;) {
            literals.push_back(parse_match_literal());
            if (!check(TokenKind::Comma)) {
                break;
            }
            const Token comma = advance();
            if (check(TokenKind::Colon) || check(TokenKind::LeftBrace) || check(TokenKind::Comma) || is_at_end()) {
                fail("invalid pattern", comma);
            }
        }
        if (literals.size() == 1) {
            auto pattern = std::make_unique<MatchPattern>();
            pattern->kind = MatchPatternKind::Literal;
            pattern->loc = literals[0]->loc;
            pattern->literal = std::move(literals[0]);
            return pattern;
        }
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Literals;
        pattern->loc = literals[0]->loc;
        pattern->literals = std::move(literals);
        return pattern;
    }
    if (check(TokenKind::Identifier)) {
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Binding;
        pattern->loc = location_from(peek());
        pattern->binding = advance().lexeme;
        return pattern;
    }
    std::vector<std::unique_ptr<Expr>> literals;
    for (;;) {
        literals.push_back(parse_match_literal());
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::Colon) || check(TokenKind::LeftBrace) || check(TokenKind::Comma) || is_at_end()) {
            fail("invalid pattern", comma);
        }
    }
    if (literals.size() == 1) {
        auto pattern = std::make_unique<MatchPattern>();
        pattern->kind = MatchPatternKind::Literal;
        pattern->loc = literals[0]->loc;
        pattern->literal = std::move(literals[0]);
        return pattern;
    }
    auto pattern = std::make_unique<MatchPattern>();
    pattern->kind = MatchPatternKind::Literals;
    pattern->loc = literals[0]->loc;
    pattern->literals = std::move(literals);
    return pattern;
}

std::unique_ptr<DestructPattern> Parser::parse_destructure_positional_pattern(const std::string& type_name,
                                                                              const Location& loc) {
    expect(TokenKind::LeftParen, "(");
    auto pattern = std::make_unique<DestructPattern>();
    pattern->kind = DestructPatternKind::Positional;
    pattern->loc = loc;
    pattern->struct_type_name = type_name;
    if (check(TokenKind::RightParen)) {
        fail("invalid pattern", loc);
    }
    for (;;) {
        pattern->elements.push_back(parse_destructure_pattern_element());
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::RightParen)) {
            fail("invalid pattern", comma);
        }
        if (pattern->elements.back() && pattern->elements.back()->kind == DestructPatternKind::Rest) {
            fail("rest pattern must be last in destructuring assignment", comma);
        }
    }
    expect(TokenKind::RightParen, ")");
    validate_destructure_pattern(*pattern);
    return pattern;
}

std::unique_ptr<Expr> Parser::parse_match_literal() {
    if (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        const Token op = advance();
        if (!check(TokenKind::Integer) && !check(TokenKind::Float)) {
            fail("invalid pattern");
        }
        auto expr = std::make_unique<UnaryExpr>();
        expr->loc = location_from(op);
        expr->op = op.kind;
        expr->operand = parse_primary();
        return expr;
    }
    if (check(TokenKind::Integer) || check(TokenKind::Float) || check(TokenKind::String) ||
        check(TokenKind::True) || check(TokenKind::False) || check(TokenKind::Null)) {
        return parse_primary();
    }
    if (check(TokenKind::Identifier) && check(TokenKind::Dot, 1) && check(TokenKind::Identifier, 2) &&
        !check(TokenKind::LeftParen, 3)) {
        auto object = std::make_unique<IdentifierExpr>();
        object->loc = location_from(peek());
        object->name = advance().lexeme;
        advance();
        auto access = std::make_unique<FieldAccessExpr>();
        access->loc = object->loc;
        access->object = std::move(object);
        access->field = advance().lexeme;
        return access;
    }
    fail("invalid pattern");
}

std::unique_ptr<Expr> Parser::parse_if_condition() {
    if (check(TokenKind::Colon) || is_elif() || check(TokenKind::Else) || check(TokenKind::RightBrace) ||
        is_at_end()) {
        fail("missing condition");
    }
    if (check(TokenKind::LeftBrace) && !looks_like_dict()) {
        fail("missing condition");
    }
    return parse_expression();
}

bool Parser::looks_like_dict() const {
    if (!check(TokenKind::LeftBrace)) {
        return false;
    }
    if (check(TokenKind::RightBrace, 1)) {
        return true;
    }
    const Token& next = peek(1);
    if (next.kind == TokenKind::Eof || next.line > peek().line) {
        return false;
    }
    return can_start_expression_kind(next.kind);
}

std::vector<std::unique_ptr<Stmt>> Parser::parse_block(int header_column) {
    if (check(TokenKind::Colon)) {
        const Token colon = advance();
        if (check(TokenKind::LeftBrace)) {
            fail("cannot mix ':' and '{'");
        }
        return parse_indented_suite(header_column, colon.line);
    }
    if (check(TokenKind::LeftBrace)) {
        const Token brace = advance();
        return parse_brace_block(header_column, brace.line);
    }
    fail("missing block");
}

std::vector<std::unique_ptr<Stmt>> Parser::parse_indented_suite(int header_column, int opener_line) {
    if (is_at_end() || peek().line <= opener_line || peek().column <= header_column) {
        fail("missing indent");
    }

    const int body_indent = peek().column;
    std::vector<std::unique_ptr<Stmt>> statements;
    while (!is_at_end()) {
        const int column = peek().column;
        if (column < body_indent) {
            if (column <= header_column) {
                break;
            }
            fail("invalid indent");
        }
        if (column > body_indent) {
            fail("unexpected indent");
        }
        statements.push_back(parse_statement());
    }
    if (statements.empty()) {
        fail("missing indent");
    }
    return statements;
}

std::vector<std::unique_ptr<Stmt>> Parser::parse_brace_block(int header_column, int opener_line) {
    if (check(TokenKind::RightBrace)) {
        advance();
        return {};
    }
    if (is_at_end() || peek().line <= opener_line || peek().column <= header_column) {
        fail("missing indent");
    }

    const int body_indent = peek().column;
    std::vector<std::unique_ptr<Stmt>> statements;
    while (!is_at_end() && !check(TokenKind::RightBrace)) {
        const int column = peek().column;
        if (column != body_indent) {
            if (column > body_indent) {
                fail("unexpected indent");
            }
            fail("invalid indent");
        }
        statements.push_back(parse_statement());
    }
    expect(TokenKind::RightBrace, "}");
    return statements;
}

bool Parser::is_elif() const {
    return check(TokenKind::Elif) || check(TokenKind::ElseIf);
}

bool Parser::can_continue_branch(int header_column) const {
    if (!is_elif() && !check(TokenKind::Else)) {
        return false;
    }
    if (peek().column == header_column) {
        return true;
    }
    if (pos_ == 0) {
        return false;
    }
    const Token& prev = tokens_[pos_ - 1];
    return prev.kind == TokenKind::RightBrace && prev.line == peek().line;
}

std::unique_ptr<Stmt> Parser::parse_print_statement() {
    const Token print_tok = advance();
    auto stmt = std::make_unique<PrintStmt>();
    stmt->loc = location_from(print_tok);

    if (is_immediate_call_paren(print_tok)) {
        stmt->parenthesized = true;
        advance();
        stmt->expressions = parse_print_arguments(true);
        expect(TokenKind::RightParen, ")");
        return stmt;
    }

    stmt->expressions = parse_print_arguments(false);
    return stmt;
}

bool Parser::is_immediate_call_paren(const Token& callee) const {
    if (!check(TokenKind::LeftParen)) {
        return false;
    }
    const Token& paren = peek();
    return paren.line == callee.line &&
           paren.column == callee.column + static_cast<int>(callee.lexeme.size());
}

std::vector<std::unique_ptr<Expr>> Parser::parse_print_arguments(bool parenthesized) {
    if (parenthesized && check(TokenKind::RightParen)) {
        fail("invalid print arguments");
    }
    if (check(TokenKind::Comma) || is_at_end()) {
        fail("invalid print arguments");
    }

    std::vector<std::unique_ptr<Expr>> args;
    args.push_back(parse_expression());
    while (check(TokenKind::Comma)) {
        const Token comma = advance();
        if (check(TokenKind::Comma) || is_at_end() || (parenthesized && check(TokenKind::RightParen))) {
            fail("invalid print arguments", comma);
        }
        args.push_back(parse_expression());
    }
    return args;
}

bool Parser::looks_like_generic_instantiation_call() const {
    if (!check(TokenKind::Less)) {
        return false;
    }
    bool saw_greater = false;
    for (std::size_t ahead = 1; ahead < 16; ++ahead) {
        const TokenKind kind = peek(ahead).kind;
        if (kind == TokenKind::Eof) {
            break;
        }
        if (kind == TokenKind::LeftParen) {
            return saw_greater;
        }
        if (kind == TokenKind::Greater || kind == TokenKind::GreaterGreater) {
            saw_greater = true;
        }
        if (kind == TokenKind::Colon && peek(ahead).line == peek().line) {
            break;
        }
    }
    return false;
}

bool Parser::looks_like_type_prefix() const {
    if (!check(TokenKind::Identifier)) {
        return false;
    }
    if (check(TokenKind::Identifier, 1) && peek().line == peek(1).line) {
        return true;
    }
    if (check(TokenKind::Less, 1)) {
        return true;
    }
    return false;
}

bool Parser::pattern_type_followed_by(std::size_t name_ahead, TokenKind open) const {
    if (!check(TokenKind::Identifier, name_ahead)) {
        return false;
    }
    const int line = peek(name_ahead).line;
    std::size_t i = name_ahead + 1;
    if (check(TokenKind::Less, i) && peek(i).line == line) {
        ++i;
        int depth = 1;
        while (depth > 0) {
            if (pos_ + i >= tokens_.size()) {
                return false;
            }
            const TokenKind kind = peek(i).kind;
            if (kind == TokenKind::Less) {
                ++depth;
            } else if (kind == TokenKind::Greater || kind == TokenKind::GreaterGreater) {
                --depth;
            }
            ++i;
        }
    }
    return check(open, i) && peek(i).line == line;
}

std::string Parser::parse_type_name_for_pattern() {
    if (check(TokenKind::Less, 1) && peek(1).line == peek().line) {
        return parse_type_annotation_primary();
    }
    return advance().lexeme;
}

void Parser::consume_generic_close() {
    if (check(TokenKind::Greater)) {
        advance();
        return;
    }
    if (check(TokenKind::GreaterGreater)) {
        const Token shifted = advance();
        Token greater;
        greater.kind = TokenKind::Greater;
        greater.line = shifted.line;
        greater.column = shifted.column + 1;
        greater.lexeme = ">";
        tokens_.insert(tokens_.begin() + static_cast<std::ptrdiff_t>(pos_), std::move(greater));
        return;
    }
    fail(">");
}

std::string Parser::parse_type_annotation_primary() {
    std::string base = advance().lexeme;
    if (base == "List") {
        base = "list";
    } else if (base == "Dict" || base == "dict") {
        base = "map";
    } else if (base == "Task") {
        base = "task";
    } else if (base == "Tuple") {
        base = "tuple";
    } else if (base == "Set") {
        base = "set";
    }
    if (!check(TokenKind::Less)) {
        return base;
    }
    advance();
    if (base == "list" || base == "set" || base == "task") {
        const std::string inner = parse_type_annotation_string();
        consume_generic_close();
        return base + "<" + inner + ">";
    }
    if (base == "tuple") {
        std::string inner;
        if (!check(TokenKind::Greater) && !check(TokenKind::GreaterGreater)) {
            inner = parse_type_annotation_string();
            while (check(TokenKind::Comma)) {
                advance();
                inner += "," + parse_type_annotation_string();
            }
        }
        consume_generic_close();
        return inner.empty() ? "tuple<>" : "tuple<" + inner + ">";
    }
    if (base == "map") {
        const std::string key = parse_type_annotation_string();
        expect(TokenKind::Comma, ",");
        const std::string value = parse_type_annotation_string();
        consume_generic_close();
        return "map<" + key + "," + value + ">";
    }
    {
        std::string inner = parse_type_annotation_string();
        while (check(TokenKind::Comma)) {
            advance();
            inner += "," + parse_type_annotation_string();
        }
        consume_generic_close();
        return base + "<" + inner + ">";
    }
}

std::string Parser::parse_type_annotation_string() {
    std::string result = parse_type_annotation_primary();
    while (check(TokenKind::Pipe)) {
        advance();
        result += "|" + parse_type_annotation_primary();
    }
    return result;
}

std::vector<Annotation> Parser::parse_annotations() {
    std::vector<Annotation> annotations;
    while (check(TokenKind::At)) {
        const Token at = advance();
        Annotation annotation;
        annotation.loc = location_from(at);
        annotation.name = expect(TokenKind::Identifier, "annotation name").lexeme;
        if (check(TokenKind::LeftParen)) {
            advance();
            if (!check(TokenKind::RightParen)) {
                for (;;) {
                    AnnotationArg arg;
                    if (check(TokenKind::Identifier) && check(TokenKind::Equal, 1)) {
                        arg.key = advance().lexeme;
                        advance();
                        arg.value = parse_expression();
                    } else {
                        arg.value = parse_expression();
                    }
                    annotation.args.emplace_back(std::move(arg));
                    if (!check(TokenKind::Comma)) {
                        break;
                    }
                    advance();
                }
            }
            expect(TokenKind::RightParen, ")");
        }
        annotations.push_back(std::move(annotation));
    }
    return annotations;
}

std::vector<GenericTypeParam> Parser::parse_generic_type_params() {
    std::vector<GenericTypeParam> params;
    if (!check(TokenKind::Less)) {
        return params;
    }
    advance();
    if (check(TokenKind::Greater) || check(TokenKind::GreaterGreater)) {
        consume_generic_close();
        return params;
    }
    for (;;) {
        GenericTypeParam param;
        param.loc = location_from(peek());
        param.name = expect(TokenKind::Identifier, "type parameter").lexeme;
        if (check(TokenKind::Colon)) {
            advance();
            param.constraint_type_names.push_back(parse_type_annotation_primary());
            while (check(TokenKind::Amp)) {
                advance();
                param.constraint_type_names.push_back(parse_type_annotation_primary());
            }
            if (param.constraint_type_names.size() == 1) {
                param.constraint_type_name = param.constraint_type_names.front();
            } else if (!param.constraint_type_names.empty()) {
                std::string combined;
                for (std::size_t i = 0; i < param.constraint_type_names.size(); ++i) {
                    if (i != 0) {
                        combined += " & ";
                    }
                    combined += param.constraint_type_names[i];
                }
                param.constraint_type_name = combined;
            }
        }
        params.push_back(std::move(param));
        if (!check(TokenKind::Comma)) {
            break;
        }
        advance();
    }
    consume_generic_close();
    return params;
}

std::unique_ptr<Stmt> Parser::parse_assign_statement() {
    const Token first = peek();
    auto stmt = std::make_unique<AssignStmt>();
    stmt->loc = location_from(first);

    if (looks_like_type_prefix()) {
        stmt->annotation = TypeAnnotationKind::Prefix;
        stmt->type_name = parse_type_annotation_string();
        stmt->name = expect(TokenKind::Identifier, "identifier").lexeme;
    } else {
        stmt->name = advance().lexeme;
        if (check(TokenKind::Colon)) {
            stmt->annotation = TypeAnnotationKind::Colon;
            advance();
            if (!check(TokenKind::Identifier)) {
                fail("missing expected token 'type name'");
            }
            stmt->type_name = parse_type_annotation_string();
        }
    }

    if (check(TokenKind::Equal)) {
        advance();
        if (is_at_end() || check(TokenKind::Equal)) {
            fail("invalid assignment");
        }
        stmt->value = parse_expression();
        return stmt;
    }

    if (stmt->annotation == TypeAnnotationKind::None) {
        fail("invalid statement");
    }
    return stmt;
}

std::unique_ptr<Expr> Parser::parse_expression() {
    return parse_assignment();
}

std::unique_ptr<Expr> Parser::parse_assignment() {
    auto expr = parse_pipeline();
    if (!is_assign_op(peek().kind)) {
        return expr;
    }
    if (!is_lvalue(*expr)) {
        fail("invalid assignment");
    }
    const Token op = advance();
    if (is_at_end() || check(TokenKind::Equal) || is_assign_op(peek().kind)) {
        fail("invalid assignment");
    }
    auto assign = std::make_unique<AssignExpr>();
    assign->loc = expr->loc;
    assign->target = std::move(expr);
    assign->op = op.kind;
    assign->value = parse_assignment();
    return assign;
}

std::unique_ptr<Expr> Parser::parse_pipeline() {
    auto expr = parse_ternary();
    while (check(TokenKind::PipeGreater)) {
        advance();
        std::unique_ptr<Expr> right = parse_ternary();
        if (auto* call = dynamic_cast<CallExpr*>(right.get())) {
            std::vector<std::unique_ptr<Expr>> args;
            args.push_back(std::move(expr));
            for (std::unique_ptr<Expr>& arg : call->arguments) {
                args.push_back(std::move(arg));
            }
            call->arguments = std::move(args);
            expr = std::move(right);
            continue;
        }
        if (auto* method = dynamic_cast<MethodCallExpr*>(right.get())) {
            std::vector<std::unique_ptr<Expr>> args;
            args.push_back(std::move(expr));
            for (std::unique_ptr<Expr>& arg : method->arguments) {
                args.push_back(std::move(arg));
            }
            method->arguments = std::move(args);
            expr = std::move(right);
            continue;
        }
        fail("pipeline operator requires a function call");
    }
    return expr;
}

bool Parser::looks_like_ternary_question() const {
    if (!check(TokenKind::Question)) {
        return false;
    }
    std::size_t i = pos_ + 1;
    if (i >= tokens_.size()) {
        return false;
    }
    const int question_line = tokens_[pos_].line;
    int depth_paren = 0;
    int depth_bracket = 0;
    int depth_brace = 0;
    bool same_line_after_question = false;
    for (; i < tokens_.size(); ++i) {
        const TokenKind kind = tokens_[i].kind;
        if (kind == TokenKind::Eof) {
            break;
        }
        if (tokens_[i].line > question_line && depth_paren == 0 && depth_bracket == 0 && depth_brace == 0) {
            break;
        }
        if (tokens_[i].line == question_line) {
            same_line_after_question = true;
        }
        if (depth_paren == 0 && depth_bracket == 0 && depth_brace == 0) {
            if (kind == TokenKind::Colon) {
                return true;
            }
            if (kind == TokenKind::Comma || kind == TokenKind::RightParen || kind == TokenKind::RightBracket ||
                kind == TokenKind::RightBrace) {
                break;
            }
        }
        if (kind == TokenKind::LeftParen) {
            ++depth_paren;
        } else if (kind == TokenKind::RightParen && depth_paren > 0) {
            --depth_paren;
        } else if (kind == TokenKind::LeftBracket) {
            ++depth_bracket;
        } else if (kind == TokenKind::RightBracket && depth_bracket > 0) {
            --depth_bracket;
        } else if (kind == TokenKind::LeftBrace) {
            ++depth_brace;
        } else if (kind == TokenKind::RightBrace && depth_brace > 0) {
            --depth_brace;
        }
    }
    return same_line_after_question;
}

std::unique_ptr<Expr> Parser::parse_ternary() {
    auto expr = parse_null_coalesce();
    if (!check(TokenKind::Question)) {
        return expr;
    }
    advance();
    if (check(TokenKind::Colon) || is_at_end()) {
        fail("invalid ternary");
    }
    auto ternary = std::make_unique<TernaryExpr>();
    ternary->loc = expr->loc;
    ternary->condition = std::move(expr);
    ternary->then_expr = parse_assignment();
    expect(TokenKind::Colon, ":");
    if (is_at_end() || check(TokenKind::Colon)) {
        fail("invalid ternary");
    }
    ternary->else_expr = parse_ternary();
    return ternary;
}

std::unique_ptr<Expr> Parser::parse_null_coalesce() {
    auto expr = parse_or();
    if (!check(TokenKind::NullCoalesce)) {
        return expr;
    }
    advance();
    auto right = parse_null_coalesce();
    auto binary = std::make_unique<BinaryExpr>();
    binary->loc = expr->loc;
    binary->left = std::move(expr);
    binary->op = TokenKind::NullCoalesce;
    binary->right = std::move(right);
    return binary;
}

std::unique_ptr<Expr> Parser::parse_or() {
    auto expr = parse_and();
    while (is_or(peek().kind)) {
        const Token op = advance();
        auto right = parse_and();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_and() {
    auto expr = parse_membership();
    while (is_and(peek().kind)) {
        const Token op = advance();
        auto right = parse_membership();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_membership() {
    auto expr = parse_comparison();
    for (;;) {
        bool negated = false;
        if (check(TokenKind::Not) && check(TokenKind::In, 1)) {
            advance();
            negated = true;
        } else if (!check(TokenKind::In)) {
            break;
        }
        advance();
        if (check(TokenKind::Colon) || check(TokenKind::Question) || is_at_end()) {
            fail("invalid expression");
        }
        auto membership = std::make_unique<MembershipExpr>();
        membership->loc = expr->loc;
        membership->value = std::move(expr);
        membership->negated = negated;
        membership->container = parse_comparison();
        expr = std::move(membership);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_comparison() {
    auto expr = parse_bitwise_or();
    while (is_comparison(peek().kind)) {
        const Token op = advance();
        auto right = parse_bitwise_or();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_bitwise_or() {
    auto expr = parse_bitwise_xor();
    while (check(TokenKind::Pipe)) {
        const Token op = advance();
        auto right = parse_bitwise_xor();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_bitwise_xor() {
    auto expr = parse_bitwise_and();
    while (check(TokenKind::Caret)) {
        const Token op = advance();
        auto right = parse_bitwise_and();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_bitwise_and() {
    auto expr = parse_shift();
    while (check(TokenKind::Amp)) {
        const Token op = advance();
        auto right = parse_shift();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_shift() {
    auto expr = parse_additive();
    while (check(TokenKind::LessLess) || check(TokenKind::GreaterGreater)) {
        const Token op = advance();
        auto right = parse_additive();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_additive() {
    auto expr = parse_multiplicative();
    while (is_additive(peek().kind)) {
        const Token op = advance();
        auto right = parse_multiplicative();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_multiplicative() {
    auto expr = parse_unary();
    while (is_multiplicative(peek().kind)) {
        const Token op = advance();
        auto right = parse_unary();
        auto binary = std::make_unique<BinaryExpr>();
        binary->loc = expr->loc;
        binary->left = std::move(expr);
        binary->op = op.kind;
        binary->right = std::move(right);
        expr = std::move(binary);
    }
    return expr;
}

std::unique_ptr<Expr> Parser::parse_unary() {
    if (check(TokenKind::Await)) {
        if (async_function_depth_ == 0 && function_depth_ > 0) {
            fail("await outside async function");
        }
        const Token tok = advance();
        auto expr = std::make_unique<AwaitExpr>();
        expr->loc = location_from(tok);
        expr->operand = parse_unary();
        return expr;
    }
    if (check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) {
        const Token op = advance();
        auto target = parse_unary();
        if (!is_lvalue(*target)) {
            fail("invalid increment", op);
        }
        auto expr = std::make_unique<UpdateExpr>();
        expr->loc = location_from(op);
        expr->op = op.kind;
        expr->prefix = true;
        expr->target = std::move(target);
        return expr;
    }
    if (check(TokenKind::Plus) || check(TokenKind::Minus) || check(TokenKind::Bang) || check(TokenKind::Not) ||
        check(TokenKind::Tilde)) {
        const Token op = advance();
        auto expr = std::make_unique<UnaryExpr>();
        expr->loc = location_from(op);
        expr->op = op.kind;
        expr->operand = parse_unary();
        return expr;
    }
    return parse_postfix();
}

std::unique_ptr<Expr> Parser::parse_postfix() {
    auto expr = parse_primary();
    for (;;) {
        if (check(TokenKind::LeftBracket)) {
            const Token bracket = advance();
            auto index = std::make_unique<IndexExpr>();
            index->loc = expr->loc;
            index->object = std::move(expr);
            if (check(TokenKind::RightBracket) || is_at_end()) {
                fail("invalid expression", bracket);
            }
            index->index = parse_expression();
            expect(TokenKind::RightBracket, "]");
            expr = std::move(index);
            continue;
        }
        if (check(TokenKind::Dot)) {
            advance();
            std::string field_name;
            if (check(TokenKind::Identifier)) {
                field_name = advance().lexeme;
            } else if (check(TokenKind::True)) {
                advance();
                field_name = "true";
            } else if (check(TokenKind::False)) {
                advance();
                field_name = "false";
            } else if (check(TokenKind::Null)) {
                advance();
                field_name = "null";
            } else {
                fail("missing expected token 'field name'");
            }
            if (check(TokenKind::LeftParen)) {
                advance();
                auto call = std::make_unique<MethodCallExpr>();
                call->loc = expr->loc;
                call->object = std::move(expr);
                call->method = field_name;
                call->arguments = parse_call_arguments();
                expect(TokenKind::RightParen, ")");
                expr = std::move(call);
                continue;
            }
            auto access = std::make_unique<FieldAccessExpr>();
            access->loc = expr->loc;
            access->object = std::move(expr);
            access->field = field_name;
            expr = std::move(access);
            continue;
        }
        if (auto* ident = dynamic_cast<IdentifierExpr*>(expr.get())) {
            if (looks_like_generic_instantiation_call()) {
                std::vector<std::string> type_arguments;
                advance();
                if (!check(TokenKind::Greater) && !check(TokenKind::GreaterGreater)) {
                    type_arguments.push_back(parse_type_annotation_string());
                    while (check(TokenKind::Comma)) {
                        advance();
                        type_arguments.push_back(parse_type_annotation_string());
                    }
                }
                consume_generic_close();
                if (!check(TokenKind::LeftParen)) {
                    break;
                }
                advance();
                auto call = std::make_unique<CallExpr>();
                call->loc = expr->loc;
                call->name = ident->name;
                call->type_arguments = std::move(type_arguments);
                call->arguments = parse_call_arguments();
                expect(TokenKind::RightParen, ")");
                expr = std::move(call);
                continue;
            } else if (check(TokenKind::LeftParen)) {
                advance();
                auto call = std::make_unique<CallExpr>();
                call->loc = expr->loc;
                call->name = ident->name;
                call->arguments = parse_call_arguments();
                expect(TokenKind::RightParen, ")");
                expr = std::move(call);
                continue;
            } else if (is_whitespace_call_callee(ident->name) && can_start_whitespace_call_argument()) {
                auto call = std::make_unique<CallExpr>();
                call->loc = expr->loc;
                call->name = ident->name;
                call->arguments.push_back(parse_unary());
                while (check(TokenKind::Comma)) {
                    advance();
                    call->arguments.push_back(parse_unary());
                }
                expr = std::move(call);
                continue;
            }
        }
        if (check(TokenKind::PlusPlus) || check(TokenKind::MinusMinus)) {
            if (!is_lvalue(*expr)) {
                fail("invalid increment");
            }
            const Token op = advance();
            auto update = std::make_unique<UpdateExpr>();
            update->loc = expr->loc;
            update->op = op.kind;
            update->prefix = false;
            update->target = std::move(expr);
            expr = std::move(update);
            continue;
        }
        if (check(TokenKind::Question)) {
            if (looks_like_ternary_question()) {
                break;
            }
            advance();
            if (check(TokenKind::Colon)) {
                fail("invalid postfix ?");
            }
            auto propagate = std::make_unique<PropagateExpr>();
            propagate->loc = expr->loc;
            propagate->operand = std::move(expr);
            expr = std::move(propagate);
            continue;
        }
        break;
    }
    return expr;
}

std::vector<std::unique_ptr<Expr>> Parser::parse_call_arguments() {
    std::vector<std::unique_ptr<Expr>> args;
    if (check(TokenKind::RightParen)) {
        return args;
    }
    if (check(TokenKind::Comma)) {
        fail("invalid call arguments");
    }
    for (;;) {
        args.push_back(parse_expression());
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::Comma) || check(TokenKind::RightParen) || is_at_end()) {
            fail("invalid call arguments", comma);
        }
    }
    return args;
}

std::unique_ptr<Expr> Parser::parse_primary() {
    const Token token = peek();
    switch (token.kind) {
        case TokenKind::Integer:
            advance();
            return make_literal(LiteralExpr::Kind::Integer, token);
        case TokenKind::Float:
            advance();
            return make_literal(LiteralExpr::Kind::Float, token);
        case TokenKind::Duration:
            advance();
            return make_literal(LiteralExpr::Kind::Duration, token);
        case TokenKind::String:
            advance();
            return make_literal(LiteralExpr::Kind::String, token);
        case TokenKind::True:
            advance();
            return make_literal(LiteralExpr::Kind::True, token);
        case TokenKind::False:
            advance();
            return make_literal(LiteralExpr::Kind::False, token);
        case TokenKind::Null:
            advance();
            return make_literal(LiteralExpr::Kind::Null, token);
        case TokenKind::This: {
            advance();
            auto ident = std::make_unique<IdentifierExpr>();
            ident->loc = location_from(token);
            ident->name = "this";
            return ident;
        }
        case TokenKind::Super: {
            advance();
            auto ident = std::make_unique<IdentifierExpr>();
            ident->loc = location_from(token);
            ident->name = "super";
            return ident;
        }
        case TokenKind::Parent: {
            advance();
            auto ident = std::make_unique<IdentifierExpr>();
            ident->loc = location_from(token);
            ident->name = "parent";
            return ident;
        }
        case TokenKind::Identifier:
        case TokenKind::Input: {
            advance();
            auto ident = std::make_unique<IdentifierExpr>();
            ident->loc = location_from(token);
            ident->name = token.lexeme;
            return ident;
        }
        case TokenKind::Match:
            return parse_match_expression();
        case TokenKind::LeftParen:
            return parse_paren_expr();
        case TokenKind::LeftBracket:
            return parse_list_literal();
        case TokenKind::LeftBrace:
            return parse_dict_literal();
        default:
            fail("invalid expression");
    }
}

std::unique_ptr<Expr> Parser::parse_paren_expr() {
    const Token open = expect(TokenKind::LeftParen, "(");
    if (check(TokenKind::RightParen)) {
        advance();
        auto expr = std::make_unique<TupleExpr>();
        expr->loc = location_from(open);
        return expr;
    }
    auto first = parse_expression();
    if (check(TokenKind::RightParen)) {
        advance();
        return first;
    }
    if (!check(TokenKind::Comma)) {
        fail("missing expected token ')'");
    }
    auto expr = std::make_unique<TupleExpr>();
    expr->loc = location_from(open);
    expr->elements.push_back(std::move(first));
    for (;;) {
        const Token comma = advance();
        if (check(TokenKind::RightParen)) {
            if (expr->elements.size() != 1) {
                fail("invalid tuple", comma);
            }
            advance();
            return expr;
        }
        if (check(TokenKind::Comma) || is_at_end()) {
            fail("invalid tuple", comma);
        }
        expr->elements.push_back(parse_expression());
        if (!check(TokenKind::Comma)) {
            break;
        }
    }
    expect(TokenKind::RightParen, ")");
    return expr;
}

std::unique_ptr<Expr> Parser::parse_list_literal() {
    const Token open = expect(TokenKind::LeftBracket, "[");
    auto expr = std::make_unique<ListExpr>();
    expr->loc = location_from(open);
    if (check(TokenKind::RightBracket)) {
        advance();
        return expr;
    }
    for (;;) {
        if (check(TokenKind::Comma) || check(TokenKind::RightBracket) || is_at_end()) {
            fail("invalid list");
        }
        expr->elements.push_back(parse_expression());
        if (!check(TokenKind::Comma)) {
            break;
        }
        const Token comma = advance();
        if (check(TokenKind::Comma) || check(TokenKind::RightBracket) || is_at_end()) {
            fail("invalid list", comma);
        }
    }
    expect(TokenKind::RightBracket, "]");
    return expr;
}

std::unique_ptr<Expr> Parser::parse_dict_literal() {
    const Token open = expect(TokenKind::LeftBrace, "{");
    if (check(TokenKind::RightBrace)) {
        advance();
        auto expr = std::make_unique<DictExpr>();
        expr->loc = location_from(open);
        return expr;
    }

    auto first = parse_expression();
    if (check(TokenKind::Colon)) {
        if (const auto* key = dynamic_cast<const LiteralExpr*>(first.get());
            !key || key->kind != LiteralExpr::Kind::String) {
            fail("dictionary key must be a string", first->loc);
        }
        auto expr = std::make_unique<DictExpr>();
        expr->loc = location_from(open);
        DictEntry entry;
        entry.key = std::move(first);
        advance();
        if (check(TokenKind::Comma) || check(TokenKind::RightBrace) || is_at_end()) {
            fail("invalid dictionary");
        }
        entry.value = parse_expression();
        expr->entries.push_back(std::move(entry));
        while (check(TokenKind::Comma)) {
            const Token comma = advance();
            if (check(TokenKind::Comma) || check(TokenKind::RightBrace) || is_at_end()) {
                fail("invalid dictionary", comma);
            }
            if (!check(TokenKind::String)) {
                fail("dictionary key must be a string");
            }
            DictEntry next;
            next.key = parse_expression();
            expect(TokenKind::Colon, ":");
            if (check(TokenKind::Comma) || check(TokenKind::RightBrace) || is_at_end()) {
                fail("invalid dictionary");
            }
            next.value = parse_expression();
            expr->entries.push_back(std::move(next));
        }
        expect(TokenKind::RightBrace, "}");
        return expr;
    }

    auto expr = std::make_unique<SetExpr>();
    expr->loc = location_from(open);
    expr->elements.push_back(std::move(first));
    while (check(TokenKind::Comma)) {
        const Token comma = advance();
        if (check(TokenKind::Comma) || check(TokenKind::RightBrace) || is_at_end()) {
            fail("invalid set", comma);
        }
        expr->elements.push_back(parse_expression());
    }
    expect(TokenKind::RightBrace, "}");
    return expr;
}

bool Parser::is_comparison(TokenKind kind) {
    return kind == TokenKind::EqualEqual || kind == TokenKind::BangEqual || kind == TokenKind::Less ||
           kind == TokenKind::LessEqual || kind == TokenKind::Greater || kind == TokenKind::GreaterEqual;
}

bool Parser::is_and(TokenKind kind) {
    return kind == TokenKind::And || kind == TokenKind::AmpAmp;
}

bool Parser::is_or(TokenKind kind) {
    return kind == TokenKind::Or || kind == TokenKind::PipePipe;
}

bool Parser::is_additive(TokenKind kind) {
    return kind == TokenKind::Plus || kind == TokenKind::Minus;
}

bool Parser::is_multiplicative(TokenKind kind) {
    return kind == TokenKind::Star || kind == TokenKind::Slash || kind == TokenKind::Percent;
}

bool Parser::is_assign_op(TokenKind kind) {
    return kind == TokenKind::Equal || kind == TokenKind::PlusEqual || kind == TokenKind::MinusEqual ||
           kind == TokenKind::StarEqual || kind == TokenKind::SlashEqual || kind == TokenKind::PercentEqual;
}

bool Parser::is_lvalue(const Expr& expr) {
    return dynamic_cast<const IdentifierExpr*>(&expr) != nullptr ||
           dynamic_cast<const IndexExpr*>(&expr) != nullptr ||
           dynamic_cast<const FieldAccessExpr*>(&expr) != nullptr;
}

std::unique_ptr<LiteralExpr> Parser::make_literal(LiteralExpr::Kind kind, const Token& token) {
    auto expr = std::make_unique<LiteralExpr>();
    expr->loc = location_from(token);
    expr->kind = kind;
    expr->lexeme = token.lexeme;
    return expr;
}

[[noreturn]] void Parser::fail(std::string message) const {
    fail(std::move(message), peek());
}

[[noreturn]] void Parser::fail(std::string message, const Token& token) const {
    throw ParserError{std::move(message), token.line, token.column};
}

[[noreturn]] void Parser::fail(std::string message, const Location& loc) const {
    throw ParserError{std::move(message), loc.line, loc.column};
}

std::unique_ptr<Stmt> Parser::parse_interface_statement(MemberAccess access,
                                                        std::vector<Annotation> leading_annotations, int access_column) {
    std::vector<Annotation> annotations =
        leading_annotations.empty() ? parse_annotations() : std::move(leading_annotations);
    if (check(TokenKind::Protocol)) {
        advance();
    } else {
        expect(TokenKind::Interface, "interface");
    }
    auto stmt = std::make_unique<InterfaceStmt>();
    stmt->access = access;
    stmt->annotations = std::move(annotations);
    const Token name_tok = expect(TokenKind::Identifier, "interface name");
    stmt->name = name_tok.lexeme;
    stmt->loc = location_from(name_tok);
    stmt->type_params = parse_generic_type_params();
    const int header_column = declaration_header_column(access_column, name_tok.column);
    expect(TokenKind::Colon, ":");
    const int member_indent = peek().column;
    while (!is_at_end() && peek().column >= member_indent && peek().kind != TokenKind::Eof) {
        if (peek().column > member_indent) {
            fail("unexpected indent");
        }
        if (check(TokenKind::Func) || check(TokenKind::Async)) {
            bool is_async = false;
            if (check(TokenKind::Async)) {
                is_async = true;
                advance();
            }
            advance();
            InterfaceMethodSig method;
            method.is_async = is_async;
            const Token method_tok = expect(TokenKind::Identifier, "method name");
            method.name = method_tok.lexeme;
            method.loc = location_from(method_tok);
            expect(TokenKind::LeftParen, "(");
            method.parameters = parse_parameters();
            expect(TokenKind::RightParen, ")");
            if (check(TokenKind::MinusGreater)) {
                advance();
                method.return_type_name = parse_type_annotation_string();
            }
            if (check(TokenKind::Colon)) {
                parse_block(header_column);
            }
            stmt->methods.push_back(std::move(method));
            continue;
        }
        if (check(TokenKind::Identifier) && check(TokenKind::Colon, 1)) {
            InterfacePropertySig property;
            property.name = advance().lexeme;
            property.loc = location_from(peek());
            advance();
            property.type_name = parse_type_annotation_string();
            property.has_getter = true;
            stmt->properties.push_back(std::move(property));
            continue;
        }
        if (check(TokenKind::Pass)) {
            advance();
            continue;
        }
        fail("invalid interface member");
    }
    return stmt;
}

Program parse(std::string source) {
    return Parser(Lexer(std::move(source)).tokenize()).parse();
}

std::unique_ptr<Expr> Parser::parse_standalone_expression(const std::string& source, std::string& error) {
    try {
        Parser parser(Lexer(source).tokenize());
        std::unique_ptr<Expr> expr = parser.parse_expression();
        if (!parser.is_at_end()) {
            error = "unexpected tokens after expression";
            return nullptr;
        }
        return expr;
    } catch (const ParserError& parse_error) {
        error = parse_error.message();
        return nullptr;
    }
}

}  // namespace pscript
