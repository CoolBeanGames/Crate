#include "script/Interpreter.h"

#include "core/Math.h"
#include "script/CompiledClassInfo.h"
#include "script/ObjectDispatch.h"
#include "script/Runtime.h"
#include "scene/Actor.h"
#include "scene/BuiltinComponents.h"

#include <algorithm>

namespace crate::script {

Interpreter::Interpreter(ScriptContext* ctx, std::shared_ptr<ScriptObject> self)
    : ctx_(ctx), self_(std::move(self)) {
    dispatchClass_ = self_ ? self_->cls : nullptr;
}

std::shared_ptr<ScriptObject> Interpreter::instantiate(ScriptContext* ctx, const ClassInfo* cls,
                                                       crate::Actor* owner) {
    auto obj = std::make_shared<ScriptObject>();
    obj->cls = cls;
    obj->owner = owner;
    Interpreter interp(ctx, obj);
    interp.constructFields();
    return obj;
}

void Interpreter::constructFields() {
    if (!self_ || !self_->cls || !self_->cls->decl)
        return;
    // Base fields first, then derived (so overrides win).
    std::vector<const ClassInfo*> chain;
    for (const ClassInfo* c = self_->cls; c; c = c->baseClass)
        chain.push_back(c);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        for (const auto& fd : (*it)->decl->fields) {
            Value v = fd.init ? eval(*fd.init) : Value::Null_();
            self_->fields[fd.name] = coerce(std::move(v), fd.type);
        }
    }
}

bool Interpreter::hasMethod(const std::string& name) const {
    return self_ && self_->cls && self_->cls->findFunction(name) != nullptr;
}

Value Interpreter::call(const std::string& method, std::vector<Value> args, bool required) {
    if (!self_ || !self_->cls) {
        if (required)
            throw RuntimeError("no script class for method '" + method + "'", 0);
        return Value::Null_();
    }
    const ClassInfo* definedIn = nullptr;
    const FunctionDecl* fn = self_->cls->findFunction(method, &definedIn);
    if (!fn || fn->isAbstract) {
        if (required)
            throw RuntimeError("script has no '" + method + "' function", 0);
        return Value::Null_();
    }

    resumable_ = true;
    resumeIndex_ = (self_->asyncResumeFn == method) ? self_->asyncResumeIndex : -1;
    yielded_ = false;
    Value r = runFunction(*fn, definedIn, args, /*resumable=*/true);
    if (yielded_) {
        self_->asyncResumeFn = method;
        self_->asyncResumeIndex = yieldIndex_;
    } else if (self_->asyncResumeFn == method) {
        self_->asyncResumeFn.clear();
        self_->asyncResumeIndex = 0;
    }
    return r;
}

Value Interpreter::runFunction(const FunctionDecl& fn, const ClassInfo* definedIn,
                               std::vector<Value>& args, bool resumable) {
    const ClassInfo* prevDispatch = dispatchClass_;
    dispatchClass_ = definedIn ? definedIn : dispatchClass_;
    scopes_.push_back({});
    for (size_t k = 0; k < fn.params.size(); ++k) {
        Value a = k < args.size() ? args[k] : Value::Null_();
        scopes_.back().vars[fn.params[k].name] = coerce(std::move(a), fn.params[k].type);
    }

    Value result = Value::Null_();

    // Frame-stepped path: walk the top-level statement list so a `do_async`
    // can run one body iteration and suspend the whole method until next frame.
    if (resumable) {
        int resumeAt = resumeIndex_;
        int startStmt = 0;
        if (resumeAt >= 0) {
            int ord = 0;
            for (size_t k = 0; k < fn.body.size(); ++k) {
                if (fn.body[k]->kind == StmtKind::DoAsync) {
                    if (ord == resumeAt) {
                        startStmt = (int)k;
                        break;
                    }
                    ++ord;
                }
            }
        }
        int asyncOrd = 0;
        for (int k = 0; k < startStmt; ++k)
            if (fn.body[k]->kind == StmtKind::DoAsync)
                ++asyncOrd;

        try {
            for (size_t k = (size_t)startStmt; k < fn.body.size(); ++k) {
                const Stmt& s = *fn.body[k];
                if (s.kind == StmtKind::DoAsync) {
                    if (eval(*s.cond).truthy()) {
                        try {
                            execBlock(s.body);
                        } catch (BreakSignal&) {
                            ++asyncOrd; // loop aborted; move past it
                            continue;
                        } catch (ContinueSignal&) {
                        }
                        yielded_ = true;
                        yieldIndex_ = asyncOrd;
                        break;
                    }
                    ++asyncOrd; // cond false -> loop done, fall through
                } else {
                    execStmt(s);
                }
            }
        } catch (ReturnSignal& r) {
            result = std::move(r.value);
        }
        scopes_.pop_back();
        dispatchClass_ = prevDispatch;
        return result;
    }

    try {
        execBlock(fn.body);
    } catch (ReturnSignal& r) {
        result = std::move(r.value);
    }
    scopes_.pop_back();
    dispatchClass_ = prevDispatch;
    return result;
}

