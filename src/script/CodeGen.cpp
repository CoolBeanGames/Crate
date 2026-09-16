#include "script/CodeGen.h"

#include "script/Runtime.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <unordered_set>

namespace crate::script {
namespace {

// ---------------------------------------------------------------------------
// Identifier / literal text helpers
// ---------------------------------------------------------------------------

std::string sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out += (std::isalnum((unsigned char)c) || c == '_') ? c : '_';
    if (out.empty() || std::isdigit((unsigned char)out[0]))
        out = "_" + out;
    return out;
}

std::string fieldMember(const std::string& n) { return "field_" + sanitize(n); }
std::string methodName(const std::string& n) { return "fn_" + sanitize(n); }
std::string localName(const std::string& n) { return "local_" + sanitize(n); }

std::string cppStringLiteral(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
                    out += buf;
                } else {
                    out += (char)c;
                }
        }
    }
    out += "\"";
    return out;
}

std::string cppCharLiteral(char c) {
    std::string out = "'";
    switch (c) {
        case '\'': out += "\\'"; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\0': out += "\\0"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\x%02x", (unsigned char)c);
                out += buf;
            } else {
                out += c;
            }
    }
    out += "'";
    return out;
}

std::string cppDoubleLiteral(double v) {
    char buf[64];
    // %.17g round-trips any double exactly; a plain integer-looking result
    // (e.g. "5") is still a valid double literal in C++.
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

// crate::script::Tok's enumerator name, for embedding a Tok value as C++
// source text in a Runtime::arith(...)/mathCall-style call. Only the
// operators arith()/comparisons actually handle need to round-trip here.
const char* tokEnumName(Tok op) {
    switch (op) {
        case Tok::Plus: return "Plus";
        case Tok::Minus: return "Minus";
        case Tok::Star: return "Star";
        case Tok::Slash: return "Slash";
        case Tok::Percent: return "Percent";
        default: return nullptr;
    }
}

// ---------------------------------------------------------------------------
// Per-class generator
// ---------------------------------------------------------------------------

// Tracks which cScript names are currently in-scope locals (function params
// and `var` declarations seen so far), so an Identifier can be resolved the
// same way Interpreter::eval does: local shadows field.
struct FnCtx {
    std::vector<std::unordered_set<std::string>> scopes;
    void push() { scopes.emplace_back(); }
    void pop() { scopes.pop_back(); }
    void declare(const std::string& n) { scopes.back().insert(n); }
    bool has(const std::string& n) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
            if (it->count(n))
                return true;
        return false;
    }
};

class Gen {
public:
    explicit Gen(const ClassDecl& decl) : decl_(decl) {
        for (const auto& f : decl_.fields)
            fieldNames_.insert(f.name);
        for (const auto& f : decl_.functions)
            funcNames_.insert(f.name);
    }

    bool ok() const { return err_.empty(); }
    const std::string& error() const { return err_; }

    bool isField(const std::string& n) const { return fieldNames_.count(n) != 0; }
    bool isOwnFunc(const std::string& n) const { return funcNames_.count(n) != 0; }

    // Returns a C++ expression of type crate::script::Value, or "" (check
    // ok()/error() afterward) on an unsupported construct.
    std::string expr(const Expr& e, FnCtx& fc);

    bool stmt(const Stmt& s, FnCtx& fc, std::ostringstream& out, int indent);
    bool block(const std::vector<StmtPtr>& body, FnCtx& fc, std::ostringstream& out, int indent);

    std::string freshTemp(const char* base) { return std::string("__") + base + std::to_string(tempCounter_++); }

    void fail(const std::string& msg, int line) {
        if (!err_.empty())
            return;
        err_ = "line " + std::to_string(line) + ": " + msg;
    }

private:
    std::string ind(int n) { return std::string((size_t)n * 4, ' '); }

    // Assignment target dispatch (mirrors Interpreter::assign). Appends the
    // storing statement(s) to `out`. `valueExpr` is C++ text for the value
    // to store, embedded exactly once here (the caller is responsible for
    // not needing it again).
    bool assignTo(const Expr& target, const std::string& valueExpr, FnCtx& fc, std::ostringstream& out,
                  int indent, int line);

    const ClassDecl& decl_;
    std::unordered_set<std::string> fieldNames_;
    std::unordered_set<std::string> funcNames_;
    std::string err_;
    int tempCounter_ = 0;
};

// True if `e` is `transform` or `actor` used bare (the magic identifiers
// every script class exposes for its owning Actor -- see
// Interpreter::eval's ExprKind::Identifier case).
bool isTransformOrActorIdent(const Expr& e) {
    return e.kind == ExprKind::Identifier && (e.strVal == "transform" || e.strVal == "actor");
}

