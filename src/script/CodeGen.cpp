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

    // Break/Continue targeting. TWO INDEPENDENT stacks, because a Switch and
    // a loop (native DoWhile/DoAsync-as-synchronous, or a Phase-6 resumable
    // top-level do_async) behave differently: Interpreter::execStmt's
    // Switch case catches ONLY BreakSignal, never ContinueSignal (see
    // Interpreter.cpp) -- so a cscript `continue;` written inside a switch
    // case must skip PAST the switch entirely and target whatever loop
    // actually encloses it, while `break;` there terminates just the
    // switch. Every loop pushes onto BOTH stacks; a Switch pushes onto
    // breakTargets ONLY, leaving continueTargets exactly as the nearest
    // enclosing loop left it, so a Continue "sees through" any number of
    // intervening switches to the real loop.
    //
    // Every entry always uses a `goto <label>` (never a bare native
    // break;/continue;) -- necessary because a native break/continue
    // targets the SYNTACTICALLY nearest enclosing C++ loop/switch, which
    // would incorrectly stop at a switch's own do{}while(false) wrapper for
    // Continue (see transpiration.txt Phase 6's write-up: this was a real,
    // pre-existing bug in Phase 2's switch-inside-a-loop codegen, found and
    // fixed here). `brokeFlagVar` is only set (and only checked by the
    // emitter) for a resumable do_async's own break target, which needs to
    // distinguish "aborted via break" (skip the yield) from "continue or
    // fell off the end" (yield) -- native loops leave it empty.
    struct BreakTarget {
        std::string gotoLabel;
        std::string brokeFlagVar; // empty for a native loop's break target
    };
    struct ContinueTarget {
        std::string gotoLabel;
    };
    std::vector<BreakTarget> breakTargets;
    std::vector<ContinueTarget> continueTargets;
};

class Gen {
public:
    Gen(const ClassInfo& classInfo, const std::unordered_set<std::string>& knownClassNames)
        : classInfo_(classInfo), decl_(*classInfo.decl), knownClassNames_(knownClassNames) {
        // Own names (THIS class's own AST declarations) -- decide what
        // needs to be EMITTED (field member declarations, method bodies,
        // hasStart/hasUpdate/hasPhysicsUpdate hook-override detection).
        for (const auto& f : decl_.fields)
            ownFieldNames_.insert(f.name);
        for (const auto& f : decl_.functions)
            funcNames_.insert(f.name);
        // Ancestor names (Phase 9f), walking classInfo.baseClass -- a
        // FIELD in this set that ISN'T also in ownFieldNames_ is purely
        // inherited (no member to declare, already exists via C++
        // inheritance); one that's in BOTH is an OVERRIDE (this class
        // redeclares it -- inherited storage, but re-initialized by this
        // class's own constructor, exactly mirroring
        // Interpreter::constructFields()'s base-then-derived chain walk
        // where a same-named derived declaration simply overwrites the
        // single shared fields[] entry).
        for (const ClassInfo* c = classInfo_.baseClass; c; c = c->baseClass) {
            if (!c->decl)
                continue;
            for (const auto& f : c->decl->fields)
                ancestorFieldNames_.insert(f.name);
            for (const auto& f : c->decl->functions)
                ancestorFuncNames_.insert(f.name);
        }
        // Accessible/callable names: own + every ancestor's -- `this->
        // field_X`/`this->fn_X` is valid, unqualified C++ for an INHERITED
        // member too (ordinary member lookup finds it via the base class),
        // so CodeGen must recognize these names exist AT ALL, not just
        // this class's own, when deciding whether `this.name` / bare
        // `name(...)` is a real field/method reference vs the dynamic-
        // overflow-map/unknown-function fallback.
        fieldNames_ = ownFieldNames_;
        fieldNames_.insert(ancestorFieldNames_.begin(), ancestorFieldNames_.end());
        callableNames_ = funcNames_;
        callableNames_.insert(ancestorFuncNames_.begin(), ancestorFuncNames_.end());
        for (const ClassInfo* c = &classInfo_; c; c = c->baseClass) {
            if (!c->decl)
                continue;
            for (const auto& s : c->decl->signals)
                signalNames_.insert(s.name);
        }
    }

    bool ok() const { return err_.empty(); }
    const std::string& error() const { return err_; }

    // Accessible (own OR inherited) -- see the constructor's comment.
    bool isField(const std::string& n) const { return fieldNames_.count(n) != 0; }
    bool isCallable(const std::string& n) const { return callableNames_.count(n) != 0; }
    bool isSignal(const std::string& n) const { return signalNames_.count(n) != 0; }
    // THIS class's own declarations only -- see the constructor's comment.
    bool isOwnFunc(const std::string& n) const { return funcNames_.count(n) != 0; }
    bool isOverrideField(const std::string& n) const { return ancestorFieldNames_.count(n) != 0; }

    // Non-null when this class has a SCRIPT base (Phase 9f) -- the
    // resolved base's own ClassInfo, exactly like classInfo.baseClass.
    const ClassInfo* scriptBase() const { return classInfo_.baseClass; }

    // Returns a C++ expression of type crate::script::Value, or "" (check
    // ok()/error() afterward) on an unsupported construct.
    std::string expr(const Expr& e, FnCtx& fc);

    bool stmt(const Stmt& s, FnCtx& fc, std::ostringstream& out, int indent);
    bool block(const std::vector<StmtPtr>& body, FnCtx& fc, std::ostringstream& out, int indent);

    // Emits a hook function's body (Phase 6, do_async codegen). If `body`
    // has no TOP-LEVEL DoAsync statement, this is identical to block() --
    // only start()/update()/physicsUpdate() ever call this (see
    // generateClass()), matching the interpreter's own rule that only a
    // function invoked via Interpreter::call() (i.e. a hook) gets
    // resumable=true; every other function (helpers reached via
    // this.foo()/bare foo()) always uses runFunction's non-resumable path,
    // so its do_asyncs -- even ones at ITS OWN top level -- are plain
    // synchronous loops, unchanged from Phase 2's block()/stmt() handling.
    // `hookMethodName` is the cScript method name ("start"/"update"/
    // "physics_update"), compared against/stamped onto asyncResumeFn_.
    bool emitHookBody(const FunctionDecl& fn, FnCtx& fc, const std::string& hookMethodName,
                      std::ostringstream& out);

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

    // The non-Callable fallback for a bare `name(args)` call (mirrors
    // Interpreter::builtinCall): print/type_of/str/Vector2/Vector3/
    // emit_signal, this class's own methods, or "unknown function". Split
    // out of expr()'s Call/Identifier case (Phase 9d) so it can be reused
    // as the runtime else-branch after a Callable check on a local/field of
    // the same name. `args` are already-evaluated C++ expression strings.
    // `dynamicFallback` is true when `name` is a KNOWN local/field that
    // just isn't currently a Callable: the interpreter's own fallthrough
    // for that exact case still reaches builtinCall(name, ...) and throws
    // "unknown function" -- but dynamically, at RUNTIME, since it can't be
    // ruled out at parse time either -- so the "no match" ending emits a
    // runtime throw instead of a CodeGen::fail() (which would incorrectly
    // refuse an ENTIRE class just because ONE of its locals COULD shadow a
    // builtin name, even on the branch that's actually a Callable at
    // runtime and never reaches this fallback at all).
    std::string identifierCallDispatch(const std::string& name, const std::vector<std::string>& args,
                                       int line, bool dynamicFallback);

