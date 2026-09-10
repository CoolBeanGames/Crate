#include "script/Interpreter.h"

#include "core/Math.h"
#include "scene/Actor.h"

#include <cmath>

namespace crate::script {

Value makeVector(const std::string& kind, double x, double y, double z) {
    auto o = std::make_shared<ScriptObject>();
    o->builtin = kind;
    o->fields["x"] = Value::Float(x);
    o->fields["y"] = Value::Float(y);
    o->fields["z"] = Value::Float(kind == "Vector2" ? 0.0 : z);
    return Value::Obj(o);
}

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

// Coerce a value to a declared type name (best effort; keeps the value on a
// mismatch rather than erroring, in the spirit of a loose scripting language).
static Value coerce(Value v, const std::string& ty) {
    if (ty.empty())
        return v;
    if (ty == "int")
        return Value::Int((long long)v.num());
    if (ty == "float")
        return Value::Float(v.num());
    if (ty == "bool")
        return Value::Bool(v.truthy());
    if (ty == "string")
        return v.t == Value::T::String ? v : Value::Str(v.str());
    if (ty == "char")
        return v.t == Value::T::Char ? v : Value::Char(v.str().empty() ? '\0' : v.str()[0]);
    return v; // Actor / Vector / arrays / class types pass through
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
    return runFunction(*fn, definedIn, args);
}

Value Interpreter::runFunction(const FunctionDecl& fn, const ClassInfo* definedIn,
                               std::vector<Value>& args) {
    const ClassInfo* prevDispatch = dispatchClass_;
    dispatchClass_ = definedIn ? definedIn : dispatchClass_;
    scopes_.push_back({});
    for (size_t k = 0; k < fn.params.size(); ++k) {
        Value a = k < args.size() ? args[k] : Value::Null_();
        scopes_.back().vars[fn.params[k].name] = coerce(std::move(a), fn.params[k].type);
    }

    Value result = Value::Null_();
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
            // a bare type name evaluates to a TypeRef
            if (ctx_->findType(e.strVal) || e.strVal == "Actor" || e.strVal == "Actor2D" ||
                e.strVal == "Actor3D" || e.strVal == "Vector2" || e.strVal == "Vector3")
                return Value::Type(e.strVal);
            throw RuntimeError("unknown identifier '" + e.strVal + "'", e.line);
        }
        case ExprKind::Unary: {
            Value a = eval(*e.a);
            if (e.op == Tok::Minus)
                return a.t == Value::T::Int ? Value::Int(-a.i) : Value::Float(-a.num());
            if (e.op == Tok::Not)
                return Value::Bool(!a.truthy());
            return a;
        }
        case ExprKind::Binary: return evalBinary(e);
        case ExprKind::Assign: {
            Value v = eval(*e.b);
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
        case Tok::EqEq:
            if (a.isNumeric() && b.isNumeric())
                return Value::Bool(a.num() == b.num());
            return Value::Bool(a.str() == b.str() && a.t == b.t);
        case Tok::NotEq:
            if (a.isNumeric() && b.isNumeric())
                return Value::Bool(a.num() != b.num());
            return Value::Bool(!(a.str() == b.str() && a.t == b.t));
        case Tok::Lt: return Value::Bool(a.num() < b.num());
        case Tok::Gt: return Value::Bool(a.num() > b.num());
        case Tok::LtEq: return Value::Bool(a.num() <= b.num());
        case Tok::GtEq: return Value::Bool(a.num() >= b.num());
        default: break;
    }

    if (e.op == Tok::Plus && (a.t == Value::T::String || b.t == Value::T::String ||
                              a.t == Value::T::Char || b.t == Value::T::Char))
        return Value::Str(a.str() + b.str());

    bool bothInt = a.t == Value::T::Int && b.t == Value::T::Int;
    double x = a.num(), y = b.num();
    switch (e.op) {
        case Tok::Plus: return bothInt ? Value::Int(a.i + b.i) : Value::Float(x + y);
        case Tok::Minus: return bothInt ? Value::Int(a.i - b.i) : Value::Float(x - y);
        case Tok::Star: return bothInt ? Value::Int(a.i * b.i) : Value::Float(x * y);
        case Tok::Slash:
            if (y == 0.0)
                throw RuntimeError("division by zero", e.line);
            return bothInt ? Value::Int(a.i / b.i) : Value::Float(x / y);
        case Tok::Percent:
            if (y == 0.0)
                throw RuntimeError("modulo by zero", e.line);
            return bothInt ? Value::Int(a.i % b.i) : Value::Float(std::fmod(x, y));
        default: break;
    }
    throw RuntimeError("bad operator", e.line);
}

