#pragma once
#include <string>

namespace crate::script {

enum class Tok {
    End,
    // literals / names
    Identifier, Int, Float, String, Char,
    // punctuation
    LBrace, RBrace, LParen, RParen, LBracket, RBracket,
    Comma, Semicolon, Colon, Dot,
    // operators
    Plus, Minus, Star, Slash, Percent,
    PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    Assign, EqEq, NotEq, Lt, Gt, LtEq, GtEq,
    AndAnd, OrOr, Not,
    // keywords
    KwClass, KwFunc, KwVar, KwReturn, KwIf, KwElse, KwSwitch, KwCase, KwDefault,
    KwDo, KwDoAsync, KwTrue, KwFalse, KwNull, KwStatic, KwAbstract, KwThis, KwBase,
    KwBreak, KwContinue, KwSignal,
    Unknown,
};

struct Token {
    Tok kind = Tok::End;
    std::string text; // lexeme / decoded string
    int line = 1;
    int col = 1;
};

const char* tokName(Tok t);

} // namespace crate::script