// The Member chain `transform.position` / `actor.rotation` / etc: both magic
// identifiers alias the same owner Actor, exactly as the interpreter treats
// them (see actorMember()).
bool isTransformField(const std::string& name) {
    return name == "position" || name == "rotation" || name == "scale";
}

std::string Gen::expr(const Expr& e, FnCtx& fc) {
    switch (e.kind) {
        case ExprKind::IntLit:
            return "crate::script::Value::Int(" + std::to_string(e.intVal) + "LL)";
        case ExprKind::FloatLit:
            return "crate::script::Value::Float(" + cppDoubleLiteral(e.floatVal) + ")";
        case ExprKind::StringLit:
            return "crate::script::Value::Str(std::string(" + cppStringLiteral(e.strVal) + "))";
        case ExprKind::CharLit:
            return "crate::script::Value::Char(" +
                   cppCharLiteral(e.strVal.empty() ? '\0' : e.strVal[0]) + ")";
        case ExprKind::BoolLit:
            return std::string("crate::script::Value::Bool(") + (e.boolVal ? "true" : "false") + ")";
        case ExprKind::NullLit:
            return "crate::script::Value::Null_()";

        case ExprKind::This:
            fail("'this' cannot be used as a plain value yet (Phase 5) -- only this.<field> / "
                 "this.<method>(...)",
                 e.line);
            return "";
        case ExprKind::Base:
            fail("'base' is not supported yet (Phase 5 / script-to-script inheritance)", e.line);
            return "";

        case ExprKind::ArrayLit: {
            std::string out = "crate::script::Value::Arr(std::vector<crate::script::Value>{";
            for (size_t i = 0; i < e.items.size(); ++i) {
                if (i)
                    out += ", ";
                out += "(" + expr(*e.items[i], fc) + ")";
                if (!ok())
                    return "";
            }
            out += "})";
            return out;
        }

        case ExprKind::Identifier: {
            const std::string& n = e.strVal;
            if (fc.has(n))
                return localName(n);
            if (isField(n))
                return "this->" + fieldMember(n);
            if (n == "transform" || n == "actor")
                return "crate::script::Value::ActorRef(owner_)";
            // A bare type name evaluates to a TypeRef. Phase 2 only knows
            // about this class's own name and the hardcoded built-ins the
            // interpreter itself hardcodes (Interpreter.cpp's Identifier
            // case) -- anything else would need the cross-class type table
            // (Phase 4/5).
            static const std::unordered_set<std::string> kBuiltinTypeNames = {
                "Actor", "Actor2D", "Actor3D", "Vector2", "Vector3", "Fog", "Camera"};
            if (n == decl_.name || kBuiltinTypeNames.count(n))
                return "crate::script::Value::Type(" + cppStringLiteral(n) + ")";
            // A bare reference to one of this class's own declared methods,
            // used as a first-class value rather than immediately called
            // (e.g. `var f = helper;` for later `.connect`/invocation) --
            // the interpreter turns this into a Callable, which Phase 2
            // doesn't support yet (Phase 5). This is a genuine, always-wrong
            // construct that CodeGen CAN detect statically, so it's refused
            // here rather than deferred to a runtime crash.
            if (isOwnFunc(n)) {
                fail("'" + n +
                         "' used as a bare value (not immediately called) is not supported yet -- "
                         "first-class function references need Phase 5",
                     e.line);
                return "";
            }
            // Anything else falls back to the dynamic overflow map at
            // RUNTIME, exactly mirroring Interpreter::eval's Identifier case
            // (`self_->fields.count(e.strVal)`): an identifier that was
            // auto-vivified by an earlier assignment in this same function
            // (see assignTo()'s Identifier case) resolves here, even though
            // CodeGen cannot prove that statically -- and if it was never
            // assigned, this throws the identical "unknown identifier" error
            // the interpreter would, just detected at runtime instead of at
            // parse time (unavoidable: cScript's field creation is
            // inherently dynamic).
            std::string tmp = freshTemp("id");
            std::ostringstream o;
            o << "[&]() -> crate::script::Value {\n";
            o << ind(1) << "auto " << tmp << " = this->overflow_.find(" << cppStringLiteral(n)
              << ");\n";
            o << ind(1) << "if (" << tmp << " != this->overflow_.end()) return " << tmp
              << "->second;\n";
            o << ind(1) << "throw crate::script::RuntimeError(\"unknown identifier '" << n
              << "'\", " << e.line << ");\n";
            o << ind(0) << "}()";
            return o.str();
        }

        case ExprKind::Unary: {
            std::string a = expr(*e.a, fc);
            if (!ok())
                return "";
            if (e.op == Tok::Minus)
                return "crate::script::negate((" + a + "))";
            if (e.op == Tok::Not)
                return "crate::script::Value::Bool(!(" + a + ").truthy())";
            return a;
        }

        case ExprKind::Binary: {
            if (e.op == Tok::AndAnd)
                return "crate::script::Value::Bool((" + expr(*e.a, fc) + ").truthy() && (" +
                       expr(*e.b, fc) + ").truthy())";
            if (e.op == Tok::OrOr)
                return "crate::script::Value::Bool((" + expr(*e.a, fc) + ").truthy() || (" +
                       expr(*e.b, fc) + ").truthy())";
            std::string a = expr(*e.a, fc);
            if (!ok())
                return "";
            std::string b = expr(*e.b, fc);
            if (!ok())
                return "";
            switch (e.op) {
                case Tok::EqEq: return "crate::script::valueEquals((" + a + "), (" + b + "))";
                case Tok::NotEq: return "crate::script::valueNotEquals((" + a + "), (" + b + "))";
                case Tok::Lt: return "crate::script::valueLess((" + a + "), (" + b + "))";
                case Tok::Gt: return "crate::script::valueGreater((" + a + "), (" + b + "))";
                case Tok::LtEq: return "crate::script::valueLessEq((" + a + "), (" + b + "))";
                case Tok::GtEq: return "crate::script::valueGreaterEq((" + a + "), (" + b + "))";
                default: break;
            }
            const char* tn = tokEnumName(e.op);
            if (!tn) {
                fail("operator not supported", e.line);
                return "";
            }
            return "crate::script::arith(crate::script::Tok::" + std::string(tn) + ", (" + a +
                   "), (" + b + "), " + std::to_string(e.line) + ")";
        }

        case ExprKind::Assign: {
            std::string rhs = expr(*e.b, fc);
            if (!ok())
                return "";
            std::string valueText;
            if (e.op != Tok::Unknown) {
                std::string cur = expr(*e.a, fc); // target read, see Interpreter::eval's Assign case
                if (!ok())
                    return "";
                const char* tn = tokEnumName(e.op);
                if (!tn) {
                    fail("compound-assignment operator not supported", e.line);
                    return "";
                }
                valueText = "crate::script::arith(crate::script::Tok::" + std::string(tn) + ", (" +
                            cur + "), (" + rhs + "), " + std::to_string(e.line) + ")";
            } else {
                valueText = rhs;
            }
            std::string tmp = freshTemp("assign");
            std::ostringstream body;
            body << "[&]() -> crate::script::Value {\n";
            body << ind(1) << "crate::script::Value " << tmp << " = (" << valueText << ");\n";
            std::ostringstream storeOut;
            if (!assignTo(*e.a, tmp, fc, storeOut, 1, e.line))
                return "";
            body << storeOut.str();
            body << ind(1) << "return " << tmp << ";\n";
            body << ind(0) << "}()";
            return body.str();
        }

        case ExprKind::Call: {
            const Expr& callee = *e.a;

            if (callee.kind == ExprKind::Identifier) {
                const std::string& name = callee.strVal;
                std::vector<std::string> args;
                for (const auto& a : e.items) {
                    args.push_back(expr(*a, fc));
                    if (!ok())
                        return "";
                }
                if (name == "print") {
                    std::string out = "[&]() -> crate::script::Value {\n";
                    out += ind(1) + "std::string __s;\n";
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i)
                            out += ind(1) + "__s += \" \";\n";
                        out += ind(1) + "__s += (" + args[i] + ").str();\n";
                    }
                    out += ind(1) + "if (ctx_ && ctx_->print) ctx_->print(__s);\n";
                    out += ind(1) + "return crate::script::Value::Null_();\n";
                    out += ind(0) + "}()";
                    return out;
                }
                if (name == "type_of") {
                    if (args.empty())
                        return "crate::script::Value::Type(std::string())";
                    return "[&]() -> crate::script::Value {\n" + ind(1) +
                           "crate::script::Value __a = (" + args[0] + ");\n" + ind(1) +
                           "return __a.t == crate::script::Value::T::TypeRef ? __a : "
                           "crate::script::Value::Type(__a.typeName());\n" +
                           ind(0) + "}()";
                }
                if (name == "str") {
                    if (args.size() != 1) {
                        fail("str(...) expects exactly one argument", e.line);
                        return "";
                    }
                    return "crate::script::Value::Str((" + args[0] + ").str())";
                }
                if (name == "Vector3" || name == "Vector2") {
                    std::string x = args.size() > 0 ? "(" + args[0] + ").num()" : "0.0";
                    std::string y = args.size() > 1 ? "(" + args[1] + ").num()" : "0.0";
                    std::string z = args.size() > 2 ? "(" + args[2] + ").num()" : "0.0";
                    return "crate::script::makeVector(" + cppStringLiteral(name) + ", " + x + ", " +
                           y + ", " + z + ")";
                }
                if (name == "emit_signal") {
                    fail("emit_signal(...) is not supported yet (Phase 5 / signals)", e.line);
                    return "";
                }
                if (isOwnFunc(name)) {
                    std::string out =
                        "this->" + methodName(name) + "(std::vector<crate::script::Value>{";
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i)
                            out += ", ";
                        out += "(" + args[i] + ")";
                    }
                    out += "})";
                    return out;
                }
                fail("unknown function '" + name + "'", e.line);
                return "";
            }

            if (callee.kind == ExprKind::Member) {
                const Expr& objExpr = *callee.a;
                const std::string& method = callee.strVal;

                if (objExpr.kind == ExprKind::Member && objExpr.strVal == "base" &&
                    objExpr.a->kind == ExprKind::This) {
                    fail("this.base.<method>(...) is not supported yet (Phase 5 / script-to-script "
                         "inheritance)",
                         e.line);
                    return "";
                }

                if (objExpr.kind == ExprKind::Identifier && objExpr.strVal == "Math" && !fc.has("Math") &&
                    !isField("Math")) {
                    std::vector<std::string> args;
                    for (const auto& a : e.items) {
                        args.push_back(expr(*a, fc));
                        if (!ok())
                            return "";
                    }
                    std::string tmp = freshTemp("mathargs");
                    std::ostringstream o;
                    o << "[&]() -> crate::script::Value {\n";
                    o << ind(1) << "std::vector<crate::script::Value> " << tmp << " = {";
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i)
                            o << ", ";
                        o << "(" << args[i] << ")";
                    }
                    o << "};\n";
                    o << ind(1) << "return crate::script::mathCall(" << cppStringLiteral(method)
                      << ", " << tmp << ", " << e.line << ");\n";
                    o << ind(0) << "}()";
                    return o.str();
                }

                if (objExpr.kind == ExprKind::Identifier && objExpr.strVal == "Input" && !fc.has("Input")) {
                    std::string n = e.items.empty() ? "std::string()" : "(" + expr(*e.items[0], fc) + ").str()";
                    if (!ok())
                        return "";
                    if (method == "get_button")
                        return "(ctx_ && ctx_->inputButton ? crate::script::Value::Obj(ctx_->inputButton(" +
                               n + ")) : crate::script::Value::Null_())";
                    if (method == "get_axis")
                        return "crate::script::makeVector(\"Vector2\", ctx_ && ctx_->inputQuery ? "
                               "ctx_->inputQuery(" +
                               n + ", 3) : 0.0, ctx_ && ctx_->inputQuery ? ctx_->inputQuery(" + n +
                               ", 4) : 0.0, 0.0)";
                    if (method == "is_pressed")
                        return "crate::script::Value::Bool(ctx_ && ctx_->inputQuery && "
                               "ctx_->inputQuery(" +
                               n + ", 0) != 0.0)";
                    if (method == "is_just_pressed")
                        return "crate::script::Value::Bool(ctx_ && ctx_->inputQuery && "
                               "ctx_->inputQuery(" +
                               n + ", 1) != 0.0)";
                    if (method == "is_just_released")
                        return "crate::script::Value::Bool(ctx_ && ctx_->inputQuery && "
                               "ctx_->inputQuery(" +
                               n + ", 2) != 0.0)";
                    fail("Input has no method '" + method + "'", e.line);
                    return "";
                }

                if (objExpr.kind == ExprKind::This) {
                    if (isOwnFunc(method)) {
                        std::vector<std::string> args;
                        for (const auto& a : e.items) {
                            args.push_back(expr(*a, fc));
                            if (!ok())
                                return "";
                        }
                        std::string out =
                            "this->" + methodName(method) + "(std::vector<crate::script::Value>{";
                        for (size_t i = 0; i < args.size(); ++i) {
                            if (i)
                                out += ", ";
                            out += "(" + args[i] + ")";
                        }
                        out += "})";
                        return out;
                    }
                    fail("this." + method +
                             "(...) is not supported yet (get_component / signals need Phase 5)",
                         e.line);
                    return "";
                }

                // General receiver: only a small, fixed set of universal
                // methods is supported without the Phase 5 reflection table.
                std::string objText = expr(objExpr, fc);
                if (!ok())
                    return "";
                if (method == "str")
                    return "crate::script::Value::Str((" + objText + ").str())";
                if (method == "length")
                    return "crate::script::arrayLength((" + objText + "))";
                if (method == "add") {
                    if (e.items.size() != 1) {
                        fail("array.add(...) expects exactly one argument", e.line);
                        return "";
                    }
                    std::string item = expr(*e.items[0], fc);
                    if (!ok())
                        return "";
                    std::string tmp = freshTemp("recv");
                    std::ostringstream o;
                    o << "[&]() -> crate::script::Value {\n";
                    o << ind(1) << "crate::script::Value " << tmp << " = (" << objText << ");\n";
                    o << ind(1) << "if (!crate::script::arrayAdd(" << tmp << ", (" << item
                      << ")))\n";
                    o << ind(2) << "throw crate::script::RuntimeError(\"no method 'add'\", "
                      << e.line << ");\n";
                    o << ind(1) << "return crate::script::Value::Null_();\n";
                    o << ind(0) << "}()";
                    return o.str();
                }
                fail("method '" + method + "' is not supported yet on a general expression (Phase 5)",
                     e.line);
                return "";
            }

            fail("expression is not callable", e.line);
            return "";
        }

        case ExprKind::Member: {
            const Expr& objExpr = *e.a;
            const std::string& name = e.strVal;

            if (objExpr.kind == ExprKind::This) {
                if (isField(name))
                    return "this->" + fieldMember(name);
                if (name == "actor")
                    return "crate::script::Value::ActorRef(owner_)";
                fail("this." + name + " is not a declared field (get_component / signals need Phase 5)",
                     e.line);
                return "";
            }

            if (isTransformOrActorIdent(objExpr)) {
                if (name == "name")
                    return "crate::script::Value::Str(owner_ ? owner_->name() : std::string())";
                if (isTransformField(name)) {
                    std::string f = name == "position" ? "position" : name == "rotation" ? "rotationEuler" : "scale";
                    return "crate::script::makeVector(\"Vector3\", owner_->transform()." + f +
                           ".x, owner_->transform()." + f + ".y, owner_->transform()." + f + ".z)";
                }
                if (name == "forward" || name == "right" || name == "up") {
                    std::string base = name == "forward" ? "crate::Vec3{0,0,1}"
                                       : name == "right"  ? "crate::Vec3{1,0,0}"
                                                           : "crate::Vec3{0,1,0}";
                    std::string tmp = freshTemp("dir");
                    std::ostringstream o;
                    o << "[&]() -> crate::script::Value {\n";
                    o << ind(1) << "crate::Mat4 __rot = "
                                  "crate::Mat4::rotationEuler(owner_->transform().rotationEuler);\n";
                    o << ind(1) << "crate::Vec3 " << tmp
                      << " = crate::normalize(crate::transformDirection(" << base << ", __rot));\n";
                    o << ind(1) << "return crate::script::makeVector(\"Vector3\", " << tmp << ".x, "
                      << tmp << ".y, " << tmp << ".z);\n";
                    o << ind(0) << "}()";
                    return o.str();
                }
                fail("Actor has no member '" + name + "'", e.line);
                return "";
            }

            std::string objText = expr(objExpr, fc);
            if (!ok())
                return "";
            if (name == "x" || name == "y" || name == "z")
                return "crate::script::Value::Float(crate::script::vfield((" + objText + "), " +
                       cppStringLiteral(name) + "))";
            if (name == "length")
                return "crate::script::arrayLength((" + objText + "))";
            fail("member '" + name + "' is not supported yet on a general expression (Phase 5)",
                 e.line);
            return "";
        }

        case ExprKind::Index: {
            std::string base = expr(*e.a, fc);
            if (!ok())
                return "";
            std::string idx = expr(*e.b, fc);
            if (!ok())
                return "";
            std::string tb = freshTemp("base"), ti = freshTemp("idx"), tk = freshTemp("k");
            std::ostringstream o;
            o << "[&]() -> crate::script::Value {\n";
            o << ind(1) << "crate::script::Value " << tb << " = (" << base << ");\n";
            o << ind(1) << "crate::script::Value " << ti << " = (" << idx << ");\n";
            o << ind(1) << "if (" << tb << ".t != crate::script::Value::T::Array || !" << tb
              << ".arr)\n";
            o << ind(2) << "throw crate::script::RuntimeError(\"cannot index a \" + "
                          "std::string("
              << tb << ".typeName()), " << e.line << ");\n";
            o << ind(1) << "long long " << tk << " = (long long)(" << ti << ").num();\n";
            o << ind(1) << "if (" << tk << " < 0 || " << tk << " >= (long long)" << tb
              << ".arr->size())\n";
            o << ind(2) << "throw crate::script::RuntimeError(\"array index out of range\", "
              << e.line << ");\n";
            o << ind(1) << "return (*" << tb << ".arr)[" << tk << "];\n";
            o << ind(0) << "}()";
            return o.str();
        }
    }
    fail("unsupported expression", e.line);
    return "";
}

