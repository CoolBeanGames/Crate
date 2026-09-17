#include "script/Runtime.h"

#include "core/Math.h"
#include "scene/Actor.h"
#include "scene/BuiltinComponents.h"
#include "script/Interpreter.h" // ScriptContext's full definition (inputQuery)

#include <cmath>
#include <cstdlib>
#include <random>
#include <utility>

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

void vfieldSet(const Value& v, const char* f, double x) {
    if (!isVec(v))
        return;
    v.obj->fields[f] = Value::Float(x);
}

Value negate(const Value& a) {
    return a.t == Value::T::Int ? Value::Int(-a.i) : Value::Float(-a.num());
}

Value valueEquals(const Value& a, const Value& b) {
    if (a.isNumeric() && b.isNumeric())
        return Value::Bool(a.num() == b.num());
    return Value::Bool(a.str() == b.str() && a.t == b.t);
}

Value valueNotEquals(const Value& a, const Value& b) {
    if (a.isNumeric() && b.isNumeric())
        return Value::Bool(a.num() != b.num());
    return Value::Bool(!(a.str() == b.str() && a.t == b.t));
}

Value valueLess(const Value& a, const Value& b) { return Value::Bool(a.num() < b.num()); }
Value valueGreater(const Value& a, const Value& b) { return Value::Bool(a.num() > b.num()); }
Value valueLessEq(const Value& a, const Value& b) { return Value::Bool(a.num() <= b.num()); }
Value valueGreaterEq(const Value& a, const Value& b) { return Value::Bool(a.num() >= b.num()); }

Value arrayLength(const Value& v) {
    return Value::Int(v.t == Value::T::Array && v.arr ? (long long)v.arr->size() : 0);
}

bool arrayAdd(const Value& v, const Value& item) {
    if (v.t != Value::T::Array || !v.arr)
        return false;
    v.arr->push_back(item);
    return true;
}

Value mathCall(const std::string& fn, std::vector<Value>& args, int line) {
    static std::mt19937 rng{std::random_device{}()};
    auto n = [&](size_t i) { return i < args.size() ? args[i].num() : 0.0; };
    auto allInt = [&]() {
        for (const auto& a : args)
            if (a.t != Value::T::Int)
                return false;
        return !args.empty();
    };

    if (fn == "clamp") {
        double v = n(0), lo = n(1), hi = n(2);
        double r = v < lo ? lo : (v > hi ? hi : v);
        return allInt() ? Value::Int((long long)r) : Value::Float(r);
    }
    if (fn == "lerp")
        return Value::Float(n(0) + (n(1) - n(0)) * n(2));
    if (fn == "sine" || fn == "sin")
        return Value::Float(std::sin(n(0)));
    if (fn == "cos" || fn == "cosine")
        return Value::Float(std::cos(n(0)));
    if (fn == "tan")
        return Value::Float(std::tan(n(0)));
    if (fn == "sqrt")
        return Value::Float(std::sqrt(n(0)));
    if (fn == "exp")
        return Value::Float(std::exp(n(0)));
    if (fn == "pow")
        return Value::Float(std::pow(n(0), n(1)));
    if (fn == "abs")
        return args.size() && args[0].t == Value::T::Int ? Value::Int(std::llabs(args[0].i))
                                                         : Value::Float(std::fabs(n(0)));
    if (fn == "floor")
        return Value::Float(std::floor(n(0)));
    if (fn == "ceil")
        return Value::Float(std::ceil(n(0)));
    if (fn == "round")
        return Value::Float(std::round(n(0)));
    if (fn == "min") {
        double r = n(0) < n(1) ? n(0) : n(1);
        return allInt() ? Value::Int((long long)r) : Value::Float(r);
    }
    if (fn == "max") {
        double r = n(0) > n(1) ? n(0) : n(1);
        return allInt() ? Value::Int((long long)r) : Value::Float(r);
    }
    if (fn == "deg2rad")
        return Value::Float(n(0) * (kPi / 180.0));
    if (fn == "rad2deg")
        return Value::Float(n(0) * (180.0 / kPi));
    if (fn == "rand_f")
        return Value::Float(std::uniform_real_distribution<double>(0.0, 1.0)(rng));
    if (fn == "rand_i")
        return Value::Int(std::uniform_int_distribution<long long>(0, 0x7fffffff)(rng));
    if (fn == "rand_f_range")
        return Value::Float(std::uniform_real_distribution<double>(n(0), n(1))(rng));
    if (fn == "rand_i_range") {
        long long lo = (long long)n(0), hi = (long long)n(1);
        if (hi < lo)
            std::swap(lo, hi);
        return Value::Int(std::uniform_int_distribution<long long>(lo, hi)(rng)); // inclusive
    }

    throw RuntimeError("Math has no function '" + fn + "'", line);
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

const NativeFieldEntry kTransformFields[] = {
    {"position",
     [](void* p) -> Value {
         auto* t = static_cast<Transform*>(p);
         return makeVector("Vector3", t->position.x, t->position.y, t->position.z);
     },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         auto* t = static_cast<Transform*>(p);
         if (v.t == Value::T::Object && v.obj) {
             t->position.x = (float)v.obj->fields["x"].num();
             t->position.y = (float)v.obj->fields["y"].num();
             t->position.z = (float)v.obj->fields["z"].num();
         }
         return true;
     }},
    {"rotation",
     [](void* p) -> Value {
         auto* t = static_cast<Transform*>(p);
         return makeVector("Vector3", t->rotationEuler.x, t->rotationEuler.y, t->rotationEuler.z);
     },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         auto* t = static_cast<Transform*>(p);
         if (v.t == Value::T::Object && v.obj) {
             t->rotationEuler.x = (float)v.obj->fields["x"].num();
             t->rotationEuler.y = (float)v.obj->fields["y"].num();
             t->rotationEuler.z = (float)v.obj->fields["z"].num();
         }
         return true;
     }},
    {"scale",
     [](void* p) -> Value {
         auto* t = static_cast<Transform*>(p);
         return makeVector("Vector3", t->scale.x, t->scale.y, t->scale.z);
     },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         auto* t = static_cast<Transform*>(p);
         if (v.t == Value::T::Object && v.obj) {
             t->scale.x = (float)v.obj->fields["x"].num();
             t->scale.y = (float)v.obj->fields["y"].num();
             t->scale.z = (float)v.obj->fields["z"].num();
         }
         return true;
     }},
};