void Interpreter::execBlock(const std::vector<StmtPtr>& body) {
    scopes_.push_back({});
    struct Pop {
        std::vector<Scope>* s;
        ~Pop() { s->pop_back(); }
    } pop{&scopes_};
    for (const auto& st : body)
        execStmt(*st);
}

void Interpreter::execStmt(const Stmt& s) {
    switch (s.kind) {
        case StmtKind::Block:
            execBlock(s.body);
            return;
        case StmtKind::VarDecl: {
            Value v = s.init ? eval(*s.init) : Value::Null_();
            scopes_.back().vars[s.name] = coerce(std::move(v), s.declType);
            return;
        }
        case StmtKind::ExprStmt:
            eval(*s.expr);
            return;
        case StmtKind::Return:
            throw ReturnSignal{s.expr ? eval(*s.expr) : Value::Null_()};
        case StmtKind::If: {
            if (eval(*s.cond).truthy())
                execBlock(s.thenBody);
            else if (!s.elseBody.empty())
                execBlock(s.elseBody);
            return;
        }
        case StmtKind::DoWhile:
        case StmtKind::DoAsync: {
            // do_async proper (one iteration per frame) lands with the
            // "processing" task; for now both run synchronously with a guard.
            int guard = 0;
            while (eval(*s.cond).truthy()) {
                try {
                    execBlock(s.body);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                }
                if (++guard > 1'000'000)
                    throw RuntimeError("loop exceeded 1,000,000 iterations", s.line);
            }
            return;
        }
        case StmtKind::Switch: {
            Value subj = eval(*s.subject);
            bool matched = false;
            for (const auto& c : s.cases) {
                if (!matched) {
                    if (!c.value) {
                        matched = true; // default
                    } else {
                        Value cv = eval(*c.value);
                        matched = (cv.str() == subj.str()) ||
                                  (cv.isNumeric() && subj.isNumeric() && cv.num() == subj.num());
                    }
                }
                if (matched) {
                    try {
                        for (const auto& st : c.body)
                            execStmt(*st);
                    } catch (BreakSignal&) {
                        return;
                    }
                    return; // cScript cases do not fall through
                }
            }
            return;
        }
        case StmtKind::Break:
            throw BreakSignal{};
        case StmtKind::Continue:
            throw ContinueSignal{};
    }
}

Value* Interpreter::findVar(const std::string& name) {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto f = it->vars.find(name);
        if (f != it->vars.end())
            return &f->second;
    }
    return nullptr;
}

