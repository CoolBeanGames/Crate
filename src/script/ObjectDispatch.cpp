#include "script/ObjectDispatch.h"

#include "script/ClassInfo.h"
#include "script/CompiledClassInfo.h"
#include "script/Runtime.h"

#include "scene/Actor.h"
#include "scene/BuiltinComponents.h"

#include <algorithm>

namespace crate::script {
namespace {
// Base-chain walk for a Kind-3 (compiled) instance's fields/methods/signals
// (Phase 9f): unlike `overflow`/`selfView` (whose OWN per-class wrapper
// function already reaches inherited storage correctly via ordinary C++
// member access -- static_cast<Derived*>(p)->overflow_ finds an INHERITED
// overflow_ exactly like an own one, no walk needed), a derived class's OWN
// kFields_<suffix>/kMethods_<suffix>/kSignals_<suffix> deliberately list
// ONLY its own newly-declared/overridden members (mirroring how
// ClassDecl::fields/functions/signals are also own-only, see Ast.h) -- an
// INHERITED-but-not-overridden name is only discoverable by walking to the
// ancestor's OWN table via ci->baseClassInfo, exactly like
// ClassInfo::findFunction()/hasSignal() walk ClassInfo::baseClass for the
// interpreted side. The ancestor's OWN accessor function pointers remain
// correct to call with a DERIVED instance's void* (single, non-virtual
// inheritance: a derived object's address IS its unique base subobject's
// address, so static_cast<Base*>(derivedVoidPtr) is safe and correct).
const FieldAccessor* findCompiledField(const CompiledClassInfo* ci, const std::string& name) {
    for (; ci; ci = ci->baseClassInfo)
        for (size_t i = 0; i < ci->fieldCount; ++i)
            if (name == ci->fields[i].name)
                return &ci->fields[i];
    return nullptr;
}
const MethodAccessor* findCompiledMethod(const CompiledClassInfo* ci, const std::string& name) {
    for (; ci; ci = ci->baseClassInfo)
        for (size_t i = 0; i < ci->methodCount; ++i)
            if (name == ci->methods[i].name)
                return &ci->methods[i];
    return nullptr;
}
bool compiledHasSignal(const CompiledClassInfo* ci, const std::string& name) {
    for (; ci; ci = ci->baseClassInfo)
        for (size_t i = 0; i < ci->signalCount; ++i)
            if (name == ci->signalNames[i])
                return true;
    return false;
}
} // namespace

Value getObjectMember(const std::shared_ptr<ScriptObject>& obj, const std::string& name, int line) {
    if (!obj)
        throw RuntimeError("cannot read '." + name + "' on a null value", line);

    // InputButton signals: just_pressed/just_released/pressed aren't stored
    // fields (an InputButton object is Kind 4, `fields` stays empty) --
    // they're synthesized as a SignalRef on demand. Moved here (Phase 9d)
    // from Interpreter::evalMember's own inline special case, generalizing
    // it so generated code's getValueMember -> getObjectMember path gets it
    // too, not just the interpreter -- checked before every other branch
    // below since it applies regardless of kind (though in practice
    // builtin=="InputButton" is always Kind 4).
    if (obj->builtin == "InputButton" &&
        (name == "just_pressed" || name == "just_released" || name == "pressed"))
        return Value::SignalRef(obj, name);

    // Kind 3: compiled script instance live view. Checked before the
    // `fields` map since these objects don't use `fields` at all. Field/
    // method/signal lookups walk the base-class chain (Phase 9f) via the
    // helpers above, so an inherited-but-not-overridden name (declared by
    // an ancestor, not obj->compiledInfo's own class) still resolves.
    if (obj->nativePtr && obj->compiledInfo) {
        const CompiledClassInfo* ci = obj->compiledInfo;
        if (const FieldAccessor* fa = findCompiledField(ci, name))
            return fa->get(obj->nativePtr);
        if (ci->overflow) {
            auto& ovf = ci->overflow(obj->nativePtr);
            auto it = ovf.find(name);
            if (it != ovf.end())
                return it->second;
        }
        if (name == "actor")
            return Value::ActorRef(obj->owner);
        // Declared `signal foo();` names (Phase 9d): resolves to a
        // SignalRef pointing at `obj` itself -- correct ONLY because `obj`
        // is guaranteed to be the canonical selfView_ for this instance
        // (see CompiledClassInfo::selfView's doc comment), so its
        // `connections` map is the SAME one every other reference to this
        // instance shares.
        if (compiledHasSignal(ci, name))
            return Value::SignalRef(obj, name);
        if (findCompiledMethod(ci, name))
            return Value::Fn(obj, name); // bound Callable, invoked via callObjectMethod
        throw RuntimeError("no member '" + name + "'", line);
    }

    // Kind 2: BuiltinComponent live view (Fog/Camera). nativePtr set,
    // compiledInfo not -- also doesn't use `fields`.
    if (obj->nativePtr) {
        Value out;
        if (getNativeField(*obj, name, out))
            return out;
        throw RuntimeError(obj->builtin + " has no member '" + name + "'", line);
    }

    // Kind 1 (interpreted script instance, cls != nullptr) AND Kind 4 (a
    // plain aggregate with no cls at all -- Vector2/Vector3/InputButton)
    // BOTH store their data in `fields` -- checked regardless of cls,
    // exactly matching Interpreter::evalMember's original order (fields
    // looked up unconditionally first; "actor"/signal/bound-method
    // fallbacks only make sense, and are only tried, for an actual script
    // instance). Missing this for Kind 4 was a real regression caught by
    // script_tests.cpp/script_bug_tests.cpp/script_integration_tests.cpp
    // immediately failing ("cannot read '.y' on Vector3") the first time
    // this function replaced evalMember's inline logic.
    auto it = obj->fields.find(name);
    if (it != obj->fields.end())
        return it->second;
    if (obj->cls) {
        if (name == "actor")
            return Value::ActorRef(obj->owner);
        if (obj->cls->hasSignal(name))
            return Value::SignalRef(obj, name);
        if (obj->cls->findFunction(name))
            return Value::Fn(obj, name);
    }
    throw RuntimeError("no member '" + name + "'", line);
}

bool trySetObjectMember(const std::shared_ptr<ScriptObject>& obj, const std::string& name,
                        const Value& v) {
    if (!obj)
        return false;

    if (obj->nativePtr && obj->compiledInfo) {
        const CompiledClassInfo* ci = obj->compiledInfo;
        // Base-chain walk (Phase 9f) -- see findCompiledField's own doc
        // comment above.
        if (const FieldAccessor* fa = findCompiledField(ci, name)) {
            fa->set(obj->nativePtr, v);
            return true;
        }
        if (ci->overflow) {
            ci->overflow(obj->nativePtr)[name] = v;
            return true;
        }
        return false;
    }

    if (obj->nativePtr)
        return setNativeField(*obj, name, v);

    // Kind 1 (interpreted script instance) or Kind 4 (plain aggregate --
    // Vector2/Vector3/InputButton): implicit field creation, exactly
    // matching Interpreter::lvalue()'s Member case
    // (`return &obj.obj->fields[e.strVal];`), which applies unconditionally
    // to any Object value reaching it, not just cls-backed ones.
    obj->fields[name] = v;
    return true;
}

namespace {
// args[0] as a get_component() type-name argument: a TypeRef's own name, or
// str() of anything else -- mirrors both of Interpreter::evalCall's
// `get_component` branches (Object-escape-hatch and native-Actor-method)
// resolving their type-name argument identically.
std::string typeArgOrEmpty(const std::vector<Value>& args) {
    if (args.empty())
        return {};
    return args[0].t == Value::T::TypeRef ? args[0].s : args[0].str();
}

Value getComponentOn(ScriptContext* ctx, const Value& receiver, const std::string& typeName) {
    crate::Actor* owner = receiver.t == Value::T::Object && receiver.obj ? receiver.obj->owner
                          : receiver.t == Value::T::Actor                ? receiver.actor
                                                                          : nullptr;
    if (ctx && ctx->getComponent)
        if (auto so = ctx->getComponent(owner, typeName))
            return Value::Obj(so);
    return Value::Null_();
}

// Two callables refer to the same target+method (for disconnect /
// is_connected). Moved from Interpreter.cpp (Phase 9d) alongside
// signalCall/emitSignal/invokeCallable, which all use it.
bool sameCallable(const Value& a, const Value& b) {
    return a.t == Value::T::Callable && b.t == Value::T::Callable && a.s == b.s &&
           a.wobj.lock() == b.wobj.lock();
}
} // namespace

Value callObjectMethod(ScriptContext* ctx, const std::shared_ptr<ScriptObject>& obj,
                       const std::string& method, std::vector<Value> args, int line) {
    if (!obj)
        throw RuntimeError("cannot call '" + method + "' on a null value", line);

    const bool isScriptInstance = obj->cls || (obj->nativePtr && obj->compiledInfo);
    if (isScriptInstance) {
        // Kind-agnostic, base-chain-aware (Phase 9f for the compiled side --
        // ClassInfo::findFunction already walked its own base chain since
        // before this session).
        bool hasOwnMethod = obj->cls ? obj->cls->findFunction(method) != nullptr
                                     : findCompiledMethod(obj->compiledInfo, method) != nullptr;
        // Godot-3 style signal API + get_component escape hatch, available
        // on ANY script instance (interpreted or compiled) that doesn't
        // declare its own method of that name -- consolidated here (Phase
        // 9d) from what used to be Interpreter::evalCall's own inline
        // shortcut block, so BOTH the interpreter and generated code
        // (via callValueMethod) get it from one place.
        if (!hasOwnMethod) {
            if (method == "emit_signal" && !args.empty()) {
                emitSignal(ctx, obj, args[0].str(),
                          std::vector<Value>(args.begin() + 1, args.end()), line);
                return Value::Null_();
            }
            if ((method == "connect" || method == "disconnect" || method == "is_connected") &&
                args.size() >= 2 && args[1].t == Value::T::Callable) {
                Value sigRef = Value::SignalRef(obj, args[0].str());
                return signalCall(ctx, sigRef, method, {args[1]}, line);
            }
            if (method == "get_component")
                return getComponentOn(ctx, Value::Obj(obj), typeArgOrEmpty(args));
        }
    }

    if (obj->cls) {
        Interpreter interp(ctx, obj);
        return interp.callMethodOn(obj, method, std::move(args), line, /*viaBase=*/false);
    }

    if (obj->nativePtr && obj->compiledInfo) {
        if (const MethodAccessor* ma = findCompiledMethod(obj->compiledInfo, method))
            return ma->invoke(obj->nativePtr, ctx, std::move(args));
        throw RuntimeError("method '" + method + "' not found", line);
    }

    throw RuntimeError("cannot call '" + method + "' on a non-script value", line);
}

Value signalCall(ScriptContext* ctx, const Value& sig, const std::string& method, std::vector<Value> args,
                 int line) {
    if (!sig.obj)
        throw RuntimeError("signal has no owner", line);
    auto& conns = sig.obj->connections[sig.s];

    if (method == "connect") {
        if (args.empty() || args[0].t != Value::T::Callable)
            throw RuntimeError("signal.connect expects a callable", line);
        for (const auto& c : conns)
            if (sameCallable(c, args[0]))
                return Value::Null_(); // already connected
        conns.push_back(args[0]);
        return Value::Null_();
    }
    if (method == "disconnect") {
        if (!args.empty())
            conns.erase(std::remove_if(conns.begin(), conns.end(),
                                       [&](const Value& c) { return sameCallable(c, args[0]); }),
                        conns.end());
        return Value::Null_();
    }
    if (method == "is_connected") {
        for (const auto& c : conns)
            if (!args.empty() && sameCallable(c, args[0]))
                return Value::Bool(true);
        return Value::Bool(false);
    }
    if (method == "emit") {
        emitSignal(ctx, sig.obj, sig.s, std::move(args), line);
        return Value::Null_();
    }
    if (method == "get_connections")
        return Value::Int((long long)conns.size());
    throw RuntimeError("signal has no method '" + method + "'", line);
}

Value invokeCallable(ScriptContext* ctx, const Value& fn, std::vector<Value> args, int line) {
    if (fn.t != Value::T::Callable)
        throw RuntimeError("value is not callable", line);
    auto self = fn.wobj.lock();
    if (!self)
        return Value::Null_(); // target was freed; a no-op, as in Godot
    return callObjectMethod(ctx, self, fn.s, std::move(args), line);
}

void emitSignal(ScriptContext* ctx, const std::shared_ptr<ScriptObject>& owner, const std::string& name,
                std::vector<Value> args, int line) {
    if (!owner)
        return;
    auto it = owner->connections.find(name);
    if (it == owner->connections.end())
        return;
    // Copy: a handler may connect/disconnect while we iterate.
    std::vector<Value> handlers = it->second;
    for (const auto& h : handlers)
        invokeCallable(ctx, h, args, line);
}

Value getValueMember(ScriptContext* ctx, const Value& v, const std::string& name, int line,
                     crate::Actor* callerOwner) {
    if (v.t == Value::T::Object && v.obj)
        return getObjectMember(v.obj, name, line);
    if (v.t == Value::T::Actor)
        return actorMember(v.actor, name, line);
    if (v.t == Value::T::Array && name == "length")
        return arrayLength(v);
    // Camera.main (Phase 9c), mirroring Interpreter::evalMember's TypeRef
    // "Camera"+"main" branch exactly: the scene to search is found by
    // walking up from the CALLING script's own actor (see the header
    // comment), not from anything reachable off `v` itself (a bare TypeRef
    // carries no actor at all).
    if (v.t == Value::T::TypeRef && v.s == "Camera" && name == "main") {
        CameraComponent* cc = mainCameraFrom(callerOwner);
        if (!cc)
            return Value::Null_();
        auto o = std::make_shared<ScriptObject>();
        o->builtin = "Camera";
        o->nativePtr = cc;
        o->owner = cc->actor();
        return Value::Obj(o);
    }
    // StaticClassName.field (Phase 9e): resolves ctx->getStatic's singleton
    // (interpreted or native, Kind-agnostically) then dispatches through
    // getObjectMember -- NOT the old direct `so->fields.find(name)` this
    // mirrored before 9e, which only ever worked for an INTERPRETED static
    // (a native one's data lives behind its CompiledClassInfo accessor
    // table, never in `fields` at all).
    if (v.t == Value::T::TypeRef && ctx && ctx->getStatic) {
        if (auto so = ctx->getStatic(v.s))
            return getObjectMember(so, name, line);
    }
    throw RuntimeError("cannot read '." + name + "' on " + v.typeName(), line);
}

bool trySetValueMember(ScriptContext* ctx, const Value& v, const std::string& name, const Value& val) {
    if (v.t == Value::T::Object && v.obj)
        return trySetObjectMember(v.obj, name, val);
    // General Actor-typed receiver write-back: position/rotation/scale,
    // exactly mirroring Interpreter::assign()'s Actor branch (previously
    // only reachable for the bare `transform`/`actor` identifiers CodeGen
    // special-cases -- this lets a field/get_component-result/etc holding
    // an Actor reference support the same writes).
    if (v.t == Value::T::Actor && v.actor) {
        crate::Actor* a = v.actor;
        auto setVec = [&](Vec3& dst) -> bool {
            if (val.t != Value::T::Object || !val.obj)
                return false;
            dst.x = (float)vfield(val, "x");
            dst.y = (float)vfield(val, "y");
            dst.z = (float)vfield(val, "z");
            return true;
        };
        if (name == "position")
            return setVec(a->transform().position);
        if (name == "rotation")
            return setVec(a->transform().rotationEuler);
        if (name == "scale")
            return setVec(a->transform().scale);
        return false;
    }
    // Camera.main = someCamera; (Phase 9c), mirroring Interpreter::assign's
    // TypeRef "Camera"+"main" branch exactly -- unlike the read side, this
    // needs no caller-owner context: activateMainCamera walks up from the
    // CAMERA VALUE BEING ASSIGNED's own owner, not the caller's.
    if (v.t == Value::T::TypeRef && v.s == "Camera" && name == "main") {
        if (val.t != Value::T::Object || !val.obj || val.obj->builtin != "Camera" || !val.obj->nativePtr)
            return false;
        activateMainCamera(val.obj->owner, *static_cast<CameraComponent*>(val.obj->nativePtr));
        return true;
    }
    // StaticClassName.field = v (Phase 9e): same Kind-agnostic fix as the
    // read side -- routes through trySetObjectMember instead of the old
    // direct `so->fields[name] = v`, which silently did nothing useful for
    // a native static.
    if (v.t == Value::T::TypeRef && ctx && ctx->getStatic) {
        if (auto so = ctx->getStatic(v.s))
            return trySetObjectMember(so, name, val);
        return false;
    }
    return false;
}

Value callValueMethod(ScriptContext* ctx, const Value& v, const std::string& method,
                      std::vector<Value> args, int line, crate::Actor* callerOwner) {
    // Signal.connect/disconnect/is_connected/emit/get_connections (Phase
    // 9d), and callable.call(args)/callable.emit(args) -- both delegate to
    // the SAME free functions the interpreter itself now uses (see
    // Interpreter::signalCall/invokeCallable's own thin wrappers), so a
    // signal/Callable Value behaves identically regardless of which side
    // (caller or target) is compiled.
    if (v.t == Value::T::Signal)
        return signalCall(ctx, v, method, std::move(args), line);
    if (v.t == Value::T::Callable && (method == "call" || method == "emit"))
        return invokeCallable(ctx, v, std::move(args), line);
    // Object (declared method / get_component / the Godot-3 signal
    // shortcuts) -- ALL of that logic now lives in callObjectMethod itself
    // (Phase 9d consolidation), so this is a direct delegation, no
    // duplicated hasOwnMethod/get_component logic here anymore.
    if (v.t == Value::T::Object && v.obj && (v.obj->cls || (v.obj->nativePtr && v.obj->compiledInfo)))
        return callObjectMethod(ctx, v.obj, method, std::move(args), line);
    if (v.t == Value::T::Actor) {
        if (method == "get_component")
            return getComponentOn(ctx, v, typeArgOrEmpty(args));
        throw RuntimeError("Actor has no method '" + method + "'", line);
    }
    if (v.t == Value::T::Array) {
        if (method == "add" && !args.empty() && arrayAdd(v, args[0]))
            return Value::Null_();
        if (method == "length" && v.arr)
            return arrayLength(v);
    }
    // StaticClassName.method(...) (Phase 9e): call on the static singleton,
    // Kind-agnostically via callObjectMethod -- the old direct callMethodOn
    // only worked on an INTERPRETED static (it throws immediately for a
    // Kind-3 object, whose `cls` is always null).
    if (v.t == Value::T::TypeRef && ctx && ctx->getStatic) {
        if (auto so = ctx->getStatic(v.s))
            return callObjectMethod(ctx, so, method, std::move(args), line);
    }
    // Value.instantiate(): mirrors Interpreter::evalCall's identical check
    // exactly (see its comment) -- a "Scene"-typed field is a plain string
    // holding a .cscene asset path.
    if (v.t == Value::T::String && method == "instantiate")
        return valueInstantiate(ctx, v, callerOwner, line);
    if (method == "str")
        return Value::Str(v.str());
    throw RuntimeError("no method '" + method + "'", line);
}

} // namespace crate::script
