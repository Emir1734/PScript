#pragma once

#include <string>

namespace pscript {

enum class TokenKind {
    Identifier,
    True,
    False,
    Null,
    Const,
    If,
    Elif,
    ElseIf,
    Else,
    For,
    With,
    While,
    In,
    Break,
    Continue,
    Func,
    Async,
    Await,
    Return,
    Struct,
    Record,
    Enum,
    Class,
    Constructor,
    This,
    Super,
    Parent,
    Public,
    Pub,
    Private,
    Priv,
    Protected,
    Prot,
    Pass,
    Import,
    From,
    As,
    Match,
    Defer,
    Try,
    Catch,
    Finally,
    Throw,
    Input,
    And,
    Or,
    Not,
    Integer,
    Float,
    Duration,
    String,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Equal,
    EqualEqual,
    BangEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    AmpAmp,
    PipePipe,
    Bang,
    PlusPlus,
    MinusMinus,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    PercentEqual,
    Amp,
    Pipe,
    PipeGreater,
    Caret,
    Tilde,
    LessLess,
    GreaterGreater,
    MinusGreater,
    At,
    Ellipsis,
    Interface,
    Protocol,
    TypeAlias,
    NullCoalesce,
    Question,
    LeftParen,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Comma,
    Colon,
    Dot,
    Eof,
};

struct Token {
    TokenKind kind = TokenKind::Eof;
    int line = 1;
    int column = 1;
    std::string lexeme;
};

inline const char* token_kind_name(TokenKind kind) {
    switch (kind) {
        case TokenKind::Identifier:
            return "Identifier";
        case TokenKind::True:
            return "True";
        case TokenKind::False:
            return "False";
        case TokenKind::Null:
            return "Null";
        case TokenKind::Const:
            return "Const";
        case TokenKind::If:
            return "If";
        case TokenKind::Elif:
            return "Elif";
        case TokenKind::ElseIf:
            return "ElseIf";
        case TokenKind::Else:
            return "Else";
        case TokenKind::For:
            return "For";
        case TokenKind::With:
            return "With";
        case TokenKind::While:
            return "While";
        case TokenKind::In:
            return "In";
        case TokenKind::Break:
            return "Break";
        case TokenKind::Continue:
            return "Continue";
        case TokenKind::Func:
            return "Func";
        case TokenKind::Async:
            return "Async";
        case TokenKind::Await:
            return "Await";
        case TokenKind::Return:
            return "Return";
        case TokenKind::Struct:
            return "Struct";
        case TokenKind::Record:
            return "Record";
        case TokenKind::Enum:
            return "Enum";
        case TokenKind::Class:
            return "Class";
        case TokenKind::Constructor:
            return "Constructor";
        case TokenKind::This:
            return "This";
        case TokenKind::Super:
            return "Super";
        case TokenKind::Parent:
            return "Parent";
        case TokenKind::Public:
            return "Public";
        case TokenKind::Pub:
            return "Pub";
        case TokenKind::Private:
            return "Private";
        case TokenKind::Priv:
            return "Priv";
        case TokenKind::Protected:
            return "Protected";
        case TokenKind::Prot:
            return "Prot";
        case TokenKind::Pass:
            return "Pass";
        case TokenKind::Import:
            return "Import";
        case TokenKind::From:
            return "From";
        case TokenKind::As:
            return "As";
        case TokenKind::Match:
            return "Match";
        case TokenKind::Try:
            return "Try";
        case TokenKind::Catch:
            return "Catch";
        case TokenKind::Finally:
            return "Finally";
        case TokenKind::Throw:
            return "Throw";
        case TokenKind::Input:
            return "Input";
        case TokenKind::And:
            return "And";
        case TokenKind::Or:
            return "Or";
        case TokenKind::Not:
            return "Not";
        case TokenKind::Integer:
            return "Integer";
        case TokenKind::Float:
            return "Float";
        case TokenKind::Duration:
            return "Duration";
        case TokenKind::String:
            return "String";
        case TokenKind::Plus:
            return "Plus";
        case TokenKind::Minus:
            return "Minus";
        case TokenKind::Star:
            return "Star";
        case TokenKind::Slash:
            return "Slash";
        case TokenKind::Percent:
            return "Percent";
        case TokenKind::Equal:
            return "Equal";
        case TokenKind::EqualEqual:
            return "EqualEqual";
        case TokenKind::BangEqual:
            return "BangEqual";
        case TokenKind::Less:
            return "Less";
        case TokenKind::LessEqual:
            return "LessEqual";
        case TokenKind::Greater:
            return "Greater";
        case TokenKind::GreaterEqual:
            return "GreaterEqual";
        case TokenKind::AmpAmp:
            return "AmpAmp";
        case TokenKind::PipePipe:
            return "PipePipe";
        case TokenKind::Bang:
            return "Bang";
        case TokenKind::PlusPlus:
            return "PlusPlus";
        case TokenKind::MinusMinus:
            return "MinusMinus";
        case TokenKind::PlusEqual:
            return "PlusEqual";
        case TokenKind::MinusEqual:
            return "MinusEqual";
        case TokenKind::StarEqual:
            return "StarEqual";
        case TokenKind::SlashEqual:
            return "SlashEqual";
        case TokenKind::PercentEqual:
            return "PercentEqual";
        case TokenKind::Amp:
            return "Amp";
        case TokenKind::Pipe:
            return "Pipe";
        case TokenKind::PipeGreater:
            return "PipeGreater";
        case TokenKind::Caret:
            return "Caret";
        case TokenKind::Tilde:
            return "Tilde";
        case TokenKind::LessLess:
            return "LessLess";
        case TokenKind::GreaterGreater:
            return "GreaterGreater";
        case TokenKind::MinusGreater:
            return "MinusGreater";
        case TokenKind::At:
            return "At";
        case TokenKind::Ellipsis:
            return "Ellipsis";
        case TokenKind::Interface:
            return "Interface";
        case TokenKind::Protocol:
            return "Protocol";
        case TokenKind::TypeAlias:
            return "TypeAlias";
        case TokenKind::NullCoalesce:
            return "NullCoalesce";
        case TokenKind::Question:
            return "Question";
        case TokenKind::LeftParen:
            return "LeftParen";
        case TokenKind::RightParen:
            return "RightParen";
        case TokenKind::LeftBrace:
            return "LeftBrace";
        case TokenKind::RightBrace:
            return "RightBrace";
        case TokenKind::LeftBracket:
            return "LeftBracket";
        case TokenKind::RightBracket:
            return "RightBracket";
        case TokenKind::Comma:
            return "Comma";
        case TokenKind::Colon:
            return "Colon";
        case TokenKind::Dot:
            return "Dot";
        case TokenKind::Eof:
            return "Eof";
    }
    return "Unknown";
}

}  // namespace pscript