bool Gen::assignTo(const Expr& target, const std::string& valueExpr, FnCtx& fc, std::ostringstream& out,
                   int indent, int line) {
    // Single-component transform write-back: transform.position.y = v /
    // actor.rotation.x += v, etc. (script_bug_tests.cpp tasks 45/46).
    if (target.kind == ExprKind::Member && target.a && target.a->kind == ExprKind::Member &&
        (target.strVal == "x" || target.strVal == "y" || target.strVal == "z")) {
        const Expr& mid = *target.a;
        if (isTransformField(mid.strVal) && isTransformOrActorIdent(*mid.a)) {
            std::string f = mid.strVal == "position" ? "position"
                            : mid.strVal == "rotation" ? "rotationEuler"
                                                        : "scale";
            out << ind(indent) << "owner_->transform()." << f << "." << target.strVal
                << " = (float)(" << valueExpr << ").num();\n";
            return true;
        }
    }

    if (target.kind == ExprKind::Member) {
        const Expr& objExpr = *target.a;

        if (objExpr.kind == ExprKind::This) {
            if (isField(target.strVal))
                out << ind(indent) << "this->" << fieldMember(target.strVal) << " = (" << valueExpr
                    << ");\n";
            else
                out << ind(indent) << "this->overflow_[" << cppStringLiteral(target.strVal)
                    << "] = (" << valueExpr << ");\n";
            return true;
        }

        if (isTransformOrActorIdent(objExpr) && isTransformField(target.strVal)) {
            std::string f = target.strVal == "position" ? "position"
                            : target.strVal == "rotation" ? "rotationEuler"
                                                           : "scale";
            std::string tmp = freshTemp("vecw");
            out << ind(indent) << "{\n";
            out << ind(indent + 1) << "crate::script::Value " << tmp << " = (" << valueExpr << ");\n";
            out << ind(indent + 1) << "owner_->transform()." << f << " = crate::Vec3{(float)"
                << "crate::script::vfield(" << tmp << ", \"x\"), (float)crate::script::vfield(" << tmp
                << ", \"y\"), (float)crate::script::vfield(" << tmp << ", \"z\")};\n";
            out << ind(indent) << "}\n";
            return true;
        }

        std::string objText = expr(objExpr, fc);
        if (!ok())
            return false;
        if (target.strVal == "x" || target.strVal == "y" || target.strVal == "z") {
            out << ind(indent) << "crate::script::vfieldSet((" << objText << "), "
                << cppStringLiteral(target.strVal) << ", (" << valueExpr << ").num());\n";
            return true;
        }
        fail("cannot assign member '" + target.strVal + "' on a general expression (Phase 5)", line);
        return false;
    }

    if (target.kind == ExprKind::Identifier) {
        const std::string& n = target.strVal;
        if (fc.has(n))
            out << ind(indent) << localName(n) << " = (" << valueExpr << ");\n";
        else if (isField(n))
            out << ind(indent) << "this->" << fieldMember(n) << " = (" << valueExpr << ");\n";
        else
            out << ind(indent) << "this->overflow_[" << cppStringLiteral(n) << "] = (" << valueExpr
                << ");\n";
        return true;
    }

    if (target.kind == ExprKind::Index) {
        std::string base = expr(*target.a, fc);
        if (!ok())
            return false;
        std::string idx = expr(*target.b, fc);
        if (!ok())
            return false;
        std::string tb = freshTemp("wbase"), ti = freshTemp("widx"), tk = freshTemp("wk");
        out << ind(indent) << "{\n";
        out << ind(indent + 1) << "crate::script::Value " << tb << " = (" << base << ");\n";
        out << ind(indent + 1) << "crate::script::Value " << ti << " = (" << idx << ");\n";
        out << ind(indent + 1) << "long long " << tk << " = (long long)(" << ti << ").num();\n";
        out << ind(indent + 1) << "if (" << tb << ".t != crate::script::Value::T::Array || !" << tb
            << ".arr || " << tk << " < 0 || " << tk << " >= (long long)" << tb << ".arr->size())\n";
        out << ind(indent + 2)
            << "throw crate::script::RuntimeError(\"invalid assignment target\", " << line << ");\n";
        out << ind(indent + 1) << "(*" << tb << ".arr)[" << tk << "] = (" << valueExpr << ");\n";
        out << ind(indent) << "}\n";
        return true;
    }

    fail("invalid assignment target", line);
    return false;
}