Value Interpreter::eval(const Expr& e) {
    switch (e.kind) {
        case ExprKind::IntLit: return Value::Int(e.intVal);
        case ExprKind::FloatLit: return Value::Float(e.floatVal);
        case ExprKind::StringLit: return Value::Str(e.strVal);
        case ExprKind::CharLit: return Value::Char(e.strVal.empty() ? '\0' : e.strVal[0]);
        case ExprKind::BoolLit: return Value::Bool(e.boolVal);
        case ExprKind::NullLit: return Value::Null_();
        case ExprKind::This: return self_ ? Value::Obj(self_) : Value::Null_();
        case ExprKind::Base: return self_ ? Value::Obj(self_) : Value::Null_(); // used via Member
        case ExprKind::ArrayLit: {
            std::vector<Value> items;
            for (const auto& it : e.items)
                items.push_back(eval(*it));
            return Value::Arr(std::move(items));
        }
        case ExprKind::Identifier: {
            if (Value* v = findVar(e.strVal))
                return *v;
            if (self_ && self_->fields.count(e.strVal))
                return self_->fields[e.strVal];
            // Every script inherits Actor, so `transform` (and `actor`) are
            // available without `this.` -- transform exposes position /
            // rotation / scale.
            if ((e.strVal == "transform" || e.strVal == "actor") && self_ && self_->owner)
                return Value::ActorRef(self_->owner);
            // a bare type name evaluates to a TypeRef
            if (ctx_->findType(e.strVal) || e.strVal == "Actor" || e.strVal == "Actor2D" ||
                e.strVal == "Actor3D" || e.strVal == "Vector2" || e.strVal == "Vector3" ||
                e.strVal == "Fog" || e.strVal == "Camera" || e.strVal == "Transform" ||
                e.strVal == "Light" || e.strVal == "VolumetricFog" || e.strVal == "MeshRenderer" ||
                e.strVal == "LightProbe" || e.strVal == "Spinner")
                return Value::Type(e.strVal);
            // a bare signal name on `this`.
            if (self_ && self_->cls && self_->cls->hasSignal(e.strVal))
                return Value::SignalRef(self_, e.strVal);
            // a bare method name is a Callable bound to `this` (for signal
            // .connect(my_func) and callback passing, Godot-style).
            if (self_ && self_->cls && self_->cls->findFunction(e.strVal))
                return Value::Fn(self_, e.strVal);
            throw RuntimeError("unknown identifier '" + e.strVal + "'", e.line);
        }
        case ExprKind::Unary: {
            Value a = eval(*e.a);
            if (e.op == Tok::Minus)
                return crate::script::negate(a);
            if (e.op == Tok::Not)
                return Value::Bool(!a.truthy());
            return a;
        }
        case ExprKind::Binary: return evalBinary(e);
        case ExprKind::Assign: {
            Value v = eval(*e.b);
            if (e.op != Tok::Unknown) // compound: a op= b  ->  a = a op b
                v = arith(e.op, eval(*e.a), v, e.line);
            assign(*e.a, v);
            return v;
        }
        case ExprKind::Call: return evalCall(e);
        case ExprKind::Member: return evalMember(e);
        case ExprKind::Index: {
            Value base = eval(*e.a);
            Value idx = eval(*e.b);
            if (base.t == Value::T::Array && base.arr) {
                long long k = (long long)idx.num();
                if (k < 0 || k >= (long long)base.arr->size())
                    throw RuntimeError("array index out of range", e.line);
                return (*base.arr)[k];
            }
            throw RuntimeError("cannot index a " + std::string(base.typeName()), e.line);
        }
    }
    return Value::Null_();
}

Value Interpreter::evalBinary(const Expr& e) {
    if (e.op == Tok::AndAnd)
        return Value::Bool(eval(*e.a).truthy() && eval(*e.b).truthy());
    if (e.op == Tok::OrOr)
        return Value::Bool(eval(*e.a).truthy() || eval(*e.b).truthy());

    Value a = eval(*e.a);
    Value b = eval(*e.b);

    switch (e.op) {
        case Tok::EqEq: return crate::script::valueEquals(a, b);
        case Tok::NotEq: return crate::script::valueNotEquals(a, b);
        case Tok::Lt: return crate::script::valueLess(a, b);
        case Tok::Gt: return crate::script::valueGreater(a, b);
        case Tok::LtEq: return crate::script::valueLessEq(a, b);
        case Tok::GtEq: return crate::script::valueGreaterEq(a, b);
        default: break;
    }

    return arith(e.op, a, b, e.line);
}

Value Interpreter::arith(Tok op, const Value& a, const Value& b, int line) {
    // Thin wrapper: the actual logic is stateless (no dependency on self_/
    // ctx_/scopes_) and lives in Runtime.h/.cpp so generated native code can
    // call the identical implementation. See transpiration.txt Phase 0.
    return crate::script::arith(op, a, b, line);
}

Value Interpreter::actorMember(crate::Actor* a, const std::string& name, int line) {
    // Thin wrapper: stateless, moved to Runtime.h/.cpp (Phase 9b) so
    // generated native code shares the identical implementation for a
    // general Actor-typed value, not just the statically-known bare
    // transform/actor identifiers.
    return crate::script::actorMember(a, name, line);
}

