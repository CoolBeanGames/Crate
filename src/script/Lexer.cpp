#include "script/Lexer.h"

#include <cctype>
#include <unordered_map>

namespace crate::script {

const char* tokName(Tok t) {
    switch (t) {
        case Tok::End: return "end";
        case Tok::Identifier: return "identifier";
        case Tok::Int: return "int";
        case Tok::Float: return "float";
        case Tok::String: return "string";
        case Tok::Char: return "char";
        default: return "token";
    }
}

static const std::unordered_map<std::string, Tok>& keywords() {
    static const std::unordered_map<std::string, Tok> k = {
        {"class", Tok::KwClass},   {"func", Tok::KwFunc},       {"var", Tok::KwVar},
        {"return", Tok::KwReturn}, {"if", Tok::KwIf},           {"else", Tok::KwElse},
        {"switch", Tok::KwSwitch}, {"case", Tok::KwCase},       {"default", Tok::KwDefault},
        {"do", Tok::KwDo},         {"do_async", Tok::KwDoAsync}, {"true", Tok::KwTrue},
        {"false", Tok::KwFalse},   {"null", Tok::KwNull},       {"static", Tok::KwStatic},
        {"abstract", Tok::KwAbstract}, {"this", Tok::KwThis},   {"base", Tok::KwBase},
        {"break", Tok::KwBreak},   {"continue", Tok::KwContinue},
    };
    return k;
}

bool isTypeName(const std::string& s) {
    return !s.empty() && std::isupper(static_cast<unsigned char>(s[0]));
}

static bool isKnownType(const std::string& s) {
    static const char* builtins[] = {"Actor", "Actor2D", "Actor3D", "Vector2", "Vector3"};
    for (const char* b : builtins)
        if (s == b)
            return true;
    return isTypeName(s);
}

Lexer::Lexer(std::string src) : src_(std::move(src)) {}

char Lexer::peek(int o) const {
    size_t p = pos_ + o;
    return p < src_.size() ? src_[p] : '\0';
}

char Lexer::advance() {
    char c = src_[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

bool Lexer::match(char c) {
    if (peek() == c) {
        advance();
        return true;
    }
    return false;
}

void Lexer::skipTrivia() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (peek() && peek() != '\n')
                advance();
        } else if (c == '/' && peek(1) == '*') {
            advance();
            advance();
            while (peek() && !(peek() == '*' && peek(1) == '/'))
                advance();
            if (peek())
                advance();
            if (peek())
                advance();
        } else {
            return;
        }
    }
}

Token Lexer::make(Tok k, std::string text) {
    return Token{k, std::move(text), line_, col_};
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    for (;;) {
        skipTrivia();
        int startLine = line_, startCol = col_;
        char c = peek();
        if (c == '\0') {
            out.push_back({Tok::End, "", startLine, startCol});
            return out;
        }

        auto push = [&](Tok k, std::string t) {
            out.push_back({k, std::move(t), startLine, startCol});
        };

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::string id;
            while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
                id += advance();
            auto it = keywords().find(id);
            push(it != keywords().end() ? it->second : Tok::Identifier, std::move(id));
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::string num;
            bool isFloat = false;
            while (std::isdigit(static_cast<unsigned char>(peek())))
                num += advance();
            if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
                isFloat = true;
                num += advance();
                while (std::isdigit(static_cast<unsigned char>(peek())))
                    num += advance();
            }
            push(isFloat ? Tok::Float : Tok::Int, std::move(num));
            continue;
        }
        if (c == '"' || c == '\'') {
            char quote = advance();
            std::string val;
            while (peek() && peek() != quote) {
                char ch = advance();
                if (ch == '\\' && peek()) {
                    char e = advance();
                    switch (e) {
                        case 'n': val += '\n'; break;
                        case 't': val += '\t'; break;
                        case '\\': val += '\\'; break;
                        case '"': val += '"'; break;
                        case '\'': val += '\''; break;
                        default: val += e; break;
                    }
                } else {
                    val += ch;
                }
            }
            if (peek() == quote)
                advance();
            push(quote == '"' ? Tok::String : Tok::Char, std::move(val));
            continue;
        }

        advance();
        switch (c) {
            case '{': push(Tok::LBrace, "{"); break;
            case '}': push(Tok::RBrace, "}"); break;
            case '(': push(Tok::LParen, "("); break;
            case ')': push(Tok::RParen, ")"); break;
            case '[': push(Tok::LBracket, "["); break;
            case ']': push(Tok::RBracket, "]"); break;
            case ',': push(Tok::Comma, ","); break;
            case ';': push(Tok::Semicolon, ";"); break;
            case ':': push(Tok::Colon, ":"); break;
            case '.': push(Tok::Dot, "."); break;
            case '+': push(Tok::Plus, "+"); break;
            case '-': push(Tok::Minus, "-"); break;
            case '*': push(Tok::Star, "*"); break;
            case '/': push(Tok::Slash, "/"); break;
            case '%': push(Tok::Percent, "%"); break;
            case '=': push(match('=') ? Tok::EqEq : Tok::Assign, "="); break;
            case '!': push(match('=') ? Tok::NotEq : Tok::Not, "!"); break;
            case '<': push(match('=') ? Tok::LtEq : Tok::Lt, "<"); break;
            case '>': push(match('=') ? Tok::GtEq : Tok::Gt, ">"); break;
            case '&': push(match('&') ? Tok::AndAnd : Tok::Unknown, "&"); break;
            case '|': push(match('|') ? Tok::OrOr : Tok::Unknown, "|"); break;
            default: push(Tok::Unknown, std::string(1, c)); break;
        }
    }
}

std::vector<Lexer::Piece> Lexer::classifyLine(const std::string& line) {
    std::vector<Piece> pieces;
    size_t i = 0;
    while (i < line.size()) {
        char c = line[i];
        if (c == ' ' || c == '\t') {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
            pieces.push_back({Span::Comment, int(i), int(line.size() - i)});
            break;
        }
        if (c == '"' || c == '\'') {
            size_t start = i++;
            while (i < line.size() && line[i] != c) {
                if (line[i] == '\\')
                    ++i;
                ++i;
            }
            if (i < line.size())
                ++i;
            pieces.push_back({Span::String, int(start), int(i - start)});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            size_t start = i;
            while (i < line.size() &&
                   (std::isdigit(static_cast<unsigned char>(line[i])) || line[i] == '.'))
                ++i;
            pieces.push_back({Span::Number, int(start), int(i - start)});
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = i;
            while (i < line.size() &&
                   (std::isalnum(static_cast<unsigned char>(line[i])) || line[i] == '_'))
                ++i;
            std::string word = line.substr(start, i - start);
            Span s = Span::Ident;
            if (keywords().count(word))
                s = Span::Keyword;
            else if (isKnownType(word))
                s = Span::Type;
            pieces.push_back({s, int(start), int(i - start)});
            continue;
        }
        ++i;
    }
    return pieces;
}

} // namespace crate::script