bool Gen::block(const std::vector<StmtPtr>& body, FnCtx& fc, std::ostringstream& out, int indent) {
    fc.push();
    for (const auto& s : body) {
        if (!stmt(*s, fc, out, indent)) {
            fc.pop();
            return false;
        }
    }
    fc.pop();
    return true;
}

bool Gen::stmt(const Stmt& s, FnCtx& fc, std::ostringstream& out, int indent) {
    switch (s.kind) {
        case StmtKind::Block: {
            out << ind(indent) << "{\n";
            if (!block(s.body, fc, out, indent + 1))
                return false;
            out << ind(indent) << "}\n";
            return true;
        }
        case StmtKind::VarDecl: {
            std::string init = s.init ? expr(*s.init, fc) : "crate::script::Value::Null_()";
            if (!ok())
                return false;
            out << ind(indent) << "crate::script::Value " << localName(s.name)
                << " = crate::script::coerce((" << init << "), " << cppStringLiteral(s.declType)
                << ");\n";
            fc.declare(s.name);
            return true;
        }
        case StmtKind::ExprStmt: {
            std::string e = expr(*s.expr, fc);
            if (!ok())
                return false;
            out << ind(indent) << "(void)(" << e << ");\n";
            return true;
        }
        case StmtKind::Return: {
            std::string e = s.expr ? expr(*s.expr, fc) : "crate::script::Value::Null_()";
            if (!ok())
                return false;
            out << ind(indent) << "return (" << e << ");\n";
            return true;
        }
        case StmtKind::If: {
            std::string c = expr(*s.cond, fc);
            if (!ok())
                return false;
            out << ind(indent) << "if ((" << c << ").truthy()) {\n";
            if (!block(s.thenBody, fc, out, indent + 1))
                return false;
            out << ind(indent) << "}\n";
            if (!s.elseBody.empty()) {
                out << ind(indent) << "else {\n";
                if (!block(s.elseBody, fc, out, indent + 1))
                    return false;
                out << ind(indent) << "}\n";
            }
            return true;
        }
        case StmtKind::DoWhile:
        case StmtKind::DoAsync: {
            // Phase 2 compiles BOTH kinds as an ordinary, synchronous,
            // test-then-execute loop -- exactly matching what
            // Interpreter::execStmt does today for a *nested* do_async (and,
            // not coincidentally, for DoWhile too: despite the "do" in the
            // name, the interpreter checks the condition BEFORE the first
            // iteration, i.e. a while-loop, not a true do-while). Top-level
            // do_async frame-stepped resumption is Phase 6.
            std::string guard = freshTemp("guard");
            out << ind(indent) << "{\n";
            out << ind(indent + 1) << "int " << guard << " = 0;\n";
            std::string c = expr(*s.cond, fc);
            if (!ok())
                return false;
            out << ind(indent + 1) << "while ((" << c << ").truthy()) {\n";
            if (!block(s.body, fc, out, indent + 2))
                return false;
            out << ind(indent + 2) << "if (++" << guard << " > 1000000)\n";
            out << ind(indent + 3)
                << "throw crate::script::RuntimeError(\"loop exceeded 1,000,000 iterations\", "
                << s.line << ");\n";
            // The `while (...)` header's condition text is embedded once,
            // above; C++ itself re-evaluates that expression before every
            // iteration, matching execStmt's own re-`eval(*s.cond)` per pass
            // with no need to re-emit it here.
            out << ind(indent + 1) << "}\n";
            out << ind(indent) << "}\n";
            return true;
        }
        case StmtKind::Switch: {
            // Emitted as a SEQUENCE of independent, self-contained case
            // blocks (NOT an if/else-if chain -- a case-value temp
            // declaration can't legally sit between an `if` and its `else`,
            // and NOT a native switch -- case values are arbitrary
            // expressions, not compile-time constants, matched via a
            // specific (str()==str()) || (numeric && num()==num()) rule,
            // see Interpreter::execStmt's Switch case). Every case body ends
            // with an unconditional `break;`, so reaching a match runs
            // exactly that one case's body then exits the enclosing
            // `do { } while(false)` immediately -- both implementing "no
            // fallthrough, first match wins" and giving a bare `break;`
            // written inside cScript case body (legal, a no-op relative to
            // just falling off the case) something to target. A `continue;`
            // written inside a case body is NOT caught here -- it falls
            // through to whatever loop actually encloses this switch,
            // matching native C++ scoping (and the interpreter's own
            // exception-based unwind, which only DoWhile/DoAsync ever
            // catches a ContinueSignal).
            std::string subj = expr(*s.subject, fc);
            if (!ok())
                return false;
            std::string subjVar = freshTemp("subj");
            out << ind(indent) << "do {\n";
            out << ind(indent + 1) << "crate::script::Value " << subjVar << " = (" << subj << ");\n";
            for (const auto& c : s.cases) {
                if (!c.value) {
                    // default: always matches once control reaches it (i.e.
                    // nothing earlier in source order already matched and
                    // broken out) -- no condition, no temp needed.
                    out << ind(indent + 1) << "{\n";
                    if (!block(c.body, fc, out, indent + 2))
                        return false;
                    out << ind(indent + 2) << "break;\n";
                    out << ind(indent + 1) << "}\n";
                    continue;
                }
                std::string cv = expr(*c.value, fc);
                if (!ok())
                    return false;
                std::string cvVar = freshTemp("case");
                out << ind(indent + 1) << "{\n";
                out << ind(indent + 2) << "crate::script::Value " << cvVar << " = (" << cv << ");\n";
                out << ind(indent + 2) << "if ((" << cvVar << ".str() == " << subjVar
                    << ".str()) || (" << cvVar << ".isNumeric() && " << subjVar
                    << ".isNumeric() && " << cvVar << ".num() == " << subjVar << ".num())) {\n";
                if (!block(c.body, fc, out, indent + 3))
                    return false;
                out << ind(indent + 3) << "break;\n";
                out << ind(indent + 2) << "}\n";
                out << ind(indent + 1) << "}\n";
            }
            out << ind(indent) << "} while (false);\n";
            return true;
        }
        case StmtKind::Break:
            out << ind(indent) << "break;\n";
            return true;
        case StmtKind::Continue:
            out << ind(indent) << "continue;\n";
            return true;
    }
    fail("unsupported statement", s.line);
    return false;
}

} // namespace

