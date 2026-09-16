#include "script/ObjectDispatch.h"

#include "script/ClassInfo.h"
#include "script/CompiledClassInfo.h"
#include "script/Runtime.h"

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

} // namespace crate::script