const NativeFieldEntry kLightFields[] = {
    {"type",
     [](void* p) -> Value { return Value::Int((long long)static_cast<LightComponent*>(p)->type); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<LightComponent*>(p)->type = (LightComponent::Type)v.num();
         return true;
     }},
    {"color",
     [](void* p) -> Value {
         auto* lc = static_cast<LightComponent*>(p);
         return makeVector("Vector3", lc->color[0], lc->color[1], lc->color[2]);
     },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         auto* lc = static_cast<LightComponent*>(p);
         if (v.t == Value::T::Object && v.obj) {
             lc->color[0] = (float)v.obj->fields["x"].num();
             lc->color[1] = (float)v.obj->fields["y"].num();
             lc->color[2] = (float)v.obj->fields["z"].num();
         }
         return true;
     }},
    {"intensity",
     [](void* p) -> Value { return Value::Float(static_cast<LightComponent*>(p)->intensity); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<LightComponent*>(p)->intensity = (float)v.num();
         return true;
     }},
    {"range",
     [](void* p) -> Value { return Value::Float(static_cast<LightComponent*>(p)->range); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<LightComponent*>(p)->range = (float)v.num();
         return true;
     }},
    {"spot_inner_deg",
     [](void* p) -> Value { return Value::Float(static_cast<LightComponent*>(p)->spotInnerDeg); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<LightComponent*>(p)->spotInnerDeg = (float)v.num();
         return true;
     }},
    {"spot_outer_deg",
     [](void* p) -> Value { return Value::Float(static_cast<LightComponent*>(p)->spotOuterDeg); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<LightComponent*>(p)->spotOuterDeg = (float)v.num();
         return true;
     }},
    {"is_static",
     [](void* p) -> Value { return Value::Bool(static_cast<LightComponent*>(p)->isStatic); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<LightComponent*>(p)->isStatic = v.truthy();
         return true;
     }},
};

const NativeFieldEntry kVolumetricFogFields[] = {
    {"color",
     [](void* p) -> Value {
         auto* vf = static_cast<VolumetricFogComponent*>(p);
         return makeVector("Vector3", vf->color[0], vf->color[1], vf->color[2]);
     },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         auto* vf = static_cast<VolumetricFogComponent*>(p);
         if (v.t == Value::T::Object && v.obj) {
             vf->color[0] = (float)v.obj->fields["x"].num();
             vf->color[1] = (float)v.obj->fields["y"].num();
             vf->color[2] = (float)v.obj->fields["z"].num();
         }
         return true;
     }},
    {"density",
     [](void* p) -> Value { return Value::Float(static_cast<VolumetricFogComponent*>(p)->density); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<VolumetricFogComponent*>(p)->density = (float)v.num();
         return true;
     }},
};

const NativeFieldEntry kMeshRendererFields[] = {
    {"tint",
     [](void* p) -> Value {
         auto* mr = static_cast<MeshRenderer*>(p);
         return makeVector("Vector3", mr->tint[0], mr->tint[1], mr->tint[2]);
     },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         auto* mr = static_cast<MeshRenderer*>(p);
         if (v.t == Value::T::Object && v.obj) {
             mr->tint[0] = (float)v.obj->fields["x"].num();
             mr->tint[1] = (float)v.obj->fields["y"].num();
             mr->tint[2] = (float)v.obj->fields["z"].num();
         }
         return true;
     }},
    {"cast_shadows",
     [](void* p) -> Value { return Value::Bool(static_cast<MeshRenderer*>(p)->castShadows); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<MeshRenderer*>(p)->castShadows = v.truthy();
         return true;
     }},
    {"receive_shadows",
     [](void* p) -> Value { return Value::Bool(static_cast<MeshRenderer*>(p)->receiveShadows); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<MeshRenderer*>(p)->receiveShadows = v.truthy();
         return true;
     }},
};