Value Interpreter::evalMember(const Expr& e) {
    const std::string& name = e.strVal;

    // Input.Mouse.<member> -- structural, like Input.<method>(...) in
    // evalCall: "Input" has no meaningful standalone Value (it's pure
    // namespace syntax), so this must be recognized on the AST directly,
    // before eval(*e.a) below would throw "unknown identifier 'Input'".
    if (e.a->kind == ExprKind::Member && e.a->strVal == "Mouse" &&
        e.a->a->kind == ExprKind::Identifier && e.a->a->strVal == "Input" && !findVar("Input"))
        return crate::script::inputMouseMember(ctx_, name, e.line);

    // this.base.method(...)  -> handled in evalCall; a bare this.base is just this.
    Value obj = eval(*e.a);

    if (obj.t == Value::T::Object && obj.obj) {
        // Generic dispatch: correctly handles an interpreted script
        // instance, a compiled script instance live view, a
        // BuiltinComponent live view (Fog/Camera), or an InputButton's
        // synthesized just_pressed/just_released/pressed signals,
        // uniformly -- see ObjectDispatch.h / transpiration.txt Phase
        // 9a/9d.
        return crate::script::getObjectMember(obj.obj, name, e.line);
    }
    if (obj.t == Value::T::Actor)
        return actorMember(obj.actor, name, e.line);
    if (obj.t == Value::T::Array && name == "length")
        return crate::script::arrayLength(obj);
    if (obj.t == Value::T::TypeRef && obj.s == "Camera" && name == "main") {
        CameraComponent* cc = mainCameraFrom(self_ ? self_->owner : nullptr);
        if (!cc)
            return Value::Null_();
        auto o = std::make_shared<ScriptObject>();
        o->builtin = "Camera";
        o->nativePtr = cc;
        o->owner = cc->actor();
        return Value::Obj(o);
    }
    if (obj.t == Value::T::TypeRef && ctx_->getStatic) {
        // Kind-agnostic (Phase 9e): a static's singleton may now be a
        // COMPILED instance (Kind 3), whose data lives behind its
        // CompiledClassInfo accessor table, never in `fields` at all --
        // the old direct `so->fields.find(name)` only ever worked for an
        // interpreted static, silently missing every read on a native one.
        if (auto so = ctx_->getStatic(obj.s))
            return crate::script::getObjectMember(so, name, e.line);
    }

    throw RuntimeError("cannot read '." + name + "' on " + obj.typeName(), e.line);
}

Value Interpreter::callMethodOn(std::shared_ptr<ScriptObject> obj, const std::string& method,
                                std::vector<Value> args, int line, bool viaBase) {
    if (!obj || !obj->cls)
        throw RuntimeError("cannot call '" + method + "' on a non-script value", line);
    const ClassInfo* start = viaBase ? obj->cls->baseClass : obj->cls;
    if (!start)
        throw RuntimeError("no base class for '" + method + "'", line);
    const ClassInfo* definedIn = nullptr;
    const FunctionDecl* fn = start->findFunction(method, &definedIn);
    if (!fn)
        throw RuntimeError("method '" + method + "' not found", line);
    if (fn->isAbstract)
        throw RuntimeError("abstract function '" + method + "' has no override", line);
    Interpreter sub(ctx_, obj);
    return sub.runFunction(*fn, definedIn, args);
}

