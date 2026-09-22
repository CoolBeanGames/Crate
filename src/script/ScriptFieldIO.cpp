#include "script/ScriptFieldIO.h"

#include "scene/Actor.h"
#include "scene/FieldCodec.h"
#include "script/ClassInfo.h"
#include "script/CompiledClassInfo.h"
#include "script/Interpreter.h"
#include "script/Runtime.h"

#include <ostream>

namespace crate::script {

void writeValueField(std::ostream& out, const std::string& key, const Value& v,
                     const std::function<int(const crate::Actor*)>& idOf) {
    switch (v.t) {
        case Value::T::Null:
        case Value::T::Signal:
        case Value::T::Callable:
        case Value::T::Array: // arrays of arbitrary values: not yet supported by Scenes
            writeField(out, key.c_str(), "n", "");
            return;
        case Value::T::Bool: writeFieldBool(out, key.c_str(), v.b); return;
        case Value::T::Int: writeFieldInt(out, key.c_str(), v.i); return;
        case Value::T::Float: writeFieldFloat(out, key.c_str(), v.f); return;
        case Value::T::Char:
        case Value::T::String: writeFieldString(out, key.c_str(), v.s); return;
        case Value::T::TypeRef: writeFieldString(out, key.c_str(), v.s); return;
        case Value::T::Actor: {
            int id = v.actor ? idOf(v.actor) : -1;
            writeField(out, key.c_str(), "aref", std::to_string(id));
            return;
        }
        case Value::T::Object: {
            if (!v.obj) {
                writeField(out, key.c_str(), "n", "");
                return;
            }
            if (v.obj->builtin == "Vector2") {
                writeFieldVec2(out, key.c_str(), (float)vfield(v, "x"), (float)vfield(v, "y"));
                return;
            }
            if (v.obj->builtin == "Vector3") {
                writeFieldVec3(out, key.c_str(), (float)vfield(v, "x"), (float)vfield(v, "y"),
                               (float)vfield(v, "z"));
                return;
            }
            // A live-view reference: either a native builtin component
            // (builtin names it directly, e.g. "Light"/"Transform") or
            // another script instance (cls/compiledInfo name its class).
            // Both resolve the same way on load: ctx->getComponent(owner,
            // typeName) -- exactly the generalized lookup Inspector drag-
            // drop and get_component() already share (ScriptSystem.cpp).
            std::string typeName = v.obj->builtin;
            if (typeName.empty() && v.obj->cls)
                typeName = v.obj->cls->name;
            if (typeName.empty() && v.obj->compiledInfo)
                typeName = v.obj->compiledInfo->className;
            int id = v.obj->owner ? idOf(v.obj->owner) : -1;
            if (id < 0 || typeName.empty()) {
                writeField(out, key.c_str(), "n", "");
                return;
            }
            writeField(out, key.c_str(), "cref", std::to_string(id) + " " + typeName);
            return;
        }
    }
}

Value readValueField(const std::string& kind, const std::string& raw,
                     const std::function<crate::Actor*(int)>& actorById, ScriptContext* ctx) {
    if (kind == "n") return Value::Null_();
    if (kind == "b") return Value::Bool(fieldB(raw));
    if (kind == "i") return Value::Int(fieldI(raw));
    if (kind == "f") return Value::Float(fieldF(raw));
    if (kind == "s") return Value::Str(raw);
    if (kind == "v2" || kind == "v3") {
        float v[3];
        parseFieldVec3(raw, v);
        return makeVector(kind == "v2" ? "Vector2" : "Vector3", v[0], v[1], v[2]);
    }
    if (kind == "aref") {
        int id = (int)fieldI(raw);
        return Value::ActorRef(id < 0 ? nullptr : actorById(id));
    }
    if (kind == "cref") {
        size_t sp = raw.find(' ');
        if (sp == std::string::npos)
            return Value::Null_();
        int id = (int)fieldI(raw.substr(0, sp));
        std::string typeName = raw.substr(sp + 1);
        crate::Actor* owner = id < 0 ? nullptr : actorById(id);
        if (!owner || !ctx || !ctx->getComponent)
            return Value::Null_();
        if (auto so = ctx->getComponent(owner, typeName))
            return Value::Obj(so);
        return Value::Null_();
    }
    return Value::Null_();
}

} // namespace crate::script