CodeGenResult generateClass(const ClassDecl& decl) {
    CodeGenResult r;

    if (decl.isStatic) {
        r.error = "static classes are not supported yet (Phase 4/5 / ScriptSystem statics registry)";
        return r;
    }
    if (decl.isAbstract) {
        r.error = "abstract classes are not compiled to native code (they are never instantiated "
                  "directly)";
        return r;
    }
    if (decl.base != "Actor" && decl.base != "Actor2D" && decl.base != "Actor3D") {
        r.error = "class '" + decl.name + "' has base '" + decl.base +
                  "' -- only a direct Actor/Actor2D/Actor3D base is supported yet (script-to-script "
                  "inheritance needs Phase 4/5's cross-class registry)";
        return r;
    }
    if (!decl.signals.empty()) {
        r.error = "signal declarations are not supported yet (Phase 5)";
        return r;
    }
    for (const auto& fn : decl.functions) {
        if (fn.isAbstract) {
            r.error = "function '" + fn.name + "' is abstract, but '" + decl.name +
                      "' is not an abstract class";
            return r;
        }
    }

    const std::string cls = sanitize(decl.name) + "_Native";
    r.className = cls;

    Gen gen(decl);

    // ---- header ----
    std::ostringstream h;
    h << "#pragma once\n";
    h << "#include \"script/Runtime.h\"\n";
    h << "#include \"script/Interpreter.h\"\n";
    h << "#include \"scene/Actor.h\"\n";
    h << "#include \"core/Math.h\"\n\n";
    h << "#include <string>\n#include <unordered_map>\n#include <vector>\n\n";
    h << "namespace crate::script::generated {\n\n";
    h << "class " << cls << " {\n";
    h << "public:\n";
    h << "    " << cls << "(crate::script::ScriptContext* ctx, crate::Actor* owner);\n\n";
    for (const auto& fn : decl.functions)
        h << "    crate::script::Value " << methodName(fn.name)
          << "(std::vector<crate::script::Value> args);\n";
    h << "\n";
    for (const auto& f : decl.fields)
        h << "    crate::script::Value " << fieldMember(f.name) << ";\n";
    h << "    // Undeclared-field auto-vivification (mirrors Interpreter::lvalue()).\n";
    h << "    std::unordered_map<std::string, crate::script::Value> overflow_;\n\n";
    h << "private:\n";
    h << "    crate::script::ScriptContext* ctx_;\n";
    h << "    crate::Actor* owner_;\n";
    h << "};\n\n";
    h << "} // namespace crate::script::generated\n";
    r.header = h.str();

    // ---- source ----
    std::ostringstream c;
    c << "#include \"" << cls << ".gen.h\"\n\n";
    c << "namespace crate::script::generated {\n\n";
    c << cls << "::" << cls << "(crate::script::ScriptContext* ctx, crate::Actor* owner)\n";
    c << "    : ctx_(ctx), owner_(owner) {\n";
    {
        FnCtx fieldScope;
        fieldScope.push();
        for (const auto& f : decl.fields) {
            std::string init = f.init ? gen.expr(*f.init, fieldScope) : "crate::script::Value::Null_()";
            if (!gen.ok()) {
                r.error = gen.error();
                return r;
            }
            c << "    " << fieldMember(f.name) << " = crate::script::coerce((" << init << "), "
              << cppStringLiteral(f.type) << ");\n";
        }
    }
    c << "}\n\n";

    for (const auto& fn : decl.functions) {
        c << "crate::script::Value " << cls << "::" << methodName(fn.name)
          << "(std::vector<crate::script::Value> args) {\n";
        FnCtx fc;
        fc.push();
        for (size_t i = 0; i < fn.params.size(); ++i) {
            c << "    crate::script::Value " << localName(fn.params[i].name)
              << " = crate::script::coerce(" << i << " < args.size() ? args[" << i
              << "] : crate::script::Value::Null_(), " << cppStringLiteral(fn.params[i].type)
              << ");\n";
            fc.declare(fn.params[i].name);
        }
        std::ostringstream body;
        if (!gen.block(fn.body, fc, body, 1)) {
            r.error = gen.error();
            return r;
        }
        c << body.str();
        c << "    return crate::script::Value::Null_();\n";
        c << "}\n\n";
    }

    c << "} // namespace crate::script::generated\n";
    r.source = c.str();

    r.ok = true;
    return r;
}

} // namespace crate::script
