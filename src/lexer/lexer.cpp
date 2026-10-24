#include "lexer/lexer.h"

#include "lexer/duration_util.h"

#include <utility>

namespace pscript {
namespace {

bool is_utf8_continuation(unsigned char b) {
    return (b & 0xC0) == 0x80;
}

}  // namespace

LexerError::LexerError(std::string message, int line, int column)
    : std::runtime_error(message),
      message_(std::move(message)),
      line_(line),
      column_(column) {}

Lexer::Lexer(std::string source) : source_(std::move(source)) {
    skip_utf8_bom();
}

void Lexer::skip_utf8_bom() {
    if (source_.size() < 3) {
        return;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(source_.data());
    if (data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        pos_ = 3;
    }
}

Token Lexer::next() {
    skip_trivia();
    if (at_end()) {
        return Token{TokenKind::Eof, line_, column_, {}};
    }
    at_line_start_ = false;

    const int start_line = line_;
    const int start_column = column_;
    const std::size_t start = pos_;
    const char32_t c = peek();

    if (is_identifier_start(c)) {
        return scan_identifier_or_keyword(start_line, start_column, start);
    }
    if (is_digit(c)) {
        return scan_number(start_line, start_column, start);
    }
    if (c == U'.') {
        if (is_digit(peek_ahead(1))) {
            return scan_number(start_line, start_column, start);
        }
        if (peek_ahead(1) == U'.' && peek_ahead(2) == U'.') {
            advance();
            advance();
            advance();
            return make_token(TokenKind::Ellipsis, start_line, start_column, start);
        }
        return scan_simple_token(TokenKind::Dot, start_line, start_column, start);
    }
    if (c == U'@') {
        return scan_simple_token(TokenKind::At, start_line, start_column, start);
    }
    if (c == U'"') {
        return scan_string(start_line, start_column, start);
    }
    if (c == U'=' || c == U'!' || c == U'<' || c == U'>') {
        return scan_operator(start_line, start_column, start);
    }
    if (c == U'&' || c == U'|') {
        return scan_amp_pipe(start_line, start_column, start);
    }
    if (c == U'+' || c == U'-' || c == U'*' || c == U'/' || c == U'%') {
        return scan_arithmetic(start_line, start_column, start);
    }
    if (c == U'~') {
        return scan_simple_token(TokenKind::Tilde, start_line, start_column, start);
    }
    if (c == U'^') {
        return scan_simple_token(TokenKind::Caret, start_line, start_column, start);
    }
    if (c == U'?') {
        if (peek_ahead(1) == U'?') {
            if (peek_ahead(2) == U'=') {
                fail("??= is not supported", start_line, start_column);
            }
            advance();
            advance();
            return make_token(TokenKind::NullCoalesce, start_line, start_column, start);
        }
        return scan_simple_token(TokenKind::Question, start_line, start_column, start);
    }
    if (c == U'(') {
        return scan_simple_token(TokenKind::LeftParen, start_line, start_column, start);
    }
    if (c == U')') {
        return scan_simple_token(TokenKind::RightParen, start_line, start_column, start);
    }
    if (c == U'{') {
        return scan_simple_token(TokenKind::LeftBrace, start_line, start_column, start);
    }
    if (c == U'}') {
        return scan_simple_token(TokenKind::RightBrace, start_line, start_column, start);
    }
    if (c == U'[') {
        return scan_simple_token(TokenKind::LeftBracket, start_line, start_column, start);
    }
    if (c == U']') {
        return scan_simple_token(TokenKind::RightBracket, start_line, start_column, start);
    }
    if (c == U',') {
        return scan_simple_token(TokenKind::Comma, start_line, start_column, start);
    }
    if (c == U':') {
        return scan_simple_token(TokenKind::Colon, start_line, start_column, start);
    }

    fail("invalid character", start_line, start_column);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        Token token = next();
        const bool done = token.kind == TokenKind::Eof;
        tokens.push_back(std::move(token));
        if (done) {
            break;
        }
    }
    return tokens;
}

bool Lexer::at_end() const {
    return pos_ >= source_.size();
}

bool Lexer::try_decode(std::size_t index, CodePoint& out) const {
    if (index >= source_.size()) {
        return false;
    }

    const auto* data = reinterpret_cast<const unsigned char*>(source_.data());
    const unsigned char b0 = data[index];
    char32_t value = 0;
    int nbytes = 0;

    if (b0 <= 0x7F) {
        value = b0;
        nbytes = 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        nbytes = 2;
        if (index + 1 >= source_.size() || !is_utf8_continuation(data[index + 1])) {
            return false;
        }
        value = (static_cast<char32_t>(b0 & 0x1F) << 6) |
                static_cast<char32_t>(data[index + 1] & 0x3F);
        if (value < 0x80) {
            return false;
        }
    } else if ((b0 & 0xF0) == 0xE0) {
        nbytes = 3;
        if (index + 2 >= source_.size() || !is_utf8_continuation(data[index + 1]) ||
            !is_utf8_continuation(data[index + 2])) {
            return false;
        }
        value = (static_cast<char32_t>(b0 & 0x0F) << 12) |
                (static_cast<char32_t>(data[index + 1] & 0x3F) << 6) |
                static_cast<char32_t>(data[index + 2] & 0x3F);
        if (value < 0x800 || (value >= 0xD800 && value <= 0xDFFF)) {
            return false;
        }
    } else if ((b0 & 0xF8) == 0xF0) {
        nbytes = 4;
        if (index + 3 >= source_.size() || !is_utf8_continuation(data[index + 1]) ||
            !is_utf8_continuation(data[index + 2]) || !is_utf8_continuation(data[index + 3])) {
            return false;
        }
        value = (static_cast<char32_t>(b0 & 0x07) << 18) |
                (static_cast<char32_t>(data[index + 1] & 0x3F) << 12) |
                (static_cast<char32_t>(data[index + 2] & 0x3F) << 6) |
                static_cast<char32_t>(data[index + 3] & 0x3F);
        if (value < 0x10000 || value > 0x10FFFF) {
            return false;
        }
    } else {
        return false;
    }

    out.value = value;
    out.nbytes = nbytes;
    return true;
}

Lexer::CodePoint Lexer::peek_cp() const {
    CodePoint cp;
    if (!try_decode(pos_, cp)) {
        if (at_end()) {
            return CodePoint{};
        }
        fail("invalid character", line_, column_);
    }
    return cp;
}

char32_t Lexer::peek() const {
    if (at_end()) {
        return 0;
    }
    return peek_cp().value;
}

char32_t Lexer::peek_ahead(std::size_t code_points) const {
    std::size_t index = pos_;
    for (std::size_t i = 0; i <= code_points; ++i) {
        CodePoint cp;
        if (!try_decode(index, cp)) {
            return 0;
        }
        if (i == code_points) {
            return cp.value;
        }
        index += static_cast<std::size_t>(cp.nbytes);
    }
    return 0;
}

bool Lexer::match_ascii(char expected) const {
    return !at_end() && peek() == static_cast<unsigned char>(expected);
}

void Lexer::advance() {
    if (at_end()) {
        return;
    }
    const CodePoint cp = peek_cp();
    pos_ += static_cast<std::size_t>(cp.nbytes);
    column_ += 1;
}

void Lexer::skip_trivia() {
    for (;;) {
        if (at_end()) {
            return;
        }
        const char32_t c = peek();
        if (c == U'\t') {
            if (at_line_start_) {
                fail("invalid indent", line_, column_);
            }
            advance();
            continue;
        }
        if (c == U' ') {
            advance();
            continue;
        }
        if (skip_newline()) {
            at_line_start_ = true;
            continue;
        }
        if (c == U'#') {
            advance();
            while (!at_end() && !is_newline_start(peek())) {
                advance();
            }
            continue;
        }
        return;
    }
}

bool Lexer::skip_newline() {
    if (at_end()) {
        return false;
    }
    const char32_t c = peek();
    if (c == U'\n') {
        advance();
        line_ += 1;
        column_ = 1;
        return true;
    }
    if (c == U'\r') {
        advance();
        if (!at_end() && peek() == U'\n') {
            pos_ += 1;
        }
        line_ += 1;
        column_ = 1;
        return true;
    }
    return false;
}

Token Lexer::make_token(TokenKind kind, int line, int column, std::size_t start) const {
    return Token{kind, line, column, source_.substr(start, pos_ - start)};
}

Token Lexer::scan_simple_token(TokenKind kind, int line, int column, std::size_t start) {
    advance();
    return make_token(kind, line, column, start);
}

Token Lexer::scan_identifier_or_keyword(int line, int column, std::size_t start) {
    advance();
    while (!at_end() && is_identifier_continue(peek())) {
        advance();
    }
    Token token = make_token(TokenKind::Identifier, line, column, start);
    token.kind = keyword_kind(token.lexeme);
    return token;
}

Token Lexer::scan_number(int line, int column, std::size_t start) {
    if (peek() == U'.') {
        fail("invalid numeric form", line, column);
    }

    while (!at_end() && is_digit(peek())) {
        advance();
    }

    if (!at_end() && peek() == U'.') {
        const char32_t after_dot = peek_ahead(1);
        if (!is_digit(after_dot)) {
            fail("invalid numeric form", line, column);
        }
        advance();
        while (!at_end() && is_digit(peek())) {
            advance();
        }
        if (!at_end() && peek() == U'.') {
            fail("invalid numeric form", line, column);
        }
        if (!at_end() && peek() == U'm' && peek_ahead(1) == U's') {
            advance();
            advance();
            const Token token = make_token(TokenKind::Duration, line, column, start);
            if (!duration_lexeme_to_milliseconds(token.lexeme)) {
                fail("invalid duration literal", line, column);
            }
            return token;
        }
        if (!at_end() && (peek() == U's' || peek() == U'm' || peek() == U'h')) {
            advance();
            const Token token = make_token(TokenKind::Duration, line, column, start);
            if (!duration_lexeme_to_milliseconds(token.lexeme)) {
                fail("invalid duration literal", line, column);
            }
            return token;
        }
        if (!at_end() && is_identifier_start(peek())) {
            fail("invalid numeric form", line, column);
        }
        return make_token(TokenKind::Float, line, column, start);
    }

    if (!at_end() && peek() == U'm' && peek_ahead(1) == U's') {
        advance();
        advance();
        const Token token = make_token(TokenKind::Duration, line, column, start);
        if (!duration_lexeme_to_milliseconds(token.lexeme)) {
            fail("invalid duration literal", line, column);
        }
        return token;
    }
    if (!at_end() && (peek() == U's' || peek() == U'm' || peek() == U'h')) {
        advance();
        const Token token = make_token(TokenKind::Duration, line, column, start);
        if (!duration_lexeme_to_milliseconds(token.lexeme)) {
            fail("invalid duration literal", line, column);
        }
        return token;
    }
    if (!at_end() && is_identifier_start(peek())) {
        fail("invalid numeric form", line, column);
    }

    return make_token(TokenKind::Integer, line, column, start);
}

Token Lexer::scan_string(int line, int column, std::size_t start) {
    advance();

    while (!at_end()) {
        const char32_t c = peek();
        if (is_newline_start(c)) {
            fail("unterminated string", line, column);
        }
        if (c == U'"') {
            advance();
            return make_token(TokenKind::String, line, column, start);
        }
        if (c == U'\\') {
            const int escape_line = line_;
            const int escape_column = column_;
            advance();
            if (at_end() || is_newline_start(peek())) {
                fail("invalid escape", escape_line, escape_column);
            }
            const char32_t escaped = peek();
            if (escaped != U'"' && escaped != U'\\' && escaped != U'n' && escaped != U't' &&
                escaped != U'r') {
                fail("invalid escape", escape_line, escape_column);
            }
            advance();
            continue;
        }
        advance();
    }

    fail("unterminated string", line, column);
}

Token Lexer::scan_operator(int line, int column, std::size_t start) {
    const char32_t c = peek();
    advance();

    if (c == U'=') {
        if (match_ascii('=')) {
            advance();
            return make_token(TokenKind::EqualEqual, line, column, start);
        }
        return make_token(TokenKind::Equal, line, column, start);
    }
    if (c == U'!') {
        if (match_ascii('=')) {
            advance();
            return make_token(TokenKind::BangEqual, line, column, start);
        }
        return make_token(TokenKind::Bang, line, column, start);
    }
    if (c == U'<') {
        if (match_ascii('<')) {
            advance();
            return make_token(TokenKind::LessLess, line, column, start);
        }
        if (match_ascii('=')) {
            advance();
            return make_token(TokenKind::LessEqual, line, column, start);
        }
        return make_token(TokenKind::Less, line, column, start);
    }
    if (match_ascii('>')) {
        advance();
        return make_token(TokenKind::GreaterGreater, line, column, start);
    }
    if (match_ascii('=')) {
        advance();
        return make_token(TokenKind::GreaterEqual, line, column, start);
    }
    return make_token(TokenKind::Greater, line, column, start);
}

Token Lexer::scan_amp_pipe(int line, int column, std::size_t start) {
    const char32_t c = peek();
    advance();
    if (c == U'&') {
        if (match_ascii('&')) {
            advance();
            return make_token(TokenKind::AmpAmp, line, column, start);
        }
        return make_token(TokenKind::Amp, line, column, start);
    }
    if (c == U'|') {
        if (match_ascii('|')) {
            advance();
            return make_token(TokenKind::PipePipe, line, column, start);
        }
        if (match_ascii('>')) {
            advance();
            return make_token(TokenKind::PipeGreater, line, column, start);
        }
        return make_token(TokenKind::Pipe, line, column, start);
    }
    return make_token(TokenKind::Pipe, line, column, start);
}

Token Lexer::scan_arithmetic(int line, int column, std::size_t start) {
    const char32_t c = peek();
    advance();
    if (c == U'+') {
        if (match_ascii('+')) {
            advance();
            return make_token(TokenKind::PlusPlus, line, column, start);
        }
        if (match_ascii('=')) {
            advance();
            return make_token(TokenKind::PlusEqual, line, column, start);
        }
        return make_token(TokenKind::Plus, line, column, start);
    }
    if (c == U'-') {
        if (match_ascii('-')) {
            advance();
            return make_token(TokenKind::MinusMinus, line, column, start);
        }
        if (match_ascii('>')) {
            advance();
            return make_token(TokenKind::MinusGreater, line, column, start);
        }
        if (match_ascii('=')) {
            advance();
            return make_token(TokenKind::MinusEqual, line, column, start);
        }
        return make_token(TokenKind::Minus, line, column, start);
    }
    if (c == U'*') {
        if (match_ascii('=')) {
            advance();
            return make_token(TokenKind::StarEqual, line, column, start);
        }
        return make_token(TokenKind::Star, line, column, start);
    }
    if (c == U'/') {
        if (match_ascii('=')) {
            advance();
            return make_token(TokenKind::SlashEqual, line, column, start);
        }
        return make_token(TokenKind::Slash, line, column, start);
    }
    if (match_ascii('=')) {
        advance();
        return make_token(TokenKind::PercentEqual, line, column, start);
    }
    return make_token(TokenKind::Percent, line, column, start);
}

[[noreturn]] void Lexer::fail(std::string_view message, int line, int column) const {
    throw LexerError{std::string(message), line, column};
}

TokenKind Lexer::keyword_kind(std::string_view lexeme) {
    if (lexeme == "true") {
        return TokenKind::True;
    }
    if (lexeme == "false") {
        return TokenKind::False;
    }
    if (lexeme == "null") {
        return TokenKind::Null;
    }
    if (lexeme == "const") {
        return TokenKind::Const;
    }
    if (lexeme == "if") {
        return TokenKind::If;
    }
    if (lexeme == "elif") {
        return TokenKind::Elif;
    }
    if (lexeme == "elseif") {
        return TokenKind::ElseIf;
    }
    if (lexeme == "else") {
        return TokenKind::Else;
    }
    if (lexeme == "for") {
        return TokenKind::For;
    }
    if (lexeme == "with") {
        return TokenKind::With;
    }
    if (lexeme == "while") {
        return TokenKind::While;
    }
    if (lexeme == "in") {
        return TokenKind::In;
    }
    if (lexeme == "break") {
        return TokenKind::Break;
    }
    if (lexeme == "continue") {
        return TokenKind::Continue;
    }
    if (lexeme == "func") {
        return TokenKind::Func;
    }
    if (lexeme == "async") {
        return TokenKind::Async;
    }
    if (lexeme == "await") {
        return TokenKind::Await;
    }
    if (lexeme == "return") {
        return TokenKind::Return;
    }
    if (lexeme == "struct") {
        return TokenKind::Struct;
    }
    if (lexeme == "record") {
        return TokenKind::Record;
    }
    if (lexeme == "enum") {
        return TokenKind::Enum;
    }
    if (lexeme == "class") {
        return TokenKind::Class;
    }
    if (lexeme == "constructor") {
        return TokenKind::Constructor;
    }
    if (lexeme == "this") {
        return TokenKind::This;
    }
    if (lexeme == "super") {
        return TokenKind::Super;
    }
    if (lexeme == "parent") {
        return TokenKind::Parent;
    }
    if (lexeme == "public") {
        return TokenKind::Public;
    }
    if (lexeme == "pub") {
        return TokenKind::Pub;
    }
    if (lexeme == "private") {
        return TokenKind::Private;
    }
    if (lexeme == "priv") {
        return TokenKind::Priv;
    }
    if (lexeme == "protected") {
        return TokenKind::Protected;
    }
    if (lexeme == "prot") {
        return TokenKind::Prot;
    }
    if (lexeme == "pass") {
        return TokenKind::Pass;
    }
    if (lexeme == "import") {
        return TokenKind::Import;
    }
    if (lexeme == "from") {
        return TokenKind::From;
    }
    if (lexeme == "as") {
        return TokenKind::As;
    }
    if (lexeme == "match") {
        return TokenKind::Match;
    }
    if (lexeme == "typealias") {
        return TokenKind::TypeAlias;
    }
    if (lexeme == "defer") {
        return TokenKind::Defer;
    }
    if (lexeme == "try") {
        return TokenKind::Try;
    }
    if (lexeme == "catch") {
        return TokenKind::Catch;
    }
    if (lexeme == "finally") {
        return TokenKind::Finally;
    }
    if (lexeme == "throw") {
        return TokenKind::Throw;
    }
    if (lexeme == "input") {
        return TokenKind::Input;
    }
    if (lexeme == "and") {
        return TokenKind::And;
    }
    if (lexeme == "or") {
        return TokenKind::Or;
    }
    if (lexeme == "not") {
        return TokenKind::Not;
    }
    if (lexeme == "interface") {
        return TokenKind::Interface;
    }
    if (lexeme == "protocol") {
        return TokenKind::Protocol;
    }
    return TokenKind::Identifier;
}

bool Lexer::is_ascii_letter(char32_t c) {
    return (c >= U'A' && c <= U'Z') || (c >= U'a' && c <= U'z');
}

bool Lexer::is_digit(char32_t c) {
    return c >= U'0' && c <= U'9';
}

bool Lexer::is_identifier_start(char32_t c) {
    return is_ascii_letter(c) || c == U'_';
}

bool Lexer::is_identifier_continue(char32_t c) {
    return is_identifier_start(c) || is_digit(c);
}

bool Lexer::is_newline_start(char32_t c) {
    return c == U'\n' || c == U'\r';
}

}  // namespace pscript