Value Interpreter::evalCall(const Expr& e) {
    const Expr& callee = *e.a;
    std::vector<Value> args;
    args.reserve(e.items.size());
    for (const auto& a : e.items)
        args.push_back(eval(*a));

    // Global function:  print(...), type_of(...), Vector3(...), Vector2(...)
    if (callee.kind == ExprKind::Identifier) {
        // A local/field holding a Callable can be invoked directly: cb(args).
        if (Value* v = findVar(callee.strVal))
            if (v->t == Value::T::Callable)
                return invokeCallable(*v, std::move(args), e.line);
        if (self_) {
            auto fit = self_->fields.find(callee.strVal);
            if (fit != self_->fields.end() && fit->second.t == Value::T::Callable)
                return invokeCallable(fit->second, std::move(args), e.line);
        }
        return builtinCall(callee.strVal, args, e.line);
    }

    // Method call: obj.method(args)
    if (callee.kind == ExprKind::Member) {
        const Expr& objExpr = *callee.a;
        const std::string& method = callee.strVal;

        // this.base.method(...)
        if (objExpr.kind == ExprKind::Member && objExpr.strVal == "base" &&
            objExpr.a->kind == ExprKind::This) {
            return callMethodOn(self_, method, std::move(args), e.line, /*viaBase=*/true);
        }

        // Global helper namespace: Math.clamp(...), Math.lerp(...), etc.
        if (objExpr.kind == ExprKind::Identifier && objExpr.strVal == "Math" &&
            !findVar("Math") && !(self_ && self_->fields.count("Math")))
            return mathCall(method, args, e.line);

        // Global Input namespace.
        if (objExpr.kind == ExprKind::Identifier && objExpr.strVal == "Input" && !findVar("Input")) {
            std::string n = args.empty() ? std::string() : args[0].str();
            if (method == "get_button")
                return ctx_->inputButton ? Value::Obj(ctx_->inputButton(n)) : Value::Null_();
            if (method == "get_axis") {
                double x = ctx_->inputQuery ? ctx_->inputQuery(n, 3) : 0.0;
                double y = ctx_->inputQuery ? ctx_->inputQuery(n, 4) : 0.0;
                return makeVector("Vector2", x, y, 0);
            }
            // Mouse buttons ride the same is_pressed/is_just_pressed/
            // is_just_released/get_button API above under reserved names:
            // "mouse_left" / "mouse_right" / "mouse_middle" (see Input::
            // pollMouse). Delta/scroll/button-state convenience live at
            // Input.Mouse.<member> instead (see evalMember).
            if (method == "is_pressed")
                return Value::Bool(ctx_->inputQuery && ctx_->inputQuery(n, 0) != 0.0);
            if (method == "is_just_pressed")
                return Value::Bool(ctx_->inputQuery && ctx_->inputQuery(n, 1) != 0.0);
            if (method == "is_just_released")
                return Value::Bool(ctx_->inputQuery && ctx_->inputQuery(n, 2) != 0.0);
            throw RuntimeError("Input has no method '" + method + "'", e.line);
        }

        Value obj = eval(objExpr);

        // Signal.connect / emit / disconnect / is_connected  (Godot-style).
        if (obj.t == Value::T::Signal)
            return signalCall(obj, method, std::move(args), e.line);

        // callable.call(args)
        if (obj.t == Value::T::Callable && (method == "call" || method == "emit"))
            return invokeCallable(obj, std::move(args), e.line);

        // StaticClass.method(...)  -> call on the static singleton.
        // Kind-agnostic (Phase 9e) via callObjectMethod -- the old direct
        // callMethodOn() only ever worked for an interpreted static (it
        // throws immediately for a Kind-3 object, whose `cls` is always
        // null).
        if (obj.t == Value::T::TypeRef && ctx_->getStatic) {
            if (auto so = ctx_->getStatic(obj.s))
                return crate::script::callObjectMethod(ctx_, so, method, std::move(args), e.line);
        }

        if (obj.t == Value::T::Object && obj.obj &&
            (obj.obj->cls || (obj.obj->nativePtr && obj.obj->compiledInfo))) {
            // Generic dispatch: handles "does the object declare this
            // method itself" (kind-agnostic), the get_component/Godot-3
            // signal API shortcuts (emit_signal/connect/disconnect/
            // is_connected) for when it doesn't, and the actual call
            // (interpreted target via callMethodOn, compiled target via its
            // reflection table's method-invoke function pointer) -- ALL
            // consolidated into callObjectMethod itself (Phase 9d), so
            // generated code's identical dispatch (callValueMethod) can't
            // drift from the interpreter's. See ObjectDispatch.h.
            return crate::script::callObjectMethod(ctx_, obj.obj, method, std::move(args), e.line);
        }

        // native Actor methods
        if (obj.t == Value::T::Actor) {
            if (method == "get_component") {
                std::string typeName;
                if (!args.empty())
                    typeName = args[0].t == Value::T::TypeRef ? args[0].s : args[0].str();
                if (ctx_->getComponent)
                    if (auto so = ctx_->getComponent(obj.actor, typeName))
                        return Value::Obj(so);
                return Value::Null_();
            }
            throw RuntimeError("Actor has no method '" + method + "'", e.line);
        }
        if (obj.t == Value::T::Array) {
            if (method == "add" && !args.empty() && crate::script::arrayAdd(obj, args[0]))
                return Value::Null_();
            if (method == "length" && obj.arr)
                return crate::script::arrayLength(obj);
        }
        // universal .str()
        if (method == "str")
            return Value::Str(obj.str());

        throw RuntimeError("no method '" + method + "'", e.line);
    }

    throw RuntimeError("expression is not callable", e.line);
}

