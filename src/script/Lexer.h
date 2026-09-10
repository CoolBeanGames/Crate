#pragma once
#include "script/Token.h"
#include <string>
#include <vector>

namespace crate::script {

// Tokenizes cScript source. Also used by the editor for syntax highlighting,
// so it records comments as it goes (via classify()).
class Lexer {
public:
    explicit Lexer(std::string src);

    std::vector<Token> tokenize(); // stops at End; throws nothing (Unknown token on error)

    // Lightweight span classification for the editor's highlighter.
    enum class Span { Plain, Keyword, Type, Number, String, Comment, Ident };
    struct Piece {
        Span span;
        int start; // byte offset
        int length;
    };
    static std::vector<Piece> classifyLine(const std::string& line);

private:
    std::string src_;
    size_t pos_ = 0;
    int line_ = 1, col_ = 1;

    char peek(int o = 0) const;
    char advance();
    bool match(char c);
    void skipTrivia();
    Token make(Tok k, std::string text);
};

bool isTypeName(const std::string& s); // uppercase first letter => a type

} // namespace crate::script
