#include "lexer/lexer.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using pscript::Lexer;
using pscript::LexerError;
using pscript::Token;
using pscript::TokenKind;
using pscript::token_kind_name;

namespace {

int g_failed = 0;

void fail_test(std::string_view name, std::string_view detail) {
    std::cerr << "FAIL  " << name << ": " << detail << '\n';
    ++g_failed;
}

void pass_test(std::string_view name) {
    std::cout << "PASS  " << name << '\n';
}

std::vector<Token> lex(std::string source) {
    return Lexer(std::move(source)).tokenize();
}

bool tokens_match(const std::vector<Token>& got, const std::vector<Token>& expected) {
    if (got.size() != expected.size()) {
        return false;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        if (got[i].kind != expected[i].kind || got[i].line != expected[i].line ||
            got[i].column != expected[i].column || got[i].lexeme != expected[i].lexeme) {
            return false;
        }
    }
    return true;
}

std::string dump_tokens(const std::vector<Token>& tokens) {
    std::string out;
    for (const Token& token : tokens) {
        out += token_kind_name(token.kind);
        out += '(';
        out += std::to_string(token.line);
        out += ':';
        out += std::to_string(token.column);
        out += ", \"";
        out += token.lexeme;
        out += "\") ";
    }
    return out;
}

void expect_tokens(std::string_view name, std::string source, std::vector<Token> expected) {
    try {
        const std::vector<Token> got = lex(std::move(source));
        if (!tokens_match(got, expected)) {
            fail_test(name, "got " + dump_tokens(got) + " expected " + dump_tokens(expected));
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void expect_error(std::string_view name, std::string source, std::string_view message, int line,
                  int column) {
    try {
        const std::vector<Token> got = lex(std::move(source));
        fail_test(name, "expected error, got " + dump_tokens(got));
    } catch (const LexerError& err) {
        if (err.message() != message || err.line() != line || err.column() != column) {
            fail_test(name, "got \"" + err.message() + "\" at " + std::to_string(err.line()) + ":" +
                                std::to_string(err.column()));
            return;
        }
        pass_test(name);
    } catch (const std::exception& ex) {
        fail_test(name, ex.what());
    }
}

void test_repeated_eof() {
    Lexer lexer("x");
    (void)lexer.next();
    const Token first = lexer.next();
    const Token second = lexer.next();
    if (first.kind != TokenKind::Eof || second.kind != TokenKind::Eof || first.line != 1 ||
        first.column != 2 || first.lexeme != "" || second.lexeme != "") {
        fail_test("repeated EOF", "EOF was not stable");
        return;
    }
    pass_test("repeated EOF");
}

}  // namespace

int main() {
    expect_tokens("empty source", "", {{TokenKind::Eof, 1, 1, ""}});

    expect_tokens("identifiers", "x _count player2 HTTP",
                  {{TokenKind::Identifier, 1, 1, "x"},
                   {TokenKind::Identifier, 1, 3, "_count"},
                   {TokenKind::Identifier, 1, 10, "player2"},
                   {TokenKind::Identifier, 1, 18, "HTTP"},
                   {TokenKind::Eof, 1, 22, ""}});

    expect_tokens("elseif keyword", "elseif",
                  {{TokenKind::ElseIf, 1, 1, "elseif"}, {TokenKind::Eof, 1, 7, ""}});

    expect_tokens("keywords", "const if elif else for while func return struct record enum class",
                  {{TokenKind::Const, 1, 1, "const"},
                   {TokenKind::If, 1, 7, "if"},
                   {TokenKind::Elif, 1, 10, "elif"},
                   {TokenKind::Else, 1, 15, "else"},
                   {TokenKind::For, 1, 20, "for"},
                   {TokenKind::While, 1, 24, "while"},
                   {TokenKind::Func, 1, 30, "func"},
                   {TokenKind::Return, 1, 35, "return"},
                   {TokenKind::Struct, 1, 42, "struct"},
                   {TokenKind::Record, 1, 49, "record"},
                   {TokenKind::Enum, 1, 56, "enum"},
                   {TokenKind::Class, 1, 61, "class"},
                   {TokenKind::Eof, 1, 66, ""}});

    expect_tokens("access keywords", "public pub private priv protected prot pass",
                  {{TokenKind::Public, 1, 1, "public"},
                   {TokenKind::Pub, 1, 8, "pub"},
                   {TokenKind::Private, 1, 12, "private"},
                   {TokenKind::Priv, 1, 20, "priv"},
                   {TokenKind::Protected, 1, 25, "protected"},
                   {TokenKind::Prot, 1, 35, "prot"},
                   {TokenKind::Pass, 1, 40, "pass"},
                   {TokenKind::Eof, 1, 44, ""}});

    expect_tokens("import keywords", "import from as",
                  {{TokenKind::Import, 1, 1, "import"},
                   {TokenKind::From, 1, 8, "from"},
                   {TokenKind::As, 1, 13, "as"},
                   {TokenKind::Eof, 1, 15, ""}});

    expect_tokens("logical keywords", "and or not match input",
                  {{TokenKind::And, 1, 1, "and"},
                   {TokenKind::Or, 1, 5, "or"},
                   {TokenKind::Not, 1, 8, "not"},
                   {TokenKind::Match, 1, 12, "match"},
                   {TokenKind::Input, 1, 18, "input"},
                   {TokenKind::Eof, 1, 23, ""}});

    expect_tokens("loop keywords", "in break continue",
                  {{TokenKind::In, 1, 1, "in"},
                   {TokenKind::Break, 1, 4, "break"},
                   {TokenKind::Continue, 1, 10, "continue"},
                   {TokenKind::Eof, 1, 18, ""}});

    expect_tokens("unreserved words are identifiers", "fn let print",
                  {{TokenKind::Identifier, 1, 1, "fn"},
                   {TokenKind::Identifier, 1, 4, "let"},
                   {TokenKind::Identifier, 1, 8, "print"},
                   {TokenKind::Eof, 1, 13, ""}});

    expect_tokens("true false null", "true false null",
                  {{TokenKind::True, 1, 1, "true"},
                   {TokenKind::False, 1, 6, "false"},
                   {TokenKind::Null, 1, 12, "null"},
                   {TokenKind::Eof, 1, 16, ""}});

    expect_tokens("literal case sensitivity", "True FALSE Null",
                  {{TokenKind::Identifier, 1, 1, "True"},
                   {TokenKind::Identifier, 1, 6, "FALSE"},
                   {TokenKind::Identifier, 1, 12, "Null"},
                   {TokenKind::Eof, 1, 16, ""}});

    expect_tokens("integers", "0 007 42",
                  {{TokenKind::Integer, 1, 1, "0"},
                   {TokenKind::Integer, 1, 3, "007"},
                   {TokenKind::Integer, 1, 7, "42"},
                   {TokenKind::Eof, 1, 9, ""}});

    expect_tokens("floats", "0.5 10.0 3.1415",
                  {{TokenKind::Float, 1, 1, "0.5"},
                   {TokenKind::Float, 1, 5, "10.0"},
                   {TokenKind::Float, 1, 10, "3.1415"},
                   {TokenKind::Eof, 1, 16, ""}});

    expect_tokens("duration literals", "500ms 1.5s 2m 1h",
                  {{TokenKind::Duration, 1, 1, "500ms"},
                   {TokenKind::Duration, 1, 7, "1.5s"},
                   {TokenKind::Duration, 1, 12, "2m"},
                   {TokenKind::Duration, 1, 15, "1h"},
                   {TokenKind::Eof, 1, 17, ""}});

    expect_tokens("strings", "\"hello\" \"\"",
                  {{TokenKind::String, 1, 1, "\"hello\""},
                   {TokenKind::String, 1, 9, "\"\""},
                   {TokenKind::Eof, 1, 11, ""}});

    expect_tokens("string escapes", "\"\\\"\\\\\\n\\t\\r\"",
                  {{TokenKind::String, 1, 1, "\"\\\"\\\\\\n\\t\\r\""},
                   {TokenKind::Eof, 1, 13, ""}});

    expect_tokens("hash inside string is not a comment", "\"a#b\"",
                  {{TokenKind::String, 1, 1, "\"a#b\""}, {TokenKind::Eof, 1, 6, ""}});

    expect_tokens("comments", "a# comment\nb",
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::Identifier, 2, 1, "b"},
                   {TokenKind::Eof, 2, 2, ""}});

    expect_tokens("comment to eof", "# only comment", {{TokenKind::Eof, 1, 15, ""}});

    expect_tokens("whitespace", "   a", {{TokenKind::Identifier, 1, 4, "a"}, {TokenKind::Eof, 1, 5, ""}});
    expect_tokens("mid-line tab trivia", "a\tb",
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::Identifier, 1, 3, "b"},
                   {TokenKind::Eof, 1, 4, ""}});

    expect_tokens("lf newline tracking", "a\nb",
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::Identifier, 2, 1, "b"},
                   {TokenKind::Eof, 2, 2, ""}});

    expect_tokens("crlf newline tracking", std::string("a\r\nb"),
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::Identifier, 2, 1, "b"},
                   {TokenKind::Eof, 2, 2, ""}});

    expect_tokens("bare cr newline tracking", std::string("a\rb"),
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::Identifier, 2, 1, "b"},
                   {TokenKind::Eof, 2, 2, ""}});

    expect_tokens("utf-8 string columns", "\"\xC3\xA9\"",
                  {{TokenKind::String, 1, 1, "\"\xC3\xA9\""}, {TokenKind::Eof, 1, 4, ""}});

    expect_tokens("utf-8 bom skipped", "\xEF\xBB\xBFprint",
                  {{TokenKind::Identifier, 1, 1, "print"}, {TokenKind::Eof, 1, 6, ""}});

    expect_tokens("utf-8 turkish string lexeme", "\"\xC3\xA7\xC4\x9F\xC4\xB1\"",
                  {{TokenKind::String, 1, 1, "\"\xC3\xA7\xC4\x9F\xC4\xB1\""}, {TokenKind::Eof, 1, 6, ""}});

    expect_tokens("single-character operators", "a = b < c > d",
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::Equal, 1, 3, "="},
                   {TokenKind::Identifier, 1, 5, "b"},
                   {TokenKind::Less, 1, 7, "<"},
                   {TokenKind::Identifier, 1, 9, "c"},
                   {TokenKind::Greater, 1, 11, ">"},
                   {TokenKind::Identifier, 1, 13, "d"},
                   {TokenKind::Eof, 1, 14, ""}});

    expect_tokens("multi-character operators", "a==b!=c<=d>=e",
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::EqualEqual, 1, 2, "=="},
                   {TokenKind::Identifier, 1, 4, "b"},
                   {TokenKind::BangEqual, 1, 5, "!="},
                   {TokenKind::Identifier, 1, 7, "c"},
                   {TokenKind::LessEqual, 1, 8, "<="},
                   {TokenKind::Identifier, 1, 10, "d"},
                   {TokenKind::GreaterEqual, 1, 11, ">="},
                   {TokenKind::Identifier, 1, 13, "e"},
                   {TokenKind::Eof, 1, 14, ""}});

    expect_tokens("longest match ===", "===",
                  {{TokenKind::EqualEqual, 1, 1, "=="},
                   {TokenKind::Equal, 1, 3, "="},
                   {TokenKind::Eof, 1, 4, ""}});

    expect_tokens("longest match !==", "!==",
                  {{TokenKind::BangEqual, 1, 1, "!="},
                   {TokenKind::Equal, 1, 3, "="},
                   {TokenKind::Eof, 1, 4, ""}});

    test_repeated_eof();

    expect_tokens("arithmetic operators", "a + b - c * d / e % f",
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::Plus, 1, 3, "+"},
                   {TokenKind::Identifier, 1, 5, "b"},
                   {TokenKind::Minus, 1, 7, "-"},
                   {TokenKind::Identifier, 1, 9, "c"},
                   {TokenKind::Star, 1, 11, "*"},
                   {TokenKind::Identifier, 1, 13, "d"},
                   {TokenKind::Slash, 1, 15, "/"},
                   {TokenKind::Identifier, 1, 17, "e"},
                   {TokenKind::Percent, 1, 19, "%"},
                   {TokenKind::Identifier, 1, 21, "f"},
                   {TokenKind::Eof, 1, 22, ""}});

    expect_tokens("compound assignment tokens", "+= -= *= /= %=",
                  {{TokenKind::PlusEqual, 1, 1, "+="},
                   {TokenKind::MinusEqual, 1, 4, "-="},
                   {TokenKind::StarEqual, 1, 7, "*="},
                   {TokenKind::SlashEqual, 1, 10, "/="},
                   {TokenKind::PercentEqual, 1, 13, "%="},
                   {TokenKind::Eof, 1, 15, ""}});

    expect_tokens("increment decrement tokens", "++ --",
                  {{TokenKind::PlusPlus, 1, 1, "++"},
                   {TokenKind::MinusMinus, 1, 4, "--"},
                   {TokenKind::Eof, 1, 6, ""}});

    expect_tokens("bitwise tokens", "& | ^ ~ << >>",
                  {{TokenKind::Amp, 1, 1, "&"},
                   {TokenKind::Pipe, 1, 3, "|"},
                   {TokenKind::Caret, 1, 5, "^"},
                   {TokenKind::Tilde, 1, 7, "~"},
                   {TokenKind::LessLess, 1, 9, "<<"},
                   {TokenKind::GreaterGreater, 1, 12, ">>"},
                   {TokenKind::Eof, 1, 14, ""}});

    expect_tokens("question colon", "a ? b : c",
                  {{TokenKind::Identifier, 1, 1, "a"},
                   {TokenKind::Question, 1, 3, "?"},
                   {TokenKind::Identifier, 1, 5, "b"},
                   {TokenKind::Colon, 1, 7, ":"},
                   {TokenKind::Identifier, 1, 9, "c"},
                   {TokenKind::Eof, 1, 10, ""}});

    expect_tokens("in keyword", "x in y",
                  {{TokenKind::Identifier, 1, 1, "x"},
                   {TokenKind::In, 1, 3, "in"},
                   {TokenKind::Identifier, 1, 6, "y"},
                   {TokenKind::Eof, 1, 7, ""}});

    expect_tokens("not in tokens", "not in",
                  {{TokenKind::Not, 1, 1, "not"},
                   {TokenKind::In, 1, 5, "in"},
                   {TokenKind::Eof, 1, 7, ""}});

    expect_tokens("punctuation", "(){}[],:.",
                  {{TokenKind::LeftParen, 1, 1, "("},
                   {TokenKind::RightParen, 1, 2, ")"},
                   {TokenKind::LeftBrace, 1, 3, "{"},
                   {TokenKind::RightBrace, 1, 4, "}"},
                   {TokenKind::LeftBracket, 1, 5, "["},
                   {TokenKind::RightBracket, 1, 6, "]"},
                   {TokenKind::Comma, 1, 7, ","},
                   {TokenKind::Colon, 1, 8, ":"},
                   {TokenKind::Dot, 1, 9, "."},
                   {TokenKind::Eof, 1, 10, ""}});

    expect_tokens("member access", "obj.field",
                  {{TokenKind::Identifier, 1, 1, "obj"},
                   {TokenKind::Dot, 1, 4, "."},
                   {TokenKind::Identifier, 1, 5, "field"},
                   {TokenKind::Eof, 1, 10, ""}});

    expect_tokens("print with parentheses", "print(\"Hello\")",
                  {{TokenKind::Identifier, 1, 1, "print"},
                   {TokenKind::LeftParen, 1, 6, "("},
                   {TokenKind::String, 1, 7, "\"Hello\""},
                   {TokenKind::RightParen, 1, 14, ")"},
                   {TokenKind::Eof, 1, 15, ""}});

    expect_tokens("print without parentheses", "print \"Hello\"",
                  {{TokenKind::Identifier, 1, 1, "print"},
                   {TokenKind::String, 1, 7, "\"Hello\""},
                   {TokenKind::Eof, 1, 14, ""}});

    expect_tokens("at sign", "@", {{TokenKind::At, 1, 1, "@"}, {TokenKind::Eof, 1, 2, ""}});
    expect_tokens("arrow", "->", {{TokenKind::MinusGreater, 1, 1, "->"}, {TokenKind::Eof, 1, 3, ""}});
    expect_tokens("ellipsis", "...", {{TokenKind::Ellipsis, 1, 1, "..."}, {TokenKind::Eof, 1, 4, ""}});
    expect_error("invalid character semicolon", ";", "invalid character", 1, 1);
    expect_error("invalid character apostrophe", "'", "invalid character", 1, 1);
    expect_tokens("bang and not-equal", "! !=",
                  {{TokenKind::Bang, 1, 1, "!"},
                   {TokenKind::BangEqual, 1, 3, "!="},
                   {TokenKind::Eof, 1, 5, ""}});

    expect_tokens("amp vs ampamp", "& &&",
                  {{TokenKind::Amp, 1, 1, "&"},
                   {TokenKind::AmpAmp, 1, 3, "&&"},
                   {TokenKind::Eof, 1, 5, ""}});
    expect_tokens("pipe vs pipepipe", "| ||",
                  {{TokenKind::Pipe, 1, 1, "|"},
                   {TokenKind::PipePipe, 1, 3, "||"},
                   {TokenKind::Eof, 1, 5, ""}});
    expect_tokens("shift vs compare", "<< <= >> >=",
                  {{TokenKind::LessLess, 1, 1, "<<"},
                   {TokenKind::LessEqual, 1, 4, "<="},
                   {TokenKind::GreaterGreater, 1, 7, ">>"},
                   {TokenKind::GreaterEqual, 1, 10, ">="},
                   {TokenKind::Eof, 1, 12, ""}});
    expect_tokens("plus plus vs plus equal", "++ += +",
                  {{TokenKind::PlusPlus, 1, 1, "++"},
                   {TokenKind::PlusEqual, 1, 4, "+="},
                   {TokenKind::Plus, 1, 7, "+"},
                   {TokenKind::Eof, 1, 8, ""}});

    expect_tokens("underscore identifier", "_ value",
                  {{TokenKind::Identifier, 1, 1, "_"},
                   {TokenKind::Identifier, 1, 3, "value"},
                   {TokenKind::Eof, 1, 8, ""}});
    expect_error("invalid escape", "\"\\q\"", "invalid escape", 1, 2);
    expect_error("invalid escape at eof", "\"\\", "invalid escape", 1, 2);
    expect_error("invalid escape before newline", std::string("\"\\\n\""), "invalid escape", 1, 2);
    expect_error("unterminated string eof", "\"abc", "unterminated string", 1, 1);
    expect_error("unterminated string newline", std::string("\"abc\n\""), "unterminated string", 1, 1);
    expect_error("unterminated string crlf", std::string("\"abc\r\n\""), "unterminated string", 1, 1);
    expect_error("invalid numeric 1.", "1.", "invalid numeric form", 1, 1);
    expect_error("invalid numeric .5", ".5", "invalid numeric form", 1, 1);
    expect_tokens("lone dot is punctuation", ".",
                  {{TokenKind::Dot, 1, 1, "."}, {TokenKind::Eof, 1, 2, ""}});
    expect_error("invalid numeric 1.2.3", "1.2.3", "invalid numeric form", 1, 1);
    expect_error("invalid numeric exponent", "1e3", "invalid numeric form", 1, 1);
    expect_error("invalid numeric 1x", "1x", "invalid numeric form", 1, 1);
    expect_error("invalid numeric hex", "0x10", "invalid numeric form", 1, 1);
    expect_error("invalid numeric 1.2f", "1.2f", "invalid numeric form", 1, 1);
    expect_error("leading tab indent", "if true:\n\tprint(1)", "invalid indent", 2, 1);

    expect_tokens("assignment and comparison locations", "x==1",
                  {{TokenKind::Identifier, 1, 1, "x"},
                   {TokenKind::EqualEqual, 1, 2, "=="},
                   {TokenKind::Integer, 1, 4, "1"},
                   {TokenKind::Eof, 1, 5, ""}});

    if (g_failed != 0) {
        std::cerr << g_failed << " test(s) failed\n";
        return 1;
    }
    std::cout << "All lexer tests passed\n";
    return 0;
}