Value Interpreter::actorMember(crate::Actor* a, const std::string& name, int line) {
    if (!a)
        throw RuntimeError("null actor", line);
    if (name == "name")
        return Value::Str(a->name());
    Transform& t = a->transform();
    if (name == "position")
        return makeVector("Vector3", t.position.x, t.position.y, t.position.z);
    if (name == "rotation")
        return makeVector("Vector3", t.rotationEuler.x, t.rotationEuler.y, t.rotationEuler.z);
    if (name == "scale")
        return makeVector("Vector3", t.scale.x, t.scale.y, t.scale.z);
    throw RuntimeError("Actor has no member '" + name + "'", line);
}

Value Interpreter::evalMember(const Expr& e) {
    // this.base.method(...)  -> handled in evalCall; a bare this.base is just this.
    Value obj = eval(*e.a);
    const std::string& name = e.strVal;

    if (obj.t == Value::T::Object && obj.obj) {
        auto it = obj.obj->fields.find(name);
        if (it != obj.obj->fields.end())
            return it->second;
        // A script instance exposes its owning actor as `this.actor`.
        if (obj.obj->cls && name == "actor")
            return Value::ActorRef(obj.obj->owner);
        throw RuntimeError("no member '" + name + "'", e.line);
    }
    if (obj.t == Value::T::Actor)
        return actorMember(obj.actor, name, e.line);
    if (obj.t == Value::T::Array && name == "length")
        return Value::Int(obj.arr ? (long long)obj.arr->size() : 0);
    if (obj.t == Value::T::TypeRef && ctx_->getStatic) {
        if (auto so = ctx_->getStatic(obj.s)) {
            auto it = so->fields.find(name);
            if (it != so->fields.end())
                return it->second;
        }
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
    if (callee.kind == ExprKind::Identifier)
        return builtinCall(callee.strVal, args, e.line);

    // Method call: obj.method(args)
    if (callee.kind == ExprKind::Member) {
        const Expr& objExpr = *callee.a;
        const std::string& method = callee.strVal;

        // this.base.method(...)
        if (objExpr.kind == ExprKind::Member && objExpr.strVal == "base" &&
            objExpr.a->kind == ExprKind::This) {
            return callMethodOn(self_, method, std::move(args), e.line, /*viaBase=*/true);
        }

        Value obj = eval(objExpr);

        // StaticClass.method(...)  -> call on the static singleton
        if (obj.t == Value::T::TypeRef && ctx_->getStatic) {
            if (auto so = ctx_->getStatic(obj.s))
                return callMethodOn(so, method, std::move(args), e.line, false);
        }

        if (obj.t == Value::T::Object && obj.obj && obj.obj->cls) {
            // `this.get_component(...)` / `this.actor` shortcuts forward to the
            // owning actor when the script class has no such method.
            if (!obj.obj->cls->findFunction(method)) {
                if (method == "get_component") {
                    std::string tn;
                    if (!args.empty())
                        tn = args[0].t == Value::T::TypeRef ? args[0].s : args[0].str();
                    if (ctx_->getComponent)
                        if (auto so = ctx_->getComponent(obj.obj->owner, tn))
                            return Value::Obj(so);
                    return Value::Null_();
                }
            }
            return callMethodOn(obj.obj, method, std::move(args), e.line, false);
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
            if (method == "add" && !args.empty() && obj.arr) {
                obj.arr->push_back(args[0]);
                return Value::Null_();
            }
            if (method == "length" && obj.arr)
                return Value::Int((long long)obj.arr->size());
        }
        // universal .str()
        if (method == "str")
            return Value::Str(obj.str());

        throw RuntimeError("no method '" + method + "'", e.line);
    }

    throw RuntimeError("expression is not callable", e.line);
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
    // Actor transform proxies: this.actor.position = Vector3(...)
    if (target.kind == ExprKind::Member) {
        Value obj = eval(*target.a);
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
        if (obj.t == Value::T::TypeRef && ctx_->getStatic) {
            if (auto so = ctx_->getStatic(obj.s)) {
                so->fields[target.strVal] = std::move(v);
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