const NativeFieldEntry kLightProbeFields[] = {
    {"baked_light",
     [](void* p) -> Value {
         auto* lp = static_cast<LightProbeComponent*>(p);
         return makeVector("Vector3", lp->bakedLight[0], lp->bakedLight[1], lp->bakedLight[2]);
     },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         auto* lp = static_cast<LightProbeComponent*>(p);
         if (v.t == Value::T::Object && v.obj) {
             lp->bakedLight[0] = (float)v.obj->fields["x"].num();
             lp->bakedLight[1] = (float)v.obj->fields["y"].num();
             lp->bakedLight[2] = (float)v.obj->fields["z"].num();
         }
         return true;
     }},
    {"baked_valid",
     [](void* p) -> Value { return Value::Bool(static_cast<LightProbeComponent*>(p)->bakedValid); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<LightProbeComponent*>(p)->bakedValid = v.truthy();
         return true;
     }},
};

const NativeFieldEntry kSpinnerFields[] = {
    {"degrees_per_second",
     [](void* p) -> Value { return Value::Float(static_cast<SpinnerComponent*>(p)->degreesPerSecond); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<SpinnerComponent*>(p)->degreesPerSecond = (float)v.num();
         return true;
     }},
    {"axis",
     [](void* p) -> Value { return Value::Int(static_cast<SpinnerComponent*>(p)->axis); },
     [](void* p, const Value& v, crate::Actor*) -> bool {
         static_cast<SpinnerComponent*>(p)->axis = (int)v.num();
         return true;
     }},
};

const NativeTypeEntry kNativeTypes[] = {
    {"Fog", kFogFields, sizeof(kFogFields) / sizeof(kFogFields[0])},
    {"Camera", kCameraFields, sizeof(kCameraFields) / sizeof(kCameraFields[0])},
    {"Transform", kTransformFields, sizeof(kTransformFields) / sizeof(kTransformFields[0])},
    {"Light", kLightFields, sizeof(kLightFields) / sizeof(kLightFields[0])},
    {"VolumetricFog", kVolumetricFogFields, sizeof(kVolumetricFogFields) / sizeof(kVolumetricFogFields[0])},
    {"MeshRenderer", kMeshRendererFields, sizeof(kMeshRendererFields) / sizeof(kMeshRendererFields[0])},
    {"LightProbe", kLightProbeFields, sizeof(kLightProbeFields) / sizeof(kLightProbeFields[0])},
    {"Spinner", kSpinnerFields, sizeof(kSpinnerFields) / sizeof(kSpinnerFields[0])},
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

crate::Actor* sceneRootOf(crate::Actor* a) {
    while (a && a->parent())
        a = a->parent();
    return a;
}

Value inputMouseMember(ScriptContext* ctx, const std::string& name, int line) {
    auto iq = [&](const std::string& btn, int what) -> double {
        return ctx && ctx->inputQuery ? ctx->inputQuery(btn, what) : 0.0;
    };
    if (name == "delta")
        return makeVector("Vector2", iq("", 5), iq("", 6), 0);
    if (name == "scroll")
        return makeVector("Vector2", iq("", 7), iq("", 8), 0);
    struct BtnMap {
        const char* member;
        const char* button;
        int what;
    };
    static const BtnMap kMap[] = {
        {"left_down", "mouse_left", 0},        {"left_just_down", "mouse_left", 1},
        {"left_just_up", "mouse_left", 2},     {"right_down", "mouse_right", 0},
        {"right_just_down", "mouse_right", 1}, {"right_just_up", "mouse_right", 2},
        {"middle_down", "mouse_middle", 0},    {"middle_just_down", "mouse_middle", 1},
        {"middle_just_up", "mouse_middle", 2},
    };
    for (const auto& m : kMap)
        if (name == m.member)
            return Value::Bool(iq(m.button, m.what) != 0.0);
    throw RuntimeError("Input.Mouse has no member '" + name + "'", line);
}

Value actorMember(crate::Actor* a, const std::string& name, int line) {
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
    // Derived direction vectors, read-only: +Z/+X/+Y rotated by the transform's
    // Euler rotation (matches the +Z-forward convention used elsewhere, e.g.
    // LightComponent's directional "forward" arrow).
    if (name == "forward" || name == "right" || name == "up") {
        Mat4 rot = Mat4::rotationEuler(t.rotationEuler);
        Vec3 base = name == "forward" ? Vec3{0, 0, 1} : name == "right" ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        Vec3 dir = normalize(transformDirection(base, rot));
        return makeVector("Vector3", dir.x, dir.y, dir.z);
    }
    throw RuntimeError("Actor has no member '" + name + "'", line);
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