// Thin wrappers: signalCall/emitSignal/invokeCallable (and the
// sameCallable() helper they used) were already stateless (no self_/
// scopes_/dispatchClass_ dependency, only ctx_ -- which the free-function
// forms now take explicitly), moved to ObjectDispatch.h/.cpp (Phase 9d) so
// generated native code shares the identical implementation rather than a
// second copy -- exactly the same extraction pattern already used for
// actorMember (Runtime.cpp, Phase 9b) and mathCall (Runtime.cpp, Phase 0).
Value Interpreter::invokeCallable(const Value& fn, std::vector<Value> args, int line) {
    return crate::script::invokeCallable(ctx_, fn, std::move(args), line);
}

void Interpreter::emitSignal(const std::shared_ptr<ScriptObject>& owner, const std::string& name,
                             std::vector<Value> args, int line) {
    crate::script::emitSignal(ctx_, owner, name, std::move(args), line);
}

Value Interpreter::signalCall(const Value& sig, const std::string& method, std::vector<Value> args,
                              int line) {
    return crate::script::signalCall(ctx_, sig, method, std::move(args), line);
}

Value Interpreter::mathCall(const std::string& fn, std::vector<Value>& args, int line) {
    // Thin wrapper: stateless (no self_/ctx_/scopes_ dependency), moved to
    // Runtime.h/.cpp so generated native code shares the same
    // implementation. See transpiration.txt Phase 0/2.
    return crate::script::mathCall(fn, args, line);
}

Value Interpreter::builtinCall(const std::string& name, std::vector<Value>& args, int line) {
    if (name == "print") {
        std::string out;
        for (size_t k = 0; k < args.size(); ++k) {
            if (k)
                out += " ";
            out += args[k].str();
        }
        if (ctx_->print)
            ctx_->print(out);
        return Value::Null_();
    }
    if (name == "type_of") {
        if (args.empty())
            return Value::Type("");
        if (args[0].t == Value::T::TypeRef)
            return args[0];
        return Value::Type(args[0].typeName());
    }
    // A bare global (like Camera.main): the scene it reaches into is found
    // by walking up from the CALLING script's own actor, not an explicit
    // receiver (Scenes task -- see zen.tasks.json's "Scenes" card).
    if (name == "get_root") {
        crate::Actor* root = crate::script::sceneRootOf(self_ ? self_->owner : nullptr);
        return root ? Value::ActorRef(root) : Value::Null_();
    }
    // Godot-3 style: emit_signal("name", args...) on `this`.
    if (name == "emit_signal") {
        if (args.empty())
            throw RuntimeError("emit_signal needs a signal name", line);
        std::string sn = args[0].str();
        emitSignal(self_, sn, std::vector<Value>(args.begin() + 1, args.end()), line);
        return Value::Null_();
    }
    if (name == "Vector3" || name == "Vector2") {
        double x = args.size() > 0 ? args[0].num() : 0;
        double y = args.size() > 1 ? args[1].num() : 0;
        double z = args.size() > 2 ? args[2].num() : 0;
        return makeVector(name, x, y, z);
    }
    if (name == "str" && args.size() == 1)
        return Value::Str(args[0].str());

    // A call to a method of this class without an explicit `this.`
    if (self_ && self_->cls && self_->cls->findFunction(name))
        return callMethodOn(self_, name, args, line, false);

    throw RuntimeError("unknown function '" + name + "'", line);
}

