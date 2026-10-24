#pragma once

#include "lexer/token.h"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pscript {

class LexerError : public std::runtime_error {
public:
    LexerError(std::string message, int line, int column);

    int line() const { return line_; }
    int column() const { return column_; }
    const std::string& message() const { return message_; }

private:
    std::string message_;
    int line_;
    int column_;
};

class Lexer {
public:
    explicit Lexer(std::string source);

    Token next();
    std::vector<Token> tokenize();

private:
    struct CodePoint {
        char32_t value = 0;
        int nbytes = 0;
    };

    void skip_utf8_bom();
    bool at_end() const;
    bool try_decode(std::size_t index, CodePoint& out) const;
    CodePoint peek_cp() const;
    char32_t peek() const;
    char32_t peek_ahead(std::size_t code_points) const;
    bool match_ascii(char expected) const;

    void advance();
    void skip_trivia();
    bool skip_newline();

    Token make_token(TokenKind kind, int line, int column, std::size_t start) const;
    Token scan_identifier_or_keyword(int line, int column, std::size_t start);
    Token scan_number(int line, int column, std::size_t start);
    Token scan_string(int line, int column, std::size_t start);
    Token scan_operator(int line, int column, std::size_t start);
    Token scan_amp_pipe(int line, int column, std::size_t start);
    Token scan_arithmetic(int line, int column, std::size_t start);
    Token scan_simple_token(TokenKind kind, int line, int column, std::size_t start);

    [[noreturn]] void fail(std::string_view message, int line, int column) const;

    static TokenKind keyword_kind(std::string_view lexeme);

    static bool is_ascii_letter(char32_t c);
    static bool is_digit(char32_t c);
    static bool is_identifier_start(char32_t c);
    static bool is_identifier_continue(char32_t c);
    static bool is_newline_start(char32_t c);

    std::string source_;
    std::size_t pos_ = 0;
    int line_ = 1;
    int column_ = 1;
    bool at_line_start_ = true;
};

}  // namespace pscript
