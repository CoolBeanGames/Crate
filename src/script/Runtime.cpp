#include "script/Runtime.h"

#include "scene/Actor.h"
#include "scene/BuiltinComponents.h"

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

bool isVec(const Value& v) {
    return v.t == Value::T::Object && v.obj &&
           (v.obj->builtin == "Vector2" || v.obj->builtin == "Vector3");
}

double vfield(const Value& v, const char* f) {
    auto it = v.obj->fields.find(f);
    return it == v.obj->fields.end() ? 0.0 : it->second.num();
}

Value coerce(Value v, const std::string& ty) {
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

Value arith(Tok op, const Value& a, const Value& b, int line) {
    if (op == Tok::Plus && (a.t == Value::T::String || b.t == Value::T::String ||
                            a.t == Value::T::Char || b.t == Value::T::Char))
        return Value::Str(a.str() + b.str());

    // Vector math: vector op vector is component-wise; vector op scalar (and
    // scalar op vector) broadcasts the scalar to every component.
    if (isVec(a) || isVec(b)) {
        const std::string kind = ((isVec(a) && a.obj->builtin == "Vector3") ||
                                  (isVec(b) && b.obj->builtin == "Vector3"))
                                     ? "Vector3"
                                     : "Vector2";
        double ax, ay, az, bx, by, bz;
        if (isVec(a)) { ax = vfield(a, "x"); ay = vfield(a, "y"); az = vfield(a, "z"); }
        else          { ax = ay = az = a.num(); }
        if (isVec(b)) { bx = vfield(b, "x"); by = vfield(b, "y"); bz = vfield(b, "z"); }
        else          { bx = by = bz = b.num(); }
        switch (op) {
            case Tok::Plus:  return makeVector(kind, ax + bx, ay + by, az + bz);
            case Tok::Minus: return makeVector(kind, ax - bx, ay - by, az - bz);
            case Tok::Star:  return makeVector(kind, ax * bx, ay * by, az * bz);
            case Tok::Slash:
                if (bx == 0.0 || by == 0.0 || (kind == "Vector3" && bz == 0.0))
                    throw RuntimeError("division by zero", line);
                return makeVector(kind, ax / bx, ay / by, az / bz);
            default:
                throw RuntimeError("operator not defined for vectors", line);
        }
    }

    bool bothInt = a.t == Value::T::Int && b.t == Value::T::Int;
    double x = a.num(), y = b.num();
    switch (op) {
        case Tok::Plus: return bothInt ? Value::Int(a.i + b.i) : Value::Float(x + y);
        case Tok::Minus: return bothInt ? Value::Int(a.i - b.i) : Value::Float(x - y);
        case Tok::Star: return bothInt ? Value::Int(a.i * b.i) : Value::Float(x * y);
        case Tok::Slash:
            if (y == 0.0)
                throw RuntimeError("division by zero", line);
            return bothInt ? Value::Int(a.i / b.i) : Value::Float(x / y);
        case Tok::Percent:
            if (y == 0.0)
                throw RuntimeError("modulo by zero", line);
            return bothInt ? Value::Int(a.i % b.i) : Value::Float(std::fmod(x, y));
        default: break;
    }
    throw RuntimeError("bad operator", line);
}

// ---------------------------------------------------------------------------
// Native (non-script) component field reflection table.
//
// Each entry's get/set is a plain (non-capturing) function pointer so this
// table can be a simple static array -- the same shape a future generated
// CompiledClassInfo field table (see transpiration.txt, Phase 5) will use,
// proving the mechanism here first on the two native types that already
// need it.
// ---------------------------------------------------------------------------

namespace {

struct NativeFieldEntry {
    const char* name;
    Value (*get)(void* ptr);
    // Returns true (the field is recognized and was written) regardless of
    // whether the incoming value's shape matched -- mirrors the original
    // hand-written setNativeField, which always returned true once the field
    // name matched, even when the value type was wrong.
    bool (*set)(void* ptr, const Value& v, crate::Actor* owner);
};

struct NativeTypeEntry {
    const char* builtin;
    const NativeFieldEntry* fields;
    size_t fieldCount;
};

const NativeFieldEntry kFogFields[] = {
    {"color",
     [](void* p) -> Value {
         auto* fc = static_cast<FogComponent*>(p);
         return makeVector("Vector3", fc->color[0], fc->color[1], fc->color[2]);
     },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         auto* fc = static_cast<FogComponent*>(p);
         if (v.t == Value::T::Object && v.obj) {
             fc->color[0] = (float)v.obj->fields["x"].num();
             fc->color[1] = (float)v.obj->fields["y"].num();
             fc->color[2] = (float)v.obj->fields["z"].num();
         }
         return true;
     }},
    {"start",
     [](void* p) -> Value { return Value::Float(static_cast<FogComponent*>(p)->start); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<FogComponent*>(p)->start = (float)v.num();
         return true;
     }},
    {"end",
     [](void* p) -> Value { return Value::Float(static_cast<FogComponent*>(p)->end); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<FogComponent*>(p)->end = (float)v.num();
         return true;
     }},
    {"height_range",
     [](void* p) -> Value { return Value::Float(static_cast<FogComponent*>(p)->heightRange); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<FogComponent*>(p)->heightRange = (float)v.num();
         return true;
     }},
};

const NativeFieldEntry kCameraFields[] = {
    {"fov",
     [](void* p) -> Value { return Value::Float(static_cast<CameraComponent*>(p)->fovY); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<CameraComponent*>(p)->fovY = (float)v.num();
         return true;
     }},
    {"near",
     [](void* p) -> Value { return Value::Float(static_cast<CameraComponent*>(p)->nearZ); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<CameraComponent*>(p)->nearZ = (float)v.num();
         return true;
     }},
    {"far",
     [](void* p) -> Value { return Value::Float(static_cast<CameraComponent*>(p)->farZ); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<CameraComponent*>(p)->farZ = (float)v.num();
         return true;
     }},
    {"active",
     [](void* p) -> Value { return Value::Bool(static_cast<CameraComponent*>(p)->enabled); },
     [](void* p, const Value& v, crate::Actor* owner) -> bool {
         auto* cc = static_cast<CameraComponent*>(p);
         if (v.truthy())
             activateMainCamera(owner, *cc);
         else if (owner)
             cc->enabled = false; // no fallback search here; use Camera.main for that
         return true;
     }},
};

const NativeTypeEntry kNativeTypes[] = {
    {"Fog", kFogFields, sizeof(kFogFields) / sizeof(kFogFields[0])},
    {"Camera", kCameraFields, sizeof(kCameraFields) / sizeof(kCameraFields[0])},
};

const NativeTypeEntry* findNativeType(const std::string& builtin) {
    for (const auto& t : kNativeTypes)
        if (builtin == t.builtin)
            return &t;
    return nullptr;
}

} // namespace