    const ClassInfo& classInfo_;
    const ClassDecl& decl_;
    const std::unordered_set<std::string>& knownClassNames_;
    std::unordered_set<std::string> fieldNames_;       // own + inherited
    std::unordered_set<std::string> ownFieldNames_;    // this class's own decl_.fields only
    std::unordered_set<std::string> ancestorFieldNames_; // declared by an ancestor (may overlap ownFieldNames_ = an override)
    std::unordered_set<std::string> callableNames_;    // own + inherited
    std::unordered_set<std::string> funcNames_;        // this class's own decl_.functions only
    std::unordered_set<std::string> ancestorFuncNames_;
    std::unordered_set<std::string> signalNames_;      // own + inherited
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

std::string Gen::identifierCallDispatch(const std::string& name, const std::vector<std::string>& args,
                                        int line, bool dynamicFallback) {
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
        return "[&]() -> crate::script::Value {\n" + ind(1) + "crate::script::Value __a = (" +
               args[0] + ");\n" + ind(1) +
               "return __a.t == crate::script::Value::T::TypeRef ? __a : "
               "crate::script::Value::Type(__a.typeName());\n" +
               ind(0) + "}()";
    }
    if (name == "str") {
        if (args.size() != 1) {
            fail("str(...) expects exactly one argument", line);
            return "";
        }
        return "crate::script::Value::Str((" + args[0] + ").str())";
    }
    if (name == "Vector3" || name == "Vector2") {
        std::string x = args.size() > 0 ? "(" + args[0] + ").num()" : "0.0";
        std::string y = args.size() > 1 ? "(" + args[1] + ").num()" : "0.0";
        std::string z = args.size() > 2 ? "(" + args[2] + ").num()" : "0.0";
        return "crate::script::makeVector(" + cppStringLiteral(name) + ", " + x + ", " + y + ", " + z +
               ")";
    }
    if (name == "emit_signal") {
        // Bare global emit_signal(name, ...args) form (Phase 9d), mirroring
        // Interpreter::builtinCall's identical case: fires on `this`
        // (this->selfView_, the canonical compiledInfo-backed view every
        // generated instance owns -- see the constructor).
        if (args.empty()) {
            fail("emit_signal needs a signal name", line);
            return "";
        }
        std::string tmp = freshTemp("emit");
        std::ostringstream o;
        o << "[&]() -> crate::script::Value {\n";
        o << ind(1) << "std::vector<crate::script::Value> " << tmp << "_all = {";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i)
                o << ", ";
            o << "(" << args[i] << ")";
        }
        o << "};\n";
        o << ind(1) << "std::string " << tmp << "_name = " << tmp << "_all[0].str();\n";
        o << ind(1) << "std::vector<crate::script::Value> " << tmp << "_rest(" << tmp
          << "_all.begin() + 1, " << tmp << "_all.end());\n";
        o << ind(1) << "crate::script::emitSignal(ctx_, this->selfView_, " << tmp << "_name, std::move("
          << tmp << "_rest), " << line << ");\n";
        o << ind(1) << "return crate::script::Value::Null_();\n";
        o << ind(0) << "}()";
        return o.str();
    }
    if (isCallable(name)) {
        // Own OR inherited (Phase 9f) -- this->fn_<name>(...) is valid,
        // unqualified C++ either way (ordinary member lookup finds an
        // inherited method through the base class automatically).
        std::string out = "this->" + methodName(name) + "(std::vector<crate::script::Value>{";
        for (size_t i = 0; i < args.size(); ++i) {
            if (i)
                out += ", ";
            out += "(" + args[i] + ")";
        }
        out += "})";
        return out;
    }
    if (dynamicFallback)
        return "[&]() -> crate::script::Value { throw crate::script::RuntimeError(\"unknown function '" +
               name + "'\", " + std::to_string(line) + "); }()";
    fail("unknown function '" + name + "'", line);
    return "";
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
            // A bare type name evaluates to a TypeRef: this class's own
            // name, the hardcoded built-ins the interpreter itself
            // hardcodes (Interpreter.cpp's Identifier case), or (Phase 9b)
            // any OTHER script class name known to ScriptSystem at build
            // time -- e.g. `type_of(SomeOtherScript)` for a
            // get_component(type_of(...)) call.
            static const std::unordered_set<std::string> kBuiltinTypeNames = {
                "Actor", "Actor2D", "Actor3D", "Vector2", "Vector3", "Fog", "Camera"};
            if (n == decl_.name || kBuiltinTypeNames.count(n) || knownClassNames_.count(n))
                return "crate::script::Value::Type(" + cppStringLiteral(n) + ")";
            // A bare reference to one of this class's own declared methods,
            // used as a first-class value rather than immediately called
            // (e.g. `var f = helper;` for later `.connect`/invocation) --
            // the interpreter turns this into a Callable bound to self_;
            // the generated-code equivalent binds to `this->selfView_`
            // (Phase 9d), the canonical compiledInfo-backed ScriptObject
            // every generated instance constructs once (see the
            // constructor) -- the SAME object get_component() would hand
            // back for this instance from the outside, so a Callable
            // captured this way keeps working even if `this` itself is
            // later freed while something else still holds the reference
            // (a weak_ptr, exactly like the interpreter's own Callable).
            if (isCallable(n)) // own OR inherited (Phase 9f)
                return "crate::script::Value::Fn(this->selfView_, " + cppStringLiteral(n) + ")";
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
                // First-class function references (Phase 9d): a local or
                // field CURRENTLY holding a Callable is invoked directly --
                // mirrors Interpreter::evalCall's Identifier-callee case
                // exactly (`if (Value* v = findVar(...)) if (v->t ==
                // Callable) return invokeCallable(...)`, then the same
                // check against a self_ field). Checked at RUNTIME (CodeGen
                // can't know statically whether `name` holds a Callable
                // right now); the rest of this branch's dispatch (global
                // builtins / this class's own methods / "unknown function")
                // becomes the non-Callable fallback, via
                // identifierCallDispatch() below.
                std::string recvExpr;
                if (fc.has(name))
                    recvExpr = localName(name);
                else if (isField(name))
                    recvExpr = "this->" + fieldMember(name);
                std::string rest = identifierCallDispatch(name, args, e.line, !recvExpr.empty());
                if (!ok())
                    return "";
                if (recvExpr.empty())
                    return rest;
                std::ostringstream o;
                o << "[&]() -> crate::script::Value {\n";
                o << ind(1) << "if ((" << recvExpr << ").t == crate::script::Value::T::Callable) {\n";
                o << ind(2) << "std::vector<crate::script::Value> __cargs = {";
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i)
                        o << ", ";
                    o << "(" << args[i] << ")";
                }
                o << "};\n";
                o << ind(2) << "return crate::script::invokeCallable(ctx_, (" << recvExpr
                  << "), std::move(__cargs), " << e.line << ");\n";
                o << ind(1) << "}\n";
                o << ind(1) << "return " << rest << ";\n";
                o << ind(0) << "}()";
                return o.str();
            }

            if (callee.kind == ExprKind::Member) {
                const Expr& objExpr = *callee.a;
                const std::string& method = callee.strVal;

                if (objExpr.kind == ExprKind::Member && objExpr.strVal == "base" &&
                    objExpr.a->kind == ExprKind::This) {
                    // this.base.method(...) (Phase 9f): a direct, qualified
                    // C++ call to the base class's OWN fn_<method> --
                    // bypasses any override this class (or any class
                    // between it and wherever <method> is actually
                    // defined) might have, exactly matching
                    // Interpreter::callMethodOn(..., viaBase=true), which
                    // starts its search at obj->cls->baseClass, not
                    // obj->cls itself. Resolved and checked at CODEGEN
                    // TIME here (classInfo_.baseClass->findFunction()),
                    // unlike the interpreter's runtime check, since
                    // CodeGen has full static type info available.
                    if (!scriptBase()) {
                        fail("this.base.<method>(...) requires a script base class", e.line);
                        return "";
                    }
                    if (!scriptBase()->findFunction(method)) {
                        fail("base class has no method '" + method + "'", e.line);
                        return "";
                    }
                    std::vector<std::string> args;
                    for (const auto& a : e.items) {
                        args.push_back(expr(*a, fc));
                        if (!ok())
                            return "";
                    }
                    std::string baseCls = sanitize(scriptBase()->name) + "_Native";
                    std::string out = "this->crate::script::generated::" + baseCls +
                                      "::" + methodName(method) + "(std::vector<crate::script::Value>{";
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i)
                            out += ", ";
                        out += "(" + args[i] + ")";
                    }
                    out += "})";
                    return out;
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
                    if (isCallable(method)) { // own OR inherited (Phase 9f) -- direct call fast path
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
                    // Anything else -- get_component, the Godot-3 signal
                    // shortcuts (emit_signal/connect/disconnect/
                    // is_connected), or a call on a first-class method
                    // reference stored back onto `this` -- routes through
                    // the same generic dispatcher a general receiver uses
                    // (Phase 9b/9d), via this->selfView_ (the canonical
                    // compiledInfo-backed view of `this` every generated
                    // instance constructs once -- see the constructor).
                    // Mirrors how the interpreter's `this` is ALREADY just
                    // Value::Obj(self_) with no special-casing at all.
                    std::vector<std::string> args;
                    for (const auto& a : e.items) {
                        args.push_back(expr(*a, fc));
                        if (!ok())
                            return "";
                    }
                    std::string out = "crate::script::callValueMethod(ctx_, "
                                      "crate::script::Value::Obj(this->selfView_), "
                                      + cppStringLiteral(method) + ", std::vector<crate::script::Value>{";
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (i)
                            out += ", ";
                        out += "(" + args[i] + ")";
                    }
                    out += "}, " + std::to_string(e.line) + ")";
                    return out;
                }

                // General receiver: kind isn't known until runtime (could be
                // a get_component() result, a field holding an Actor
                // reference, a Vector, an Array, ...) -- dispatch generically
                // via callValueMethod (Phase 9b), which mirrors
                // Interpreter::evalCall's own post-special-case fallback
                // over Object/Actor/Array receivers, plus the universal
                // .str() any value supports.
                std::string objText = expr(objExpr, fc);
                if (!ok())
                    return "";
                std::vector<std::string> args;
                for (const auto& a : e.items) {
                    args.push_back(expr(*a, fc));
                    if (!ok())
                        return "";
                }
                std::string tmp = freshTemp("recv");
                std::ostringstream o;
                o << "[&]() -> crate::script::Value {\n";
                o << ind(1) << "crate::script::Value " << tmp << " = (" << objText << ");\n";
                o << ind(1) << "std::vector<crate::script::Value> " << tmp << "_args = {";
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i)
                        o << ", ";
                    o << "(" << args[i] << ")";
                }
                o << "};\n";
                o << ind(1) << "return crate::script::callValueMethod(ctx_, " << tmp << ", "
                  << cppStringLiteral(method) << ", std::move(" << tmp << "_args), " << e.line
                  << ");\n";
                o << ind(0) << "}()";
                return o.str();
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
                // Declared `signal foo();` names (Phase 9d): resolves to a
                // SignalRef pointing at this->selfView_, exactly mirroring
                // Interpreter::evalMember's cls->hasSignal() fallback --
                // needed for `this.mySignal.connect(...)`/`.emit(...)`/etc,
                // which parse as a Call whose receiver expression is THIS
                // Member read, not a this.-qualified Call itself.
                if (isSignal(name))
                    return "crate::script::Value::SignalRef(this->selfView_, " + cppStringLiteral(name) +
                           ")";
                // A bare `this.base` (not immediately followed by
                // `.method(...)`, which is handled entirely in the Call
                // case above) isn't specially handled by the interpreter
                // either (see evalMember's own header comment) -- left
                // unsupported here too, matching that.
                fail("this." + name + " is not a declared field", e.line);
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
            // General receiver: dispatch generically via getValueMember
            // (Phase 9b/9c/9e), which mirrors Interpreter::evalMember's own
            // post-special-case fallback over Object (fields/actor/signal/
            // bound-method)/Actor (position/rotation/scale/forward/right/
            // up/name)/Array (.length)/TypeRef (Camera.main, or any
            // `static class`'s field via ctx_->getStatic) receivers -- this
            // also subsumes the old narrow `.x/.y/.z`-via-vfield special
            // case (vfield() silently returns 0.0 for a non-vector or
            // missing field, whereas the interpreter always throws for a
            // missing member on a general Object receiver -- getValueMember
            // matches the interpreter, not the old shortcut). `owner_` is
            // passed through for Camera.main's own use (see
            // ObjectDispatch.h) -- harmless for every other receiver kind,
            // which ignores it.
            return "crate::script::getValueMember(ctx_, (" + objText + "), " + cppStringLiteral(name) +
                   ", " + std::to_string(e.line) + ", owner_)";
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
        if (isTransformField(mid.strVal)) {
            std::string f = mid.strVal == "position" ? "position"
                            : mid.strVal == "rotation" ? "rotationEuler"
                                                        : "scale";
            if (isTransformOrActorIdent(*mid.a)) {
                out << ind(indent) << "owner_->transform()." << f << "." << target.strVal
                    << " = (float)(" << valueExpr << ").num();\n";
                return true;
            }
            // General Actor-typed base (Phase 9b), e.g.
            // someActorField.position.x = v: <expr>.position returns a
            // FRESH Vector3 copy (see Runtime::actorMember), so a plain
            // member-write on it would be silently lost -- evaluate the
            // base once and, if it's an Actor, route straight into its
            // Transform, mirroring Interpreter::assign()'s identical
            // special case (also not restricted to a bare `transform`/
            // `actor` identifier there). A non-Actor base with a field
            // literally named position/rotation/scale is not supported
            // here (an exotic case the interpreter itself only reaches via
            // a triple re-evaluation fallback quirk of its own -- not worth
            // reproducing bit-for-bit).
            std::string baseText = expr(*mid.a, fc);
            if (!ok())
                return false;
            std::string tmp = freshTemp("actorw");
            out << ind(indent) << "{\n";
            out << ind(indent + 1) << "crate::script::Value " << tmp << " = (" << baseText << ");\n";
            out << ind(indent + 1) << "if (" << tmp << ".t != crate::script::Value::T::Actor || !"
                << tmp << ".actor)\n";
            out << ind(indent + 2)
                << "throw crate::script::RuntimeError(\"cannot assign '." << target.strVal
                << "' on \" + std::string(" << tmp << ".typeName()), " << line << ");\n";
            out << ind(indent + 1) << tmp << ".actor->transform()." << f << "." << target.strVal
                << " = (float)(" << valueExpr << ").num();\n";
            out << ind(indent) << "}\n";
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

        // General receiver: dispatch generically via trySetValueMember
        // (Phase 9b/9e), which mirrors Interpreter::assign()'s own
        // post-special-case fallback -- Object (nativePtr live-view write-
        // back, or a plain fields-map write, which also covers `.x/.y/.z`
        // on a Vector), Actor (position/rotation/scale, replacing both this
        // block's old isTransformOrActorIdent-only fast path AND the old
        // vfieldSet-based `.x/.y/.z` shortcut -- vfieldSet/vfield
        // dereference `.obj` unconditionally, which is undefined behavior
        // for a non-Object Value; trySetValueMember/getObjectMember guard
        // every receiver kind properly, exactly like the interpreter does),
        // and TypeRef (Camera.main, or any `static class`'s field via
        // ctx_->getStatic).
        std::string objText = expr(objExpr, fc);
        if (!ok())
            return false;
        std::string tmp = freshTemp("wrecv");
        out << ind(indent) << "{\n";
        out << ind(indent + 1) << "crate::script::Value " << tmp << " = (" << objText << ");\n";
        out << ind(indent + 1) << "if (!crate::script::trySetValueMember(ctx_, " << tmp << ", "
            << cppStringLiteral(target.strVal) << ", (" << valueExpr << ")))\n";
        out << ind(indent + 2) << "throw crate::script::RuntimeError(\"cannot assign member '"
            << target.strVal << "' on \" + std::string(" << tmp << ".typeName()), " << line << ");\n";
        out << ind(indent) << "}\n";
        return true;
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

bool Gen::emitHookBody(const FunctionDecl& fn, FnCtx& fc, const std::string& hookMethodName,
                       std::ostringstream& out) {
    // Only statements directly in fn.body (the function's OWN top level)
    // count -- a do_async nested inside an if/while/switch/another
    // do_async is found by the recursive block()/stmt() walk below just
    // like Phase 2 always compiled it (an ordinary synchronous loop), NOT
    // by this scan, exactly matching Interpreter::runFunction's own
    // top-level-only scan of fn.body.
    std::vector<size_t> asyncIdx;
    for (size_t k = 0; k < fn.body.size(); ++k)
        if (fn.body[k]->kind == StmtKind::DoAsync)
            asyncIdx.push_back(k);

    if (asyncIdx.empty())
        return block(fn.body, fc, out, 1); // Phase 2 behavior, unchanged

    // switch(resumeAt<0 ? 0 : resumeAt+1): case 0 is "start fresh" (also
    // reached if asyncResumeFn_ names a DIFFERENT hook, or no do_async
    // statement exists at that ordinal anymore -- both collapse to
    // resumeAt=-1); case (ordinal+1) is that do_async's own resume point.
    // A switch (rather than an if/else-if chain or nested gotos) is the
    // natural fit here because "jump directly into the Nth segment,
    // skipping everything before it" is EXACTLY what switch/case dispatch
    // already does -- no manual jump table needed.
    const std::string resumeAtVar = freshTemp("resumeAt");
    out << ind(1) << "int " << resumeAtVar << " = (asyncResumeFn_ == "
        << cppStringLiteral(hookMethodName) << ") ? asyncResumeIndex_ : -1;\n";
    out << ind(1) << "switch (" << resumeAtVar << " < 0 ? 0 : " << resumeAtVar << " + 1) {\n";

    fc.push(); // one shared scope for the whole function body, matching how
              // block(fn.body, fc, out, 1) would have pushed exactly once
              // for the same statement list in the no-do_async case above.
    out << ind(1) << "case 0: {\n";
    bool ok = true;
    size_t ord = 0;
    for (size_t k = 0; k < fn.body.size(); ++k) {
        const Stmt& s = *fn.body[k];
        if (s.kind != StmtKind::DoAsync) {
            if (!stmt(s, fc, out, 1)) {
                ok = false;
                break;
            }
            continue;
        }

        // --- a TOP-LEVEL do_async: emit its resume-point case ----------
        std::string condExpr = expr(*s.cond, fc);
        if (!this->ok()) {
            ok = false;
            break;
        }
        out << ind(1) << "}\n"; // close the previous (still-open) case
        out << ind(1) << "[[fallthrough]];\n";
        out << ind(1) << "case " << (ord + 1) << ": {\n"; // this do_async's own resume point

        std::string brokeVar = freshTemp("broke");
        std::string endLabel = freshTemp("doAsyncEnd");
        out << ind(2) << "if ((" << condExpr << ").truthy()) {\n";
        out << ind(3) << "bool " << brokeVar << " = false;\n";
        out << ind(3) << "{\n";
        // Inside this body, Break sets brokeVar and jumps to endLabel
        // (aborting the do_async -- proceed to the NEXT segment in the
        // SAME call, no yield); Continue jumps to endLabel WITHOUT setting
        // it (ends this one-shot iteration exactly like falling off the
        // end of the body does -- both cases below just yield), precisely
        // matching Interpreter::runFunction's own
        // catch(BreakSignal){++asyncOrd;continue;} vs.
        // catch(ContinueSignal){} (falls through to the SAME yield code)
        // distinction.
        fc.breakTargets.push_back({endLabel, brokeVar});
        fc.continueTargets.push_back({endLabel});
        bool bodyOk = block(s.body, fc, out, 4);
        fc.continueTargets.pop_back();
        fc.breakTargets.pop_back();
        if (!bodyOk) {
            ok = false;
            break;
        }
        out << ind(3) << "}\n";
        out << ind(3) << endLabel << ":;\n";
        out << ind(3) << "if (!" << brokeVar << ") {\n";
        out << ind(4) << "asyncResumeFn_ = " << cppStringLiteral(hookMethodName) << ";\n";
        out << ind(4) << "asyncResumeIndex_ = " << ord << ";\n";
        out << ind(4) << "return crate::script::Value::Null_();\n";
        out << ind(3) << "}\n";
        // broke, OR cond was false to begin with -- fall through (still
        // inside this same case block) to whatever comes after this
        // do_async in the source, exactly like Interpreter::runFunction's
        // `++asyncOrd;` cond-false path and its BreakSignal-caught
        // `++asyncOrd; continue;` path both proceed to the next fn.body
        // index within the SAME call.
        out << ind(2) << "}\n";

        ++ord;
        // Leave this case's brace OPEN: subsequent regular statements
        // (the segment between this do_async and the next one, or the end
        // of the function) belong inside it, exactly as analyzed in
        // transpiration.txt -- only do_async statements themselves need
        // their own case label; the code between them does not.
    }
    out << ind(1) << "}\n"; // close whichever case was left open
    fc.pop();
    out << ind(1) << "}\n"; // close switch
    if (!ok)
        return false;

    // Not yielded (we fell all the way through, or the function returned
    // earlier via a plain `return` statement -- native return already
    // exits before reaching this point, exactly matching
    // Interpreter::runFunction's ReturnSignal short-circuiting the whole
    // frame-stepped loop): if THIS hook's name is still the resume target,
    // clear it -- mirrors Interpreter::call()'s
    // `else if (self_->asyncResumeFn == method) { clear(); }` (never
    // touches a DIFFERENT hook's suspension, matching the documented
    // single-resume-slot-per-object, whichever-hook-yielded-last-wins
    // behavior).
    out << ind(1) << "if (asyncResumeFn_ == " << cppStringLiteral(hookMethodName) << ") {\n";
    out << ind(2) << "asyncResumeFn_.clear();\n";
    out << ind(2) << "asyncResumeIndex_ = 0;\n";
    out << ind(1) << "}\n";
    return true;
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
            std::string contLabel = freshTemp("loopContinue");
            out << ind(indent) << "{\n";
            out << ind(indent + 1) << "int " << guard << " = 0;\n";
            std::string c = expr(*s.cond, fc);
            if (!ok())
                return false;
            out << ind(indent + 1) << "while ((" << c << ").truthy()) {\n";
            // Guard check FIRST, at the top of every iteration -- NOT after
            // the body -- so it always runs exactly once per iteration
            // regardless of whether the body falls off the end normally or
            // takes an early `goto` via a Continue statement (a native
            // `continue;`/post-body guard-check would be silently SKIPPED
            // by such a goto, since it jumps straight to contLabel: below,
            // bypassing any code between the goto and the label -- this
            // ordering was specifically chosen to avoid that, matching
            // Interpreter.cpp's own runFunction/execStmt guard placement,
            // which likewise counts every iteration attempt exactly once).
            out << ind(indent + 2) << "if (++" << guard << " > 1000000)\n";
            out << ind(indent + 3)
                << "throw crate::script::RuntimeError(\"loop exceeded 1,000,000 iterations\", "
                << s.line << ");\n";
            // break; targets this loop natively (a Switch nested inside
            // catches break itself, per Interpreter::execStmt's Switch
            // case, so break "through" a switch already works via C++'s
            // own nearest-enclosing-construct rule -- no goto needed here).
            // continue; CANNOT be native: if a Switch is nested inside this
            // loop's body, a native `continue;` written inside the switch's
            // own do{}while(false) wrapper would target THAT wrapper
            // instead of this loop (Switch does not catch ContinueSignal in
            // the interpreter, so continue must skip past it) -- so every
            // Continue always `goto`s contLabel, placed at the very end of
            // this loop's body, which is textually reachable from anywhere
            // inside it (including through any number of nested switches)
            // and falls straight through to the closing brace = the same
            // place a native continue; would have landed.
            fc.breakTargets.push_back({"", ""});
            fc.continueTargets.push_back({contLabel});
            bool okBody = block(s.body, fc, out, indent + 2);
            fc.continueTargets.pop_back();
            fc.breakTargets.pop_back();
            if (!okBody)
                return false;
            out << ind(indent + 2) << contLabel << ":;\n";
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
            // A cscript `break;` inside any case body below must target
            // THIS switch (native, matching Interpreter::execStmt's Switch
            // case, which catches BreakSignal itself) even if an
            // ENCLOSING do_async would otherwise want it to `goto` its own
            // end label -- push a native marker so Break's emission sees
            // this switch, not whatever's further out. continueTargets is
            // deliberately left untouched (see the big comment on
            // FnCtx::continueTargets / the DoWhile/DoAsync case above): a
            // `continue;` here must skip past this switch to whatever loop
            // actually encloses it.
            fc.breakTargets.push_back({"", ""});
            bool caseOk = true;
            for (const auto& c : s.cases) {
                if (!c.value) {
                    // default: always matches once control reaches it (i.e.
                    // nothing earlier in source order already matched and
                    // broken out) -- no condition, no temp needed.
                    out << ind(indent + 1) << "{\n";
                    if (!block(c.body, fc, out, indent + 2)) {
                        caseOk = false;
                        break;
                    }
                    out << ind(indent + 2) << "break;\n";
                    out << ind(indent + 1) << "}\n";
                    continue;
                }
                std::string cv = expr(*c.value, fc);
                if (!ok()) {
                    caseOk = false;
                    break;
                }
                std::string cvVar = freshTemp("case");
                out << ind(indent + 1) << "{\n";
                out << ind(indent + 2) << "crate::script::Value " << cvVar << " = (" << cv << ");\n";
                out << ind(indent + 2) << "if ((" << cvVar << ".str() == " << subjVar
                    << ".str()) || (" << cvVar << ".isNumeric() && " << subjVar
                    << ".isNumeric() && " << cvVar << ".num() == " << subjVar << ".num())) {\n";
                if (!block(c.body, fc, out, indent + 3)) {
                    caseOk = false;
                    break;
                }
                out << ind(indent + 3) << "break;\n";
                out << ind(indent + 2) << "}\n";
                out << ind(indent + 1) << "}\n";
            }
            fc.breakTargets.pop_back();
            if (!caseOk)
                return false;
            out << ind(indent) << "} while (false);\n";
            return true;
        }
        case StmtKind::Break: {
            if (fc.breakTargets.empty()) {
                fail("'break' outside of any loop", s.line);
                return false;
            }
            const auto& t = fc.breakTargets.back();
            if (t.gotoLabel.empty()) {
                out << ind(indent) << "break;\n";
            } else {
                if (!t.brokeFlagVar.empty())
                    out << ind(indent) << t.brokeFlagVar << " = true;\n";
                out << ind(indent) << "goto " << t.gotoLabel << ";\n";
            }
            return true;
        }
        case StmtKind::Continue: {
            if (fc.continueTargets.empty()) {
                fail("'continue' outside of any loop", s.line);
                return false;
            }
            out << ind(indent) << "goto " << fc.continueTargets.back().gotoLabel << ";\n";
            return true;
        }
    }
    fail("unsupported statement", s.line);
    return false;
}

} // namespace

