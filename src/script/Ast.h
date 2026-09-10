#pragma once
#include "script/Token.h"

#include <memory>
#include <string>
#include <vector>

namespace crate::script {

struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

// ---- Expressions ---------------------------------------------------------
enum class ExprKind {
    IntLit, FloatLit, StringLit, CharLit, BoolLit, NullLit,
    Identifier,   // name
    ArrayLit,     // [a, b, c]  -> items
    Unary,        // op a
    Binary,       // a op b
    Assign,       // target = value  (target: Identifier / Member / Index)
    Call,         // callee(args)
    Member,       // object.name
    Index,        // object[index]
    This,         // this
    Base,         // base (used as this.base.f())
};

struct Expr {
    ExprKind kind;
    int line = 0;

    // literals
    long long intVal = 0;
    double floatVal = 0.0;
    std::string strVal;   // StringLit / CharLit / Identifier / Member name
    bool boolVal = false;

    Tok op = Tok::Unknown; // Unary / Binary

    ExprPtr a, b;                 // operands / object / target / callee / value
    std::vector<ExprPtr> items;   // ArrayLit items / Call args
};

// ---- Statements ---------------------------------------------------------
enum class StmtKind {
    Block, VarDecl, ExprStmt, Return, If, DoWhile, DoAsync, Switch, Break, Continue,
};

struct SwitchCase {
    ExprPtr value;   // null => default
    std::vector<StmtPtr> body;
};

struct Stmt {
    StmtKind kind;
    int line = 0;

    // VarDecl
    std::string declType;  // "" for `var` / implicit
    std::string name;
    ExprPtr init;

    // ExprStmt / Return / condition
    ExprPtr expr;

    // If
    ExprPtr cond;
    std::vector<StmtPtr> thenBody;
    std::vector<StmtPtr> elseBody;

    // DoWhile / DoAsync
    std::vector<StmtPtr> body;

    // Switch
    ExprPtr subject;
    std::vector<SwitchCase> cases;
};

// ---- Declarations -----------------------------------------------------
struct Param {
    std::string type; // "" allowed
    std::string name;
};

struct FunctionDecl {
    std::string name;
    std::vector<Param> params;
    std::string returnType; // "" if unspecified
    bool isAbstract = false;
    std::vector<StmtPtr> body;
    int line = 0;
};

struct FieldDecl {
    std::string type; // "" for `var`
    std::string name;
    ExprPtr init;
};

struct ClassDecl {
    std::string name;
    std::string base = "Actor"; // default base
    bool isStatic = false;
    bool isAbstract = false;
    std::vector<FieldDecl> fields;
    std::vector<FunctionDecl> functions;
};

} // namespace crate::script
