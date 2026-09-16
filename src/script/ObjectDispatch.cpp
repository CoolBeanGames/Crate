#include "script/ObjectDispatch.h"

#include "script/ClassInfo.h"
#include "script/CompiledClassInfo.h"
#include "script/Runtime.h"

#include "scene/Actor.h"
#include "scene/BuiltinComponents.h"

namespace crate::script {

Value getObjectMember(const std::shared_ptr<ScriptObject>& obj, const std::string& name, int line) {
    if (!obj)
        throw RuntimeError("cannot read '." + name + "' on a null value", line);

    // Kind 3: compiled script instance live view. Checked before the
    // `fields` map since these objects don't use `fields` at all.
    if (obj->nativePtr && obj->compiledInfo) {
        const CompiledClassInfo* ci = obj->compiledInfo;
        for (size_t i = 0; i < ci->fieldCount; ++i)
            if (name == ci->fields[i].name)
                return ci->fields[i].get(obj->nativePtr);
        if (ci->overflow) {
            auto& ovf = ci->overflow(obj->nativePtr);
            auto it = ovf.find(name);
            if (it != ovf.end())
                return it->second;
        }
        if (name == "actor")
            return Value::ActorRef(obj->owner);
        for (size_t i = 0; i < ci->methodCount; ++i)
            if (name == ci->methods[i].name)
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
        for (size_t i = 0; i < ci->fieldCount; ++i)
            if (name == ci->fields[i].name) {
                ci->fields[i].set(obj->nativePtr, v);
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

Value callObjectMethod(ScriptContext* ctx, const std::shared_ptr<ScriptObject>& obj,
                       const std::string& method, std::vector<Value> args, int line) {
    if (!obj)
        throw RuntimeError("cannot call '" + method + "' on a null value", line);

    if (obj->cls) {
        Interpreter interp(ctx, obj);
        return interp.callMethodOn(obj, method, std::move(args), line, /*viaBase=*/false);
    }

    if (obj->nativePtr && obj->compiledInfo) {
        const CompiledClassInfo* ci = obj->compiledInfo;
        for (size_t i = 0; i < ci->methodCount; ++i)
            if (method == ci->methods[i].name)
                return ci->methods[i].invoke(obj->nativePtr, ctx, std::move(args));
        throw RuntimeError("method '" + method + "' not found", line);
    }

    throw RuntimeError("cannot call '" + method + "' on a non-script value", line);
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
} // namespace

Value getValueMember(const Value& v, const std::string& name, int line, crate::Actor* callerOwner) {
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
    throw RuntimeError("cannot read '." + name + "' on " + v.typeName(), line);
}

bool trySetValueMember(const Value& v, const std::string& name, const Value& val) {
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
    return false;
}

Value callValueMethod(ScriptContext* ctx, const Value& v, const std::string& method,
                      std::vector<Value> args, int line) {
    if (v.t == Value::T::Object && v.obj && (v.obj->cls || (v.obj->nativePtr && v.obj->compiledInfo))) {
        bool hasOwnMethod =
            v.obj->cls ? v.obj->cls->findFunction(method) != nullptr
                      : [&] {
                            const CompiledClassInfo* ci = v.obj->compiledInfo;
                            for (size_t i = 0; i < ci->methodCount; ++i)
                                if (ci->methods[i].name == method)
                                    return true;
                            return false;
                        }();
        // get_component is an escape hatch available on any script instance
        // that doesn't declare its own method of that name, exactly as in
        // Interpreter::evalCall (signals/emit_signal/connect/disconnect/
        // is_connected on a general Object receiver are Phase 9d, not yet
        // handled here -- they fall through to callObjectMethod below,
        // which throws "method not found" until then).
        if (!hasOwnMethod && method == "get_component")
            return getComponentOn(ctx, v, typeArgOrEmpty(args));
        return callObjectMethod(ctx, v.obj, method, std::move(args), line);
    }
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
    if (method == "str")
        return Value::Str(v.str());
    throw RuntimeError("no method '" + method + "'", line);
}

} // namespace crate::script