std::string sanitizeClassName(const std::string& name) { return sanitize(name); }

CodeGenResult generateClass(const ClassInfo& classInfo,
                            const std::unordered_set<std::string>& knownClassNames) {
    CodeGenResult r;
    const ClassDecl& decl = *classInfo.decl;

    if (decl.isAbstract) {
        r.error = "abstract classes are not compiled to native code (they are never instantiated "
                  "directly)";
        return r;
    }
    // A script base (Phase 9f): real C++ inheritance, `class
    // Derived_Native : public Base_Native`. Not extended to static classes
    // (a static's `base` stays vestigial, as before Phase 9f -- the plan
    // never asked for static-class inheritance, and ClassInfo::
    // resolveBases() only ever resolves classInfo.baseClass to a non-null
    // ClassInfo when `base` actually names another known script class, so
    // an ordinary static class -- whose `base` defaults to "Actor", not a
    // script class -- is completely unaffected either way).
    const bool hasScriptBase = !decl.isStatic && classInfo.baseClass != nullptr;
    // A static class's `base` is vestigial (defaults to "Actor" even
    // though a static singleton is never instantiated as any kind of
    // Actor at all -- Interpreter::instantiate(ctx, cls, owner=nullptr)
    // never consults it) -- skip the base check entirely for one (Phase
    // 9e); a Component-shaped class needs either a real Actor/Actor2D/
    // Actor3D base OR a script base (Phase 9f).
    if (!decl.isStatic && !hasScriptBase && decl.base != "Actor" && decl.base != "Actor2D" &&
        decl.base != "Actor3D") {
        r.error = "class '" + decl.name + "' has base '" + decl.base +
                  "' -- '" + decl.base + "' is neither Actor/Actor2D/Actor3D nor a known script class";
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
    // Exported ABI symbol suffix uses the ORIGINAL cScript class name (not
    // the "_Native"-suffixed C++ type name), since NativeModule/
    // NativeClassRegistry (Phase 4) resolve exports by the script class
    // name the rest of the engine already knows it by (e.g. "Mover", not
    // "Mover_Native").
    const std::string exportSuffix = sanitize(decl.name);
    const std::string baseCls = hasScriptBase ? sanitize(classInfo.baseClass->name) + "_Native" : "";
    const std::string baseExportSuffix = hasScriptBase ? sanitize(classInfo.baseClass->name) : "";

    Gen gen(classInfo, knownClassNames);

    // Only override a lifecycle hook when the cScript class actually
    // declares it -- otherwise Component's own default no-op virtual
    // (Component.h: `virtual void start() {}` etc.) is inherited as-is,
    // exactly mirroring ScriptComponent::runHook()'s "missing hook is a
    // silent no-op, not an error" behavior. A static class has no such
    // virtual dispatch at all (see below) -- ScriptSystem's native-aware
    // startStatics/tickStatics/physicsStatics call fn_start/fn_update/
    // fn_physics_update directly, by name, through the SAME reflection
    // table every other method goes through, silently skipping a hook
    // that isn't in kMethods_<suffix> at all -- so hasStart/hasUpdate/
    // hasPhysicsUpdate only matter for the Component-shaped lifecycle
    // OVERRIDE declarations below, not for whether the hook works.
    const bool hasStart = gen.isOwnFunc("start");
    const bool hasUpdate = gen.isOwnFunc("update");
    const bool hasPhysicsUpdate = gen.isOwnFunc("physics_update");

    // ---- header ----
    // A static class's generated type does NOT inherit crate::Component at
    // all (Phase 9e) -- it's never instantiated as, or attached to, an
    // Actor; ScriptSystem's statics_ map ticks it directly by name via the
    // reflection table, exactly like the interpreted path already does via
    // Interpreter::instantiate(ctx, cls, owner=nullptr) + Interpreter::
    // call("start"/"update"/"physics_update"). `owner_` is still declared
    // (always nullptr, never assigned from a constructor parameter) purely
    // so every existing owner_-referencing codegen path (transform/actor
    // bare identifiers, this.actor, get_component, Camera.main) works
    // completely UNCHANGED for a static class too, with zero extra
    // special-casing anywhere else in this file -- exactly matching the
    // interpreter's own leniency (self_->owner is null for a static, and
    // reading e.g. `this.actor` there is a valid, if useless, Value, not
    // an error).
    std::ostringstream h;
    h << "#pragma once\n";
    h << "#include \"script/Runtime.h\"\n";
    h << "#include \"script/Interpreter.h\"\n";
    h << "#include \"scene/Actor.h\"\n";
    h << "#include \"scene/Component.h\"\n";
    h << "#include \"core/Math.h\"\n\n";
    if (hasScriptBase)
        // A plain quoted include resolves either way (Phase 9f full): for
        // a same-namespace base it's already sitting in the same gen dir
        // (no extra include path needed); for a cross-namespace one,
        // ScriptBuild.cpp's buildNamespace() adds -I<base's gen dir> to
        // this whole namespace's compile command.
        h << "#include \"" << baseCls << ".gen.h\"\n";
    h << "#include <memory>\n#include <string>\n#include <unordered_map>\n#include <vector>\n\n";
    // Cross-DLL export/import (Phase 9f full): every generated class is
    // POTENTIALLY a future cross-namespace base -- CodeGen has no way to
    // know in advance whether some OTHER namespace will end up deriving
    // from THIS one, so every class gets this guard unconditionally, and
    // ScriptBuild.cpp's buildNamespace() unconditionally passes
    // -DCRATE_GEN_BUILDING_<exportSuffix> when compiling THIS class's own
    // .cpp (so it always self-exports); a DIFFERENT namespace's .cpp that
    // #includes this header for inheritance does NOT define that macro,
    // so it sees __declspec(dllimport) instead. Needed for the class's
    // constructor (a derived class's ctor, in a different DLL, delegates
    // to it via the C++ member-init list) and any of its OWN methods an
    // inherited-but-not-overridden this->fn_X(...)/this.base.fn_X() call
    // could reach from a derived class compiled into another DLL --
    // dllexport on the class itself covers the whole vtable + every
    // member function uniformly, which is simpler and safer than trying
    // to annotate individual methods. A same-namespace-only base (or no
    // base at all) never actually crosses a DLL boundary, so the ordinary
    // intra-module extern trick (see kClassInfo_<suffix> below) still
    // does the rest of the work for THAT case; this macro is what makes
    // the SAME generated code ALSO correct when the base turns out to be
    // in a different DLL.
    h << "#if defined(CRATE_GEN_BUILDING_" << exportSuffix << ")\n";
    h << "#define CRATE_GEN_API_" << exportSuffix << " __declspec(dllexport)\n";
    h << "#else\n";
    h << "#define CRATE_GEN_API_" << exportSuffix << " __declspec(dllimport)\n";
    h << "#endif\n\n";
    h << "namespace crate::script::generated {\n\n";
    if (decl.isStatic) {
        h << "class CRATE_GEN_API_" << exportSuffix << " " << cls << " {\n";
        h << "public:\n";
        h << "    explicit " << cls << "(crate::script::ScriptContext* ctx);\n\n";
    } else {
        h << "class CRATE_GEN_API_" << exportSuffix << " " << cls << " : public "
          << (hasScriptBase ? baseCls : "crate::Component") << " {\n";
        h << "public:\n";
        h << "    " << cls << "(crate::script::ScriptContext* ctx, crate::Actor* owner);\n\n";
        h << "    const char* typeName() const override { return " << cppStringLiteral(decl.name)
          << "; }\n";
        if (hasStart)
            h << "    void start() override;\n";
        if (hasUpdate)
            h << "    void update(float dt) override;\n";
        if (hasPhysicsUpdate)
            h << "    void physicsUpdate(float dt) override;\n";
        h << "    std::unique_ptr<crate::Component> clone() const override {\n";
        h << "        return std::make_unique<" << cls << ">(ctx_, owner_);\n";
        h << "    }\n\n";
    }
    for (const auto& fn : decl.functions)
        h << "    crate::script::Value " << methodName(fn.name)
          << "(std::vector<crate::script::Value> args);\n";
    h << "\n";
    // Own fields only, and only the ones NOT already declared by an
    // ancestor (Phase 9f): an override field re-uses the INHERITED
    // storage (re-initialized by this class's own constructor, see
    // below), never a separate, shadowing member -- shadowing would give
    // this class's own methods a DIFFERENT field_X than the ancestor's
    // methods see, breaking the single-shared-value semantics
    // Interpreter::constructFields()'s flat fields[] map has.
    for (const auto& f : decl.fields)
        if (!gen.isOverrideField(f.name))
            h << "    crate::script::Value " << fieldMember(f.name) << ";\n";
    if (!hasScriptBase) {
        // Declared exactly ONCE per hierarchy, by the root-most class --
        // every derived class inherits these (Phase 9f) rather than
        // redeclaring them, so there's exactly one overflow map / one
        // do_async resume slot / one canonical selfView_ per OBJECT,
        // matching the single flat ScriptObject the interpreter uses
        // regardless of how deep the class hierarchy is.
        h << "    // Undeclared-field auto-vivification (mirrors Interpreter::lvalue()).\n";
        h << "    std::unordered_map<std::string, crate::script::Value> overflow_;\n";
        h << "    // do_async frame-stepped resumption state (Phase 6), mirrors\n";
        h << "    // crate::script::ScriptObject::asyncResumeFn/asyncResumeIndex: which\n";
        h << "    // top-level do_async of which hook is currently paused, if any.\n";
        h << "    std::string asyncResumeFn_;\n";
        h << "    int asyncResumeIndex_ = 0;\n";
        h << "    // The canonical ScriptObject wrapper for THIS instance (Phase 9d),\n";
        h << "    // constructed once (see the constructor) -- signals/first-class\n";
        h << "    // method references/get_component() ALL resolve to this SAME\n";
        h << "    // object, never a fresh one, so a connection made through one\n";
        h << "    // reference is visible to an emit through another.\n";
        h << "    std::shared_ptr<crate::script::ScriptObject> selfView_;\n\n";
    } else {
        h << "\n";
    }
    h << "private:\n";
    h << "    crate::script::ScriptContext* ctx_;\n";
    h << "    crate::Actor* owner_ = nullptr;\n";
    h << "};\n\n";
    h << "} // namespace crate::script::generated\n";
    r.header = h.str();

    // ---- source ----
    std::ostringstream c;
    c << "#include \"" << cls << ".gen.h\"\n";
    c << "#include \"script/CompiledClassInfo.h\"\n";
    c << "#include \"script/ObjectDispatch.h\"\n\n";

    // ---- reflection table + extern "C" factory ABI (transpiration.txt
    // Phase 4/5, signals + selfView added Phase 9d) ----
    // Emitted BEFORE the constructor (moved here from the end of the file,
    // Phase 9d) so the constructor can reference kClassInfo_<suffix>
    // directly when building selfView_ -- safe to move: every accessor
    // wrapper below only needs the generated class's DECLARATION (from the
    // header, already included above), never its out-of-line method
    // bodies, which come later in this same file. Deliberately minimal at
    // the boundary -- no STL types by value cross it via the three
    // dllexport functions themselves (CompiledClassInfo is always passed by
    // pointer); field/method accessor function pointers take `void*`
    // rather than the generated type, keeping this header-free reflection
    // surface usable from NativeScriptComponent.cpp without it ever needing
    // to know the per-class generated type either.
    c << "namespace {\n";
    for (const auto& f : decl.fields) {
        c << "crate::script::Value get_" << exportSuffix << "_" << sanitize(f.name)
          << "(void* p) { return static_cast<crate::script::generated::" << cls << "*>(p)->"
          << fieldMember(f.name) << "; }\n";
        c << "void set_" << exportSuffix << "_" << sanitize(f.name)
          << "(void* p, const crate::script::Value& v) { static_cast<crate::script::generated::"
          << cls << "*>(p)->" << fieldMember(f.name) << " = v; }\n";
    }
    if (!decl.fields.empty()) {
        c << "const crate::script::FieldAccessor kFields_" << exportSuffix << "[] = {\n";
        for (const auto& f : decl.fields)
            c << "    { " << cppStringLiteral(f.name) << ", " << cppStringLiteral(f.type)
              << ", &get_" << exportSuffix << "_" << sanitize(f.name) << ", &set_" << exportSuffix
              << "_" << sanitize(f.name) << " },\n";
        c << "};\n";
    }
    for (const auto& fn : decl.functions) {
        c << "crate::script::Value invoke_" << exportSuffix << "_" << sanitize(fn.name)
          << "(void* p, crate::script::ScriptContext*, std::vector<crate::script::Value> args) { "
             "return static_cast<crate::script::generated::"
          << cls << "*>(p)->" << methodName(fn.name) << "(std::move(args)); }\n";
    }
    if (!decl.functions.empty()) {
        c << "const crate::script::MethodAccessor kMethods_" << exportSuffix << "[] = {\n";
        for (const auto& fn : decl.functions)
            c << "    { " << cppStringLiteral(fn.name) << ", &invoke_" << exportSuffix << "_"
              << sanitize(fn.name) << " },\n";
        c << "};\n";
    }
    c << "std::unordered_map<std::string, crate::script::Value>& overflow_" << exportSuffix
      << "(void* p) { return static_cast<crate::script::generated::" << cls << "*>(p)->overflow_; }\n";
    if (!decl.signals.empty()) {
        c << "const char* const kSignals_" << exportSuffix << "[] = {\n";
        for (const auto& sig : decl.signals)
            c << "    " << cppStringLiteral(sig.name) << ",\n";
        c << "};\n";
    }
    c << "std::shared_ptr<crate::script::ScriptObject> selfView_" << exportSuffix
      << "(void* p) { return static_cast<crate::script::generated::" << cls << "*>(p)->selfView_; }\n";
    c << "} // namespace\n\n";
    // kClassInfo_<suffix> itself is deliberately declared OUTSIDE the
    // anonymous namespace above, with an explicit `extern` PLUS the same
    // CRATE_GEN_API_<suffix> dllexport/dllimport macro the class itself
    // uses (Phase 9f, full): each class compiles as its OWN translation
    // unit (a namespace's classes are compiled+linked together, not
    // merged into one .cpp), so a DERIVED class's TU needs to reference an
    // ANCESTOR's kClassInfo_ by address across that TU boundary for
    // baseClassInfo below -- impossible if it stayed inside an unnamed
    // namespace, since THOSE members always have internal (TU-local)
    // linkage no matter what, `extern` or not. A plain top-level `const`
    // also defaults to internal linkage in C++ (unlike C) unless marked
    // `extern` too, so both things (moving it out, AND keeping the
    // explicit extern) are required together. For a SAME-namespace
    // reference this `extern` alone is already sufficient (ordinary
    // intra-DLL link); for a CROSS-namespace one, `extern` alone is
    // necessary but NOT sufficient -- a plain top-level `extern` symbol,
    // even with external linkage, is still NOT visible to a genuinely
    // DIFFERENT DLL's linker unless it's ALSO explicitly marked
    // __declspec(dllexport) in the exporting DLL (Windows import
    // libraries only contain symbols explicitly marked for export, unlike
    // Unix shared libraries) -- caught by the FIRST real cross-namespace
    // build attempt (LNK2001 unresolved external), which is exactly why
    // this data symbol gets the SAME per-class macro the class itself
    // does: dllexport when compiled as part of its OWN namespace,
    // dllimport when referenced from a different one.
    if (hasScriptBase)
        c << "extern CRATE_GEN_API_" << baseExportSuffix << " const crate::script::CompiledClassInfo kClassInfo_"
          << baseExportSuffix << ";\n";
    c << "extern CRATE_GEN_API_" << exportSuffix << " const crate::script::CompiledClassInfo kClassInfo_"
      << exportSuffix << " = {\n";
    c << "    " << cppStringLiteral(decl.name) << ", \"\", "
      << (hasScriptBase ? ("&kClassInfo_" + baseExportSuffix) : "nullptr") << ",\n";
    c << "    " << (decl.fields.empty() ? "nullptr" : ("kFields_" + exportSuffix)) << ", "
      << decl.fields.size() << ",\n";
    c << "    " << (decl.functions.empty() ? "nullptr" : ("kMethods_" + exportSuffix)) << ", "
      << decl.functions.size() << ",\n";
    c << "    &overflow_" << exportSuffix << ",\n";
    c << "    " << (decl.signals.empty() ? "nullptr" : ("kSignals_" + exportSuffix)) << ", "
      << decl.signals.size() << ",\n";
    c << "    &selfView_" << exportSuffix << "\n";
    c << "};\n\n";

    c << "namespace crate::script::generated {\n\n";
    if (decl.isStatic) {
        c << cls << "::" << cls << "(crate::script::ScriptContext* ctx)\n";
        c << "    : ctx_(ctx) {\n";
        c << "    selfView_ = std::make_shared<crate::script::ScriptObject>();\n";
        c << "    selfView_->nativePtr = this;\n";
        c << "    selfView_->compiledInfo = &kClassInfo_" << exportSuffix << ";\n";
        c << "    selfView_->owner = owner_;\n";
    } else if (hasScriptBase) {
        // Phase 9f: delegates to the base's own constructor (member-init
        // list), which -- since `this` inside a base subobject's
        // constructor, while constructing a DERIVED instance, already
        // refers to the FULL derived object -- has already constructed
        // selfView_ with the correct nativePtr/owner AND initialized every
        // ancestor's own fields to their defaults (base-to-derived chain,
        // exactly matching Interpreter::constructFields()'s own walk
        // order). Only selfView_->compiledInfo needs correcting here, from
        // whatever the base ctor set it to (the BASE's own kClassInfo) to
        // THIS class's -- so signal/method/get_component lookups on this
        // instance see its ACTUAL most-derived type, not an ancestor's.
        c << cls << "::" << cls << "(crate::script::ScriptContext* ctx, crate::Actor* owner)\n";
        c << "    : crate::script::generated::" << baseCls << "(ctx, owner), ctx_(ctx), owner_(owner) {\n";
        c << "    selfView_->compiledInfo = &kClassInfo_" << exportSuffix << ";\n";
    } else {
        c << cls << "::" << cls << "(crate::script::ScriptContext* ctx, crate::Actor* owner)\n";
        c << "    : ctx_(ctx), owner_(owner) {\n";
        c << "    selfView_ = std::make_shared<crate::script::ScriptObject>();\n";
        c << "    selfView_->nativePtr = this;\n";
        c << "    selfView_->compiledInfo = &kClassInfo_" << exportSuffix << ";\n";
        c << "    selfView_->owner = owner_;\n";
    }
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

    // Only start/update/physics_update ever get resumable (frame-stepped)
    // do_async treatment -- exactly matching Interpreter::call()'s own
    // restriction, since that is the ONLY entry point that sets
    // resumable=true when invoking runFunction(); every other function
    // (reached via this.foo()/bare foo(), never a hook) always goes
    // through callMethodOn's non-resumable runFunction() call, so its
    // do_asyncs -- even ones at ITS OWN top level -- are plain synchronous
    // loops via block()/stmt(), completely unaffected by Phase 6.
    static const std::unordered_set<std::string> kResumableHooks = {"start", "update",
                                                                     "physics_update"};
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
        bool bodyOk = kResumableHooks.count(fn.name)
                         ? gen.emitHookBody(fn, fc, fn.name, body)
                         : gen.block(fn.body, fc, body, 1);
        if (!bodyOk) {
            r.error = gen.error();
            return r;
        }
        c << body.str();
        c << "    return crate::script::Value::Null_();\n";
        c << "}\n\n";
    }

    // Lifecycle overrides (Component-shaped classes only): each forwards to
    // its own fn_<hook>() and swallows any RuntimeError/exception
    // (reporting via ctx_->warn) rather than letting it escape into engine
    // frame-loop code -- mirroring ScriptComponent::runHook()'s try/catch,
    // which exists specifically so one broken script can't crash the whole
    // engine. Unlike runHook(), this does not latch a persistent error
    // state that suppresses future calls (that richer UX is Phase 5's
    // NativeScriptComponent's job); every frame gets a fresh attempt. A
    // static class has no Component to override virtuals on at all -- its
    // start/update/physics_update (if declared) are just ordinary fn_<hook>
    // methods already emitted above, reached by ScriptSystem's native-aware
    // statics ticking through the SAME kMethods_<suffix> reflection table
    // every other method uses (with ITS OWN try/catch around the call,
    // mirroring the interpreted statics path's existing try/catch --
    // see ScriptSystem.cpp).
    if (!decl.isStatic) {
        auto emitHook = [&](const char* cppName, const char* scriptMethod, bool hasDt) {
            c << "void " << cls << "::" << cppName << "(" << (hasDt ? "float dt" : "") << ") {\n";
            c << "    try {\n";
            c << "        " << methodName(scriptMethod) << "(std::vector<crate::script::Value>{";
            if (hasDt)
                c << "crate::script::Value::Float((double)dt)";
            c << "});\n";
            c << "    } catch (const std::exception& ex) {\n";
            c << "        if (ctx_ && ctx_->warn) ctx_->warn(std::string(" << cppStringLiteral(decl.name)
              << ") + \".\" + " << cppStringLiteral(scriptMethod) << " + \": \" + ex.what());\n";
            c << "    }\n";
            c << "}\n\n";
        };
        if (hasStart)
            emitHook("start", "start", false);
        if (hasUpdate)
            emitHook("update", "update", true);
        if (hasPhysicsUpdate)
            emitHook("physicsUpdate", "physics_update", true);
    }

    c << "} // namespace crate::script::generated\n\n";

    // ---- extern "C" factory ABI (transpiration.txt Phase 4/5, static
    // variant added Phase 9e) ----
    // The reflection table itself was moved above the constructor (Phase
    // 9d) -- these functions can stay here at the end regardless, since
    // they only need crate::script::generated::<cls>'s DECLARATION (from
    // the header) and kClassInfo_<suffix>'s definition (above both), not
    // any particular emission order relative to the method bodies. A
    // static class exports CreateStatic_<suffix>/DestroyStatic_<suffix>
    // (returning/taking a bare void*, no Actor* -- there is none) instead
    // of CreateInstance_<suffix>/DestroyInstance_<suffix>, so a class can
    // never be accidentally instantiated the wrong way; NativeModule.cpp
    // resolves whichever pair is present.
    if (decl.isStatic) {
        c << "extern \"C\" __declspec(dllexport) void* CreateStatic_" << exportSuffix
          << "(crate::script::ScriptContext* ctx) {\n";
        c << "    return new crate::script::generated::" << cls << "(ctx);\n";
        c << "}\n\n";
        c << "extern \"C\" __declspec(dllexport) void DestroyStatic_" << exportSuffix
          << "(void* instance) {\n";
        c << "    delete static_cast<crate::script::generated::" << cls << "*>(instance);\n";
        c << "}\n\n";
    } else {
        c << "extern \"C\" __declspec(dllexport) crate::Component* CreateInstance_" << exportSuffix
          << "(crate::script::ScriptContext* ctx, crate::Actor* owner) {\n";
        c << "    return new crate::script::generated::" << cls << "(ctx, owner);\n";
        c << "}\n\n";
        c << "extern \"C\" __declspec(dllexport) void DestroyInstance_" << exportSuffix
          << "(crate::Component* instance) {\n";
        c << "    delete instance;\n";
        c << "}\n\n";
    }
    c << "extern \"C\" __declspec(dllexport) const crate::script::CompiledClassInfo* GetClassInfo_"
      << exportSuffix << "() {\n";
    c << "    return &kClassInfo_" << exportSuffix << ";\n";
    c << "}\n";
    r.source = c.str();

    r.ok = true;
    return r;
}

} // namespace crate::script
