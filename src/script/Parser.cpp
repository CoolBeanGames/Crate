#include "script/Parser.h"

namespace crate::script {

const Token& Parser::peek(int o) const {
    size_t p = i_ + o;
    return p < toks_.size() ? toks_[p] : toks_.back();
}
const Token& Parser::advance() {
    const Token& t = toks_[i_];
    if (i_ + 1 < toks_.size())
        ++i_;
    return t;
}
bool Parser::accept(Tok k) {
    if (check(k)) {
        advance();
        return true;
    }
    return false;
}
const Token& Parser::expect(Tok k, const char* what) {
    if (!check(k))
        fail(std::string("expected ") + what + " but got '" + peek().text + "'");
    return advance();
}
void Parser::fail(const std::string& msg) const { throw ParseError(msg, peek().line); }

static ExprPtr mk(ExprKind k, int line) {
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->line = line;
    return e;
}
static StmtPtr mks(StmtKind k, int line) {
    auto s = std::make_unique<Stmt>();
    s->kind = k;
    s->line = line;
    return s;
}

std::unique_ptr<ClassDecl> Parser::parseClass() {
    auto cls = std::make_unique<ClassDecl>();
    while (check(Tok::KwStatic) || check(Tok::KwAbstract)) {
        if (accept(Tok::KwStatic))
            cls->isStatic = true;
        else if (accept(Tok::KwAbstract))
            cls->isAbstract = true;
    }
    expect(Tok::KwClass, "'class'");
    cls->name = expect(Tok::Identifier, "class name").text;
    if (accept(Tok::Colon))
        cls->base = expect(Tok::Identifier, "base type name").text;
    expect(Tok::LBrace, "'{'");

    while (!check(Tok::RBrace) && !check(Tok::End)) {
        bool isAbstract = accept(Tok::KwAbstract);
        if (check(Tok::KwSignal)) {
            advance();
            SignalDecl sig;
            sig.name = expect(Tok::Identifier, "signal name").text;
            if (accept(Tok::LParen)) {
                if (!check(Tok::RParen)) {
                    do {
                        // "type name" or just "name"
                        std::string first = expect(Tok::Identifier, "signal parameter").text;
                        sig.params.push_back(check(Tok::Identifier) ? advance().text : first);
                    } while (accept(Tok::Comma));
                }
                expect(Tok::RParen, "')'");
            }
            accept(Tok::Semicolon);
            cls->signals.push_back(std::move(sig));
        } else if (check(Tok::KwFunc)) {
            cls->functions.push_back(parseFunction(isAbstract));
        } else if (accept(Tok::KwVar)) {
            cls->fields.push_back(parseField(""));
        } else if (check(Tok::Identifier)) {
            // typed field:  Type name = init;
            std::string ty = advance().text;
            cls->fields.push_back(parseField(ty));
        } else {
            fail("expected a field or function declaration");
        }
    }
    expect(Tok::RBrace, "'}'");
    return cls;
}

FieldDecl Parser::parseField(const std::string& typeOrEmpty) {
    FieldDecl f;
    f.type = typeOrEmpty;
    f.name = expect(Tok::Identifier, "field name").text;
    if (accept(Tok::Assign))
        f.init = parseExpr();
    expect(Tok::Semicolon, "';'");
    return f;
}

FunctionDecl Parser::parseFunction(bool isAbstract) {
    FunctionDecl fn;
    fn.isAbstract = isAbstract;
    fn.line = peek().line;
    expect(Tok::KwFunc, "'func'");
    fn.name = expect(Tok::Identifier, "function name").text;
    expect(Tok::LParen, "'('");
    if (!check(Tok::RParen)) {
        do {
            Param p;
            // "type name" or just "name"
            std::string first = expect(Tok::Identifier, "parameter").text;
            if (check(Tok::Identifier)) {
                p.type = first;
                p.name = advance().text;
            } else {
                p.name = first;
            }
            fn.params.push_back(std::move(p));
        } while (accept(Tok::Comma));
    }
    expect(Tok::RParen, "')'");
    if (accept(Tok::Colon))
        fn.returnType = expect(Tok::Identifier, "return type").text;

    if (isAbstract) {
        accept(Tok::Semicolon);
        if (check(Tok::LBrace)) // tolerate an empty body
            parseBlock();
        return fn;
    }
    fn.body = parseBlock();
    return fn;
}

std::vector<StmtPtr> Parser::parseBlock() {
    expect(Tok::LBrace, "'{'");
    std::vector<StmtPtr> body;
    while (!check(Tok::RBrace) && !check(Tok::End))
        body.push_back(parseStmt());
    expect(Tok::RBrace, "'}'");
    return body;
}

StmtPtr Parser::parseStmt() {
    switch (peek().kind) {
        case Tok::LBrace: {
            auto s = mks(StmtKind::Block, peek().line);
            s->body = parseBlock();
            return s;
        }
        case Tok::KwVar: {
            advance();
            auto s = mks(StmtKind::VarDecl, peek().line);
            s->name = expect(Tok::Identifier, "variable name").text;
            if (accept(Tok::Assign))
                s->init = parseExpr();
            expect(Tok::Semicolon, "';'");
            return s;
        }
        case Tok::KwReturn: {
            advance();
            auto s = mks(StmtKind::Return, peek().line);
            if (!check(Tok::Semicolon))
                s->expr = parseExpr();
            expect(Tok::Semicolon, "';'");
            return s;
        }
        case Tok::KwIf: return parseIf();
        case Tok::KwDo: return parseDo(false);
        case Tok::KwDoAsync: return parseDo(true);
        case Tok::KwSwitch: return parseSwitch();
        case Tok::KwBreak: {
            advance();
            expect(Tok::Semicolon, "';'");
            return mks(StmtKind::Break, peek().line);
        }
        case Tok::KwContinue: {
            advance();
            expect(Tok::Semicolon, "';'");
            return mks(StmtKind::Continue, peek().line);
        }
        default: break;
    }

    // Declaration "Type name ..." vs expression statement.
    if (check(Tok::Identifier) && peek(1).kind == Tok::Identifier) {
        auto s = mks(StmtKind::VarDecl, peek().line);
        s->declType = advance().text;
        s->name = advance().text;
        if (accept(Tok::Assign))
            s->init = parseExpr();
        expect(Tok::Semicolon, "';'");
        return s;
    }

    auto s = mks(StmtKind::ExprStmt, peek().line);
    s->expr = parseExpr();
    // cScript is permissive about the trailing ';' on an expression statement
    // (the sample script omits it after print()).
    accept(Tok::Semicolon);
    return s;
}

StmtPtr Parser::parseIf() {
    auto s = mks(StmtKind::If, peek().line);
    expect(Tok::KwIf, "'if'");
    expect(Tok::LParen, "'('");
    s->cond = parseExpr();
    expect(Tok::RParen, "')'");
    s->thenBody = parseBlock();
    if (accept(Tok::KwElse)) {
        if (check(Tok::KwIf)) {
            s->elseBody.push_back(parseIf());
        } else {
            s->elseBody = parseBlock();
        }
    }
    return s;
}

StmtPtr Parser::parseDo(bool async) {
    auto s = mks(async ? StmtKind::DoAsync : StmtKind::DoWhile, peek().line);
    advance(); // do / do_async
    expect(Tok::LParen, "'('");
    s->cond = parseExpr();
    expect(Tok::RParen, "')'");
    s->body = parseBlock();
    return s;
}

StmtPtr Parser::parseSwitch() {
    auto s = mks(StmtKind::Switch, peek().line);
    expect(Tok::KwSwitch, "'switch'");
    expect(Tok::LParen, "'('");
    s->subject = parseExpr();
    expect(Tok::RParen, "')'");
    expect(Tok::LBrace, "'{'");
    while (!check(Tok::RBrace) && !check(Tok::End)) {
        SwitchCase c;
        if (accept(Tok::KwCase)) {
            c.value = parseExpr();
        } else {
            expect(Tok::KwDefault, "'case' or 'default'");
        }
        expect(Tok::Colon, "':'");
        if (check(Tok::LBrace)) {
            c.body = parseBlock();
        } else {
            while (!check(Tok::KwCase) && !check(Tok::KwDefault) && !check(Tok::RBrace) &&
                   !check(Tok::End))
                c.body.push_back(parseStmt());
        }
        s->cases.push_back(std::move(c));
    }
    expect(Tok::RBrace, "'}'");
    return s;
}

// ---- expression grammar (precedence climbing) --------------------------
ExprPtr Parser::parseExpr() { return parseAssignment(); }

ExprPtr Parser::parseAssignment() {
    ExprPtr lhs = parseOr();

    // Plain '=' or a compound assignment (+=, -=, *=, /=, %=). For the compound
    // forms we record the underlying arithmetic op on the Assign node; the
    // interpreter treats `a op= b` as `a = a op b`.
    Tok compound = Tok::Unknown;
    switch (peek().kind) {
        case Tok::PlusEq: compound = Tok::Plus; break;
        case Tok::MinusEq: compound = Tok::Minus; break;
        case Tok::StarEq: compound = Tok::Star; break;
        case Tok::SlashEq: compound = Tok::Slash; break;
        case Tok::PercentEq: compound = Tok::Percent; break;
        default: break;
    }
    if (check(Tok::Assign) || compound != Tok::Unknown) {
        int ln = advance().line;
        ExprPtr rhs = parseAssignment();
        auto e = mk(ExprKind::Assign, ln);
        e->op = compound; // Tok::Unknown for a plain '='
        e->a = std::move(lhs);
        e->b = std::move(rhs);
        return e;
    }
    return lhs;
}

static ExprPtr binary(Tok op, ExprPtr a, ExprPtr b, int ln) {
    auto e = mk(ExprKind::Binary, ln);
    e->op = op;
    e->a = std::move(a);
    e->b = std::move(b);
    return e;
}

ExprPtr Parser::parseOr() {
    ExprPtr a = parseAnd();
    while (check(Tok::OrOr)) {
        int ln = advance().line;
        a = binary(Tok::OrOr, std::move(a), parseAnd(), ln);
    }
    return a;
}
ExprPtr Parser::parseAnd() {
    ExprPtr a = parseEquality();
    while (check(Tok::AndAnd)) {
        int ln = advance().line;
        a = binary(Tok::AndAnd, std::move(a), parseEquality(), ln);
    }
    return a;
}
ExprPtr Parser::parseEquality() {
    ExprPtr a = parseComparison();
    while (check(Tok::EqEq) || check(Tok::NotEq)) {
        Tok op = peek().kind;
        int ln = advance().line;
        a = binary(op, std::move(a), parseComparison(), ln);
    }
    return a;
}
ExprPtr Parser::parseComparison() {
    ExprPtr a = parseTerm();
    while (check(Tok::Lt) || check(Tok::Gt) || check(Tok::LtEq) || check(Tok::GtEq)) {
        Tok op = peek().kind;
        int ln = advance().line;
        a = binary(op, std::move(a), parseTerm(), ln);
    }
    return a;
}
ExprPtr Parser::parseTerm() {
    ExprPtr a = parseFactor();
    while (check(Tok::Plus) || check(Tok::Minus)) {
        Tok op = peek().kind;
        int ln = advance().line;
        a = binary(op, std::move(a), parseFactor(), ln);
    }
    return a;
}
ExprPtr Parser::parseFactor() {
    ExprPtr a = parseUnary();
    while (check(Tok::Star) || check(Tok::Slash) || check(Tok::Percent)) {
        Tok op = peek().kind;
        int ln = advance().line;
        a = binary(op, std::move(a), parseUnary(), ln);
    }
    return a;
}
ExprPtr Parser::parseUnary() {
    if (check(Tok::Minus) || check(Tok::Not) || check(Tok::Plus)) {
        Tok op = peek().kind;
        int ln = advance().line;
        auto e = mk(ExprKind::Unary, ln);
        e->op = op;
        e->a = parseUnary();
        return e;
    }
    return parsePostfix();
}
ExprPtr Parser::parsePostfix() {
    ExprPtr e = parsePrimary();
    for (;;) {
        if (accept(Tok::Dot)) {
            auto m = mk(ExprKind::Member, peek().line);
            m->a = std::move(e);
            // `base` is a keyword but is also a valid member name (this.base).
            if (check(Tok::KwBase)) {
                advance();
                m->strVal = "base";
            } else {
                m->strVal = expect(Tok::Identifier, "member name").text;
            }
            e = std::move(m);
        } else if (check(Tok::LParen)) {
            int ln = advance().line;
            auto call = mk(ExprKind::Call, ln);
            call->a = std::move(e);
            if (!check(Tok::RParen)) {
                do {
                    call->items.push_back(parseExpr());
                } while (accept(Tok::Comma));
            }
            expect(Tok::RParen, "')'");
            e = std::move(call);
        } else if (accept(Tok::LBracket)) {
            auto idx = mk(ExprKind::Index, peek().line);
            idx->a = std::move(e);
            idx->b = parseExpr();
            expect(Tok::RBracket, "']'");
            e = std::move(idx);
        } else {
            return e;
        }
    }
}
ExprPtr Parser::parsePrimary() {
    const Token& t = peek();
    switch (t.kind) {
        case Tok::Int: {
            advance();
            auto e = mk(ExprKind::IntLit, t.line);
            e->intVal = std::stoll(t.text);
            return e;
        }
        case Tok::Float: {
            advance();
            auto e = mk(ExprKind::FloatLit, t.line);
            e->floatVal = std::stod(t.text);
            return e;
        }
        case Tok::String: {
            advance();
            auto e = mk(ExprKind::StringLit, t.line);
            e->strVal = t.text;
            return e;
        }
        case Tok::Char: {
            advance();
            auto e = mk(ExprKind::CharLit, t.line);
            e->strVal = t.text;
            return e;
        }
        case Tok::KwTrue:
        case Tok::KwFalse: {
            advance();
            auto e = mk(ExprKind::BoolLit, t.line);
            e->boolVal = (t.kind == Tok::KwTrue);
            return e;
        }
        case Tok::KwNull:
            advance();
            return mk(ExprKind::NullLit, t.line);
        case Tok::KwThis:
            advance();
            return mk(ExprKind::This, t.line);
        case Tok::KwBase:
            advance();
            return mk(ExprKind::Base, t.line);
        case Tok::Identifier: {
            advance();
            auto e = mk(ExprKind::Identifier, t.line);
            e->strVal = t.text;
            return e;
        }
        case Tok::LParen: {
            advance();
            ExprPtr inner = parseExpr();
            expect(Tok::RParen, "')'");
            return inner;
        }
        case Tok::LBracket: {
            advance();
            auto e = mk(ExprKind::ArrayLit, t.line);
            if (!check(Tok::RBracket)) {
                do {
                    e->items.push_back(parseExpr());
                } while (accept(Tok::Comma));
            }
            expect(Tok::RBracket, "']'");
            return e;
        }
        default:
            fail("unexpected token '" + t.text + "'");
    }
}

} // namespace crate::script
