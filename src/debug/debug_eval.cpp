#include "debug/debug_eval.h"

#include "parser/ast.h"
#include "parser/parser.h"

#include <cctype>
#include <stdexcept>

namespace pscript::debug {
namespace {

std::int64_t parse_integer_lexeme(const std::string& lexeme) {
    std::size_t idx = 0;
    const long long value = std::stoll(lexeme, &idx, 10);
    if (idx != lexeme.size()) {
        throw std::runtime_error("invalid integer literal");
    }
    return static_cast<std::int64_t>(value);
}

double parse_float_lexeme(const std::string& lexeme) {
    std::size_t idx = 0;
    const double value = std::stod(lexeme, &idx);
    if (idx != lexeme.size()) {
        throw std::runtime_error("invalid float literal");
    }
    return value;
}

std::string decode_string_lexeme(const std::string& lexeme) {
    if (lexeme.size() < 2 || lexeme.front() != '"' || lexeme.back() != '"') {
        throw std::runtime_error("invalid string literal");
    }
    std::string out;
    out.reserve(lexeme.size() - 2);
    for (std::size_t i = 1; i + 1 < lexeme.size(); ++i) {
        const char c = lexeme[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= lexeme.size() - 1) {
            throw std::runtime_error("invalid string escape");
        }
        const char next = lexeme[++i];
        if (next == 'n') {
            out.push_back('\n');
        } else if (next == 't') {
            out.push_back('\t');
        } else if (next == '"' || next == '\\') {
            out.push_back(next);
        } else {
            out.push_back(next);
        }
    }
    return out;
}

bool is_identifier_start(char ch) {
    return std::isalpha(static_cast<unsigned char>(ch)) || ch == '_';
}

bool is_identifier_part(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

bool is_truthy(const Value& value) {
    switch (value.kind) {
        case ValueKind::Null:
            return false;
        case ValueKind::Bool:
            return value.boolean;
        case ValueKind::Integer:
            return value.integer != 0;
        case ValueKind::Float:
            return value.floating != 0.0;
        case ValueKind::String:
            return !value.string.empty();
        default:
            return true;
    }
}

bool is_number(const Value& value) {
    return value.kind == ValueKind::Integer || value.kind == ValueKind::Float;
}

double as_number(const Value& value) {
    return value.kind == ValueKind::Integer ? static_cast<double>(value.integer) : value.floating;
}

Value make_bool(bool value) {
    Value out;
    out.kind = ValueKind::Bool;
    out.boolean = value;
    return out;
}

class ExprInterpreter {
public:
    explicit ExprInterpreter(const VmInspector& inspector) : inspector_(inspector) {}

    std::optional<Value> eval(const Expr* expr, EvalError& error) const {
        if (expr == nullptr) {
            error.message = "invalid expression";
            return std::nullopt;
        }
        if (const auto* literal = dynamic_cast<const LiteralExpr*>(expr)) {
            return eval_literal(*literal);
        }
        if (const auto* ident = dynamic_cast<const IdentifierExpr*>(expr)) {
            return lookup_name(ident->name, error);
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
            return eval_unary(*unary, error);
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
            return eval_binary(*binary, error);
        }
        if (const auto* field = dynamic_cast<const FieldAccessExpr*>(expr)) {
            return eval_field(*field, error);
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
            return eval_index(*index, error);
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
            return eval_call(*call, error);
        }
        if (const auto* method = dynamic_cast<const MethodCallExpr*>(expr)) {
            return eval_method(*method, error);
        }
        if (const auto* list = dynamic_cast<const ListExpr*>(expr)) {
            return eval_list(*list, error);
        }
        if (const auto* ternary = dynamic_cast<const TernaryExpr*>(expr)) {
            return eval_ternary(*ternary, error);
        }
        if (dynamic_cast<const AssignExpr*>(expr) != nullptr || dynamic_cast<const UpdateExpr*>(expr) != nullptr) {
            error.message = "assignment not supported in debugger evaluation";
            return std::nullopt;
        }
        error.message = "unsupported expression syntax";
        return std::nullopt;
    }

private:
    const VmInspector& inspector_;

    std::optional<Value> lookup_name(const std::string& name, EvalError& error) const {
        const std::vector<DebugStackFrame> frames = inspector_.stack_frames();
        if (!frames.empty()) {
            for (const DebugVariable& local : frames.back().locals) {
                if (local.name == name && local.in_scope) {
                    return local.value;
                }
            }
            for (const DebugVariable& up : frames.back().upvalues) {
                if (up.name == name) {
                    return up.value;
                }
            }
        }
        const std::optional<Value> global = inspector_.global_variable(name);
        if (!global.has_value()) {
            error.message = "undefined name '" + name + "'";
            return std::nullopt;
        }
        return global;
    }

    static std::optional<Value> eval_literal(const LiteralExpr& literal) {
        switch (literal.kind) {
            case LiteralExpr::Kind::Integer:
                return Runtime::make_integer(parse_integer_lexeme(literal.lexeme));
            case LiteralExpr::Kind::Float:
                return Runtime::make_float(parse_float_lexeme(literal.lexeme));
            case LiteralExpr::Kind::String:
                return Runtime::make_string(decode_string_lexeme(literal.lexeme));
            case LiteralExpr::Kind::True:
                return Runtime::make_bool(true);
            case LiteralExpr::Kind::False:
                return Runtime::make_bool(false);
            case LiteralExpr::Kind::Null:
                return Runtime::make_null();
            default:
                return std::nullopt;
        }
    }

    std::optional<Value> eval_unary(const UnaryExpr& unary, EvalError& error) const {
        const auto operand = eval(unary.operand.get(), error);
        if (!operand.has_value()) {
            return std::nullopt;
        }
        if (unary.op == TokenKind::Not) {
            return make_bool(!is_truthy(*operand));
        }
        if (unary.op == TokenKind::Minus) {
            if (operand->kind == ValueKind::Integer) {
                return Runtime::make_integer(-operand->integer);
            }
            if (operand->kind == ValueKind::Float) {
                return Runtime::make_float(-operand->floating);
            }
        }
        error.message = "unsupported unary operator";
        return std::nullopt;
    }

    std::optional<Value> eval_binary(const BinaryExpr& binary, EvalError& error) const {
        if (binary.op == TokenKind::And) {
            const auto left = eval(binary.left.get(), error);
            if (!left.has_value()) {
                return std::nullopt;
            }
            if (!is_truthy(*left)) {
                return left;
            }
            return eval(binary.right.get(), error);
        }
        if (binary.op == TokenKind::Or) {
            const auto left = eval(binary.left.get(), error);
            if (!left.has_value()) {
                return std::nullopt;
            }
            if (is_truthy(*left)) {
                return left;
            }
            return eval(binary.right.get(), error);
        }
        const auto left = eval(binary.left.get(), error);
        if (!left.has_value()) {
            return std::nullopt;
        }
        const auto right = eval(binary.right.get(), error);
        if (!right.has_value()) {
            return std::nullopt;
        }
        try {
            return eval_binary_values(binary.op, *left, *right, error);
        } catch (const std::exception&) {
            error.message = "runtime error during expression evaluation";
            return std::nullopt;
        }
    }

    std::optional<Value> eval_binary_values(TokenKind op, const Value& left, const Value& right, EvalError& error) const {
        switch (op) {
            case TokenKind::Plus:
                if (left.kind == ValueKind::String && right.kind == ValueKind::String) {
                    return Runtime::concat_strings(left.string, right.string);
                }
                if (is_number(left) && is_number(right)) {
                    if (left.kind == ValueKind::Integer && right.kind == ValueKind::Integer) {
                        return Runtime::make_integer(left.integer + right.integer);
                    }
                    return Runtime::make_float(as_number(left) + as_number(right));
                }
                break;
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Percent:
                if (!is_number(left) || !is_number(right)) {
                    break;
                }
                if (left.kind == ValueKind::Integer && right.kind == ValueKind::Integer) {
                    switch (op) {
                        case TokenKind::Minus:
                            return Runtime::make_integer(left.integer - right.integer);
                        case TokenKind::Star:
                            return Runtime::make_integer(left.integer * right.integer);
                        case TokenKind::Slash:
                            if (right.integer == 0) {
                                error.message = "division by zero";
                                return std::nullopt;
                            }
                            return Runtime::make_integer(left.integer / right.integer);
                        case TokenKind::Percent:
                            if (right.integer == 0) {
                                error.message = "modulo by zero";
                                return std::nullopt;
                            }
                            return Runtime::make_integer(left.integer % right.integer);
                        default:
                            break;
                    }
                } else {
                    const double a = as_number(left);
                    const double b = as_number(right);
                    if (op == TokenKind::Slash && b == 0.0) {
                        error.message = "division by zero";
                        return std::nullopt;
                    }
                    switch (op) {
                        case TokenKind::Minus:
                            return Runtime::make_float(a - b);
                        case TokenKind::Star:
                            return Runtime::make_float(a * b);
                        case TokenKind::Slash:
                            return Runtime::make_float(a / b);
                        default:
                            break;
                    }
                }
                break;
            case TokenKind::EqualEqual:
                return make_bool(Runtime::equals(left, right));
            case TokenKind::BangEqual:
                return make_bool(!Runtime::equals(left, right));
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
                if (is_number(left) && is_number(right)) {
                    const double a = as_number(left);
                    const double b = as_number(right);
                    switch (op) {
                        case TokenKind::Less:
                            return make_bool(a < b);
                        case TokenKind::LessEqual:
                            return make_bool(a <= b);
                        case TokenKind::Greater:
                            return make_bool(a > b);
                        case TokenKind::GreaterEqual:
                            return make_bool(a >= b);
                        default:
                            break;
                    }
                }
                break;
            default:
                break;
        }
        error.message = "unsupported binary operator for operand types";
        return std::nullopt;
    }

    std::optional<Value> eval_field(const FieldAccessExpr& field, EvalError& error) const {
        const auto object = eval(field.object.get(), error);
        if (!object.has_value()) {
            return std::nullopt;
        }
        if (object->kind == ValueKind::Class && object->class_value) {
            for (std::size_t i = 0; i < object->class_value->field_names.size(); ++i) {
                if (object->class_value->field_names[i] == field.field) {
                    return object->class_value->fields[i];
                }
            }
            error.message = "unknown field '" + field.field + "'";
            return std::nullopt;
        }
        error.message = "cannot access field on non-object value";
        return std::nullopt;
    }

    std::optional<Value> eval_index(const IndexExpr& index, EvalError& error) const {
        const auto object = eval(index.object.get(), error);
        if (!object.has_value()) {
            return std::nullopt;
        }
        const auto key = eval(index.index.get(), error);
        if (!key.has_value()) {
            return std::nullopt;
        }
        if (object->kind == ValueKind::List && object->list) {
            if (key->kind != ValueKind::Integer) {
                error.message = "list index must be integer";
                return std::nullopt;
            }
            const std::int64_t idx = key->integer;
            if (idx < 0 || static_cast<std::size_t>(idx) >= object->list->items.size()) {
                error.message = "list index out of range";
                return std::nullopt;
            }
            return object->list->items[static_cast<std::size_t>(idx)];
        }
        if (object->kind == ValueKind::Dict && object->dict) {
            if (key->kind != ValueKind::String) {
                error.message = "dict key must be string";
                return std::nullopt;
            }
            for (const auto& entry : object->dict->entries) {
                if (entry.first == key->string) {
                    return entry.second;
                }
            }
            error.message = "key not found";
            return std::nullopt;
        }
        error.message = "value not indexable";
        return std::nullopt;
    }

    std::optional<Value> eval_call(const CallExpr& call, EvalError& error) const {
        std::optional<Value> callee = lookup_name(call.name, error);
        if (!callee.has_value()) {
            error.message.clear();
            callee = inspector_.function_value(call.name);
        }
        if (!callee.has_value()) {
            error.message = "undefined name '" + call.name + "'";
            return std::nullopt;
        }
        std::vector<Value> args;
        args.reserve(call.arguments.size());
        for (const auto& arg_expr : call.arguments) {
            const auto arg = eval(arg_expr.get(), error);
            if (!arg.has_value()) {
                return std::nullopt;
            }
            args.push_back(*arg);
        }
        try {
            return inspector_.invoke_callable(*callee, args);
        } catch (const std::exception&) {
            error.message = "runtime error during call";
            return std::nullopt;
        }
    }

    std::optional<Value> eval_method(const MethodCallExpr& method, EvalError& error) const {
        const auto object = eval(method.object.get(), error);
        if (!object.has_value()) {
            return std::nullopt;
        }
        error.message = "method calls not supported in debugger evaluation";
        return std::nullopt;
    }

    std::optional<Value> eval_list(const ListExpr& list, EvalError& error) const {
        std::vector<Value> items;
        for (const auto& element : list.elements) {
            const auto item = eval(element.get(), error);
            if (!item.has_value()) {
                return std::nullopt;
            }
            items.push_back(*item);
        }
        return Runtime::make_list(std::move(items));
    }

    std::optional<Value> eval_ternary(const TernaryExpr& ternary, EvalError& error) const {
        const auto condition = eval(ternary.condition.get(), error);
        if (!condition.has_value()) {
            return std::nullopt;
        }
        if (is_truthy(*condition)) {
            return eval(ternary.then_expr.get(), error);
        }
        return eval(ternary.else_expr.get(), error);
    }
};

}  // namespace

ExpressionEvaluator::ExpressionEvaluator(const VmInspector& inspector) : inspector_(inspector) {}

std::optional<Value> ExpressionEvaluator::evaluate_path(const std::string& expression, EvalError& error) const {
    std::size_t pos = 0;
    while (pos < expression.size() && std::isspace(static_cast<unsigned char>(expression[pos]))) {
        ++pos;
    }
    if (pos >= expression.size() || !is_identifier_start(expression[pos])) {
        error.message = "invalid expression";
        return std::nullopt;
    }
    std::size_t start = pos;
    ++pos;
    while (pos < expression.size() && is_identifier_part(expression[pos])) {
        ++pos;
    }
    const std::string root = expression.substr(start, pos - start);
    std::optional<Value> current = lookup_name(root);
    if (!current.has_value()) {
        error.message = "undefined name '" + root + "'";
        return std::nullopt;
    }
    while (pos < expression.size()) {
        while (pos < expression.size() && std::isspace(static_cast<unsigned char>(expression[pos]))) {
            ++pos;
        }
        if (pos >= expression.size()) {
            break;
        }
        if (expression[pos] == '.') {
            ++pos;
            start = pos;
            if (pos >= expression.size() || !is_identifier_start(expression[pos])) {
                error.message = "expected field name";
                return std::nullopt;
            }
            ++pos;
            while (pos < expression.size() && is_identifier_part(expression[pos])) {
                ++pos;
            }
            const std::string field = expression.substr(start, pos - start);
            if (current->kind != ValueKind::Class || !current->class_value) {
                error.message = "cannot access field on non-object value";
                return std::nullopt;
            }
            bool found = false;
            for (std::size_t i = 0; i < current->class_value->field_names.size(); ++i) {
                if (current->class_value->field_names[i] == field) {
                    current = current->class_value->fields[i];
                    found = true;
                    break;
                }
            }
            if (!found) {
                error.message = "unknown field '" + field + "'";
                return std::nullopt;
            }
            continue;
        }
        if (expression[pos] == '[') {
            ++pos;
            start = pos;
            while (pos < expression.size() && expression[pos] != ']') {
                ++pos;
            }
            if (pos >= expression.size()) {
                error.message = "unterminated index";
                return std::nullopt;
            }
            const std::string index_text = expression.substr(start, pos - start);
            ++pos;
            std::optional<Value> index_value;
            try {
                const std::int64_t as_int = std::stoll(index_text);
                index_value = Runtime::make_integer(as_int);
            } catch (...) {
                EvalError index_error;
                index_value = evaluate_path(index_text, index_error);
            }
            if (!index_value.has_value()) {
                error.message = "invalid index expression";
                return std::nullopt;
            }
            if (current->kind == ValueKind::List && current->list) {
                if (index_value->kind != ValueKind::Integer) {
                    error.message = "list index must be integer";
                    return std::nullopt;
                }
                const std::int64_t idx = index_value->integer;
                if (idx < 0 || static_cast<std::size_t>(idx) >= current->list->items.size()) {
                    error.message = "list index out of range";
                    return std::nullopt;
                }
                current = current->list->items[static_cast<std::size_t>(idx)];
                continue;
            }
            if (current->kind == ValueKind::Dict && current->dict) {
                if (index_value->kind != ValueKind::String) {
                    error.message = "dict key must be string";
                    return std::nullopt;
                }
                bool found = false;
                for (const auto& entry : current->dict->entries) {
                    if (entry.first == index_value->string) {
                        current = entry.second;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    error.message = "key not found";
                    return std::nullopt;
                }
                continue;
            }
            error.message = "value not indexable";
            return std::nullopt;
        }
        error.message = "unsupported expression syntax";
        return std::nullopt;
    }
    while (pos < expression.size() && std::isspace(static_cast<unsigned char>(expression[pos]))) {
        ++pos;
    }
    if (pos != expression.size()) {
        error.message = "unsupported expression syntax";
        return std::nullopt;
    }
    return current;
}

std::optional<Value> ExpressionEvaluator::lookup_name(const std::string& name) const {
    const std::vector<DebugStackFrame> frames = inspector_.stack_frames();
    if (!frames.empty()) {
        for (const DebugVariable& local : frames.back().locals) {
            if (local.name == name && local.in_scope) {
                return local.value;
            }
        }
        for (const DebugVariable& up : frames.back().upvalues) {
            if (up.name == name) {
                return up.value;
            }
        }
    }
    return inspector_.global_variable(name);
}

bool looks_like_assignment(const std::string& expression) {
    for (std::size_t i = 0; i < expression.size(); ++i) {
        if (expression[i] != '=') {
            continue;
        }
        if (i > 0 && expression[i - 1] == '=') {
            continue;
        }
        if (i + 1 < expression.size() && expression[i + 1] == '=') {
            continue;
        }
        if (i > 0 && (expression[i - 1] == '!' || expression[i - 1] == '<' || expression[i - 1] == '>')) {
            continue;
        }
        return true;
    }
    return false;
}

std::optional<Value> ExpressionEvaluator::evaluate(const std::string& expression, EvalError& error) const {
    if (looks_like_assignment(expression)) {
        error.message = "assignment not supported in debugger evaluation";
        return std::nullopt;
    }
    std::string trimmed = expression;
    std::size_t pos = 0;
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos]))) {
        ++pos;
    }
    if (pos > 0) {
        trimmed = trimmed.substr(pos);
    }
    if (trimmed.empty()) {
        error.message = "invalid expression";
        return std::nullopt;
    }
    if (std::isdigit(static_cast<unsigned char>(trimmed[0])) ||
        (trimmed[0] == '-' && trimmed.size() > 1 && std::isdigit(static_cast<unsigned char>(trimmed[1])))) {
        try {
            const std::int64_t value = std::stoll(trimmed);
            return Runtime::make_integer(value);
        } catch (...) {
        }
    }
    if (trimmed.find('(') == std::string::npos && trimmed.find('+') == std::string::npos &&
        trimmed.find('-') == std::string::npos && trimmed.find('*') == std::string::npos &&
        trimmed.find('/') == std::string::npos && trimmed.find("==") == std::string::npos &&
        trimmed.find("!=") == std::string::npos && trimmed.find('<') == std::string::npos &&
        trimmed.find('>') == std::string::npos && trimmed.find(" and ") == std::string::npos &&
        trimmed.find(" or ") == std::string::npos) {
        return evaluate_path(trimmed, error);
    }
    std::string parse_error;
    const std::unique_ptr<Expr> ast = Parser::parse_standalone_expression(trimmed, parse_error);
    if (!ast) {
        error.message = parse_error.empty() ? "invalid expression" : parse_error;
        return std::nullopt;
    }
    return ExprInterpreter(inspector_).eval(ast.get(), error);
}

}  // namespace pscript::debug