Value* Interpreter::lvalue(const Expr& e) {
    if (e.kind == ExprKind::Identifier) {
        if (Value* v = findVar(e.strVal))
            return v;
        if (self_) {
            return &self_->fields[e.strVal]; // implicit field / new local-on-object
        }
    }
    if (e.kind == ExprKind::Member) {
        Value obj = eval(*e.a);
        if (obj.t == Value::T::Object && obj.obj)
            return &obj.obj->fields[e.strVal];
    }
    if (e.kind == ExprKind::Index) {
        Value base = eval(*e.a);
        Value idx = eval(*e.b);
        if (base.t == Value::T::Array && base.arr) {
            long long k = (long long)idx.num();
            if (k >= 0 && k < (long long)base.arr->size())
                return &(*base.arr)[k];
        }
    }
    return nullptr;
}

void Interpreter::assign(const Expr& target, Value v) {
    // Actor transform component write-back:  <actor>.position.y = 3
    // (transform.position returns a fresh Vector3 copy, so a plain lvalue write
    // would be lost -- route single components straight into the Transform.)
    if (target.kind == ExprKind::Member && target.a && target.a->kind == ExprKind::Member &&
        (target.strVal == "x" || target.strVal == "y" || target.strVal == "z")) {
        const Expr& mid = *target.a;
        if (mid.strVal == "position" || mid.strVal == "rotation" || mid.strVal == "scale") {
            Value base = eval(*mid.a);
            if (base.t == Value::T::Actor && base.actor) {
                Transform& t = base.actor->transform();
                Vec3& dst = mid.strVal == "position" ? t.position
                            : mid.strVal == "rotation" ? t.rotationEuler
                                                       : t.scale;
                float& c = target.strVal == "x" ? dst.x : target.strVal == "y" ? dst.y : dst.z;
                c = (float)v.num();
                return;
            }
        }
    }

    // Actor transform proxies: this.actor.position = Vector3(...)
    if (target.kind == ExprKind::Member) {
        Value obj = eval(*target.a);
        // Native component write-back, e.g. get_component(type_of(Fog)).start = 10:
        // such an object is a live view onto the real component -- a
        // BuiltinComponent (Fog/Camera) or, since Phase 9a, a compiled
        // script instance -- not a fields-map snapshot, whether reached
        // directly off the call or via a variable holding it.
        if (obj.t == Value::T::Object && obj.obj && obj.obj->nativePtr) {
            if (crate::script::trySetObjectMember(obj.obj, target.strVal, v))
                return;
            throw RuntimeError(obj.obj->builtin + " has no member '" + target.strVal + "'",
                               target.line);
        }
        if (obj.t == Value::T::Actor) {
            crate::Actor* a = obj.actor;
            auto setVec = [&](Vec3& dst) {
                if (v.t == Value::T::Object && v.obj) {
                    dst.x = (float)v.obj->fields["x"].num();
                    dst.y = (float)v.obj->fields["y"].num();
                    dst.z = (float)v.obj->fields["z"].num();
                }
            };
            if (target.strVal == "position") { setVec(a->transform().position); return; }
            if (target.strVal == "rotation") { setVec(a->transform().rotationEuler); return; }
            if (target.strVal == "scale") { setVec(a->transform().scale); return; }
            throw RuntimeError("cannot assign Actor." + target.strVal, target.line);
        }
        if (obj.t == Value::T::TypeRef && obj.s == "Camera" && target.strVal == "main") {
            if (v.t != Value::T::Object || !v.obj || v.obj->builtin != "Camera" ||
                !v.obj->nativePtr)
                throw RuntimeError(
                    "Camera.main must be assigned a Camera (e.g. actor.get_component(type_of(Camera)))",
                    target.line);
            activateMainCamera(v.obj->owner, *static_cast<CameraComponent*>(v.obj->nativePtr));
            return;
        }
        if (obj.t == Value::T::TypeRef && ctx_->getStatic) {
            // Kind-agnostic (Phase 9e): the old direct `so->fields[...] =
            // v` only ever worked for an interpreted static -- a native
            // one's `fields` map is never read/written at all.
            if (auto so = ctx_->getStatic(obj.s)) {
                crate::script::trySetObjectMember(so, target.strVal, v);
                return;
            }
        }
    }
    if (Value* slot = lvalue(target)) {
        *slot = std::move(v);
        return;
    }
    throw RuntimeError("invalid assignment target", target.line);
}

} // namespace crate::script
