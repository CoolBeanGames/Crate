#pragma once
#include "script/Ast.h"
#include "script/Token.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace crate::script {

struct ParseError : std::runtime_error {
    int line;
    ParseError(std::string msg, int ln) : std::runtime_error(std::move(msg)), line(ln) {}
};

// Recursive-descent parser for a single cScript file (one class per file).
class Parser {
public:
    explicit Parser(std::vector<Token> tokens) : toks_(std::move(tokens)) {}

    std::unique_ptr<ClassDecl> parseClass();

private:
    std::vector<Token> toks_;
    size_t i_ = 0;

    const Token& peek(int o = 0) const;
    const Token& advance();
    bool check(Tok k) const { return peek().kind == k; }
    bool accept(Tok k);
    const Token& expect(Tok k, const char* what);
    [[noreturn]] void fail(const std::string& msg) const;

    FieldDecl parseField(const std::string& typeOrEmpty);
    FunctionDecl parseFunction(bool isAbstract);

    std::vector<StmtPtr> parseBlock();
    StmtPtr parseStmt();
    StmtPtr parseIf();
    StmtPtr parseDo(bool async);
    StmtPtr parseSwitch();

    ExprPtr parseExpr();
    ExprPtr parseAssignment();
    ExprPtr parseOr();
    ExprPtr parseAnd();
    ExprPtr parseEquality();
    ExprPtr parseComparison();
    ExprPtr parseTerm();
    ExprPtr parseFactor();
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr parsePrimary();
};

} // namespace crate::script