bool getNativeField(const ScriptObject& o, const std::string& name, Value& out) {
    const NativeTypeEntry* t = findNativeType(o.builtin);
    if (!t)
        return false;
    for (size_t i = 0; i < t->fieldCount; ++i)
        if (name == t->fields[i].name) {
            out = t->fields[i].get(o.nativePtr);
            return true;
        }
    return false;
}

bool setNativeField(ScriptObject& o, const std::string& name, const Value& v) {
    const NativeTypeEntry* t = findNativeType(o.builtin);
    if (!t)
        return false;
    for (size_t i = 0; i < t->fieldCount; ++i)
        if (name == t->fields[i].name)
            return t->fields[i].set(o.nativePtr, v, o.owner);
    return false;
}

// ---------------------------------------------------------------------------
// Camera.main resolution (task 77).
// ---------------------------------------------------------------------------

namespace {

CameraComponent* findCameraComp(crate::Actor& node, bool requireEnabled, CameraComponent* exclude) {
    if (auto* cc = node.getComponent<CameraComponent>())
        if (cc != exclude && (!requireEnabled || cc->enabled))
            return cc;
    for (const auto& c : node.children())
        if (CameraComponent* hit = findCameraComp(*c, requireEnabled, exclude))
            return hit;
    return nullptr;
}

void disableOtherCameraComps(crate::Actor& node, CameraComponent* keep) {
    if (auto* cc = node.getComponent<CameraComponent>())
        if (cc != keep)
            cc->enabled = false;
    for (const auto& c : node.children())
        disableOtherCameraComps(*c, keep);
}

crate::Actor* sceneRootOf(crate::Actor* a) {
    while (a && a->parent())
        a = a->parent();
    return a;
}

} // namespace

CameraComponent* mainCameraFrom(crate::Actor* any) {
    crate::Actor* root = sceneRootOf(any);
    if (!root)
        return nullptr;
    for (const auto& child : root->children())
        if (CameraComponent* hit = findCameraComp(*child, /*requireEnabled=*/true, nullptr))
            return hit;
    return nullptr;
}

void activateMainCamera(crate::Actor* any, CameraComponent& cam) {
    crate::Actor* root = sceneRootOf(any);
    if (!root)
        return;
    for (const auto& child : root->children())
        disableOtherCameraComps(*child, &cam);
    cam.enabled = true;
}

} // namespace crate::script
