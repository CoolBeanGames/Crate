// Exhaustive coverage for src/script/Runtime.h/.cpp -- the stateless script
// runtime extracted from Interpreter.cpp (see transpiration.txt, Phase 0).
// Every function gets its own section; every value-conversion path gets its
// own explicit case (per the project's "test everything, a bajillion tests"
// policy) rather than one representative example per function.
//
// No framework: asserts + a pass counter, run via CTest (matches every other
// test file in this repo).

#include "script/Runtime.h"

#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using namespace crate;
using namespace crate::script;

static int g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            return 1;                                                                \
        }                                                                           \
    } while (0)

#define CHECK_THROWS_RUNTIME_ERROR(expr)                                             \
    do {                                                                             \
        ++g_checks;                                                                  \
        bool threw = false;                                                         \
        try {                                                                        \
            (void)(expr);                                                           \
        } catch (const RuntimeError&) {                                             \
            threw = true;                                                          \
        }                                                                            \
        if (!threw) {                                                               \
            std::printf("FAIL %s:%d  expected RuntimeError from: %s\n", __FILE__,   \
                        __LINE__, #expr);                                          \
            return 1;                                                              \
        }                                                                           \
    } while (0)

static int testCoerce() {
    // Empty declared type: value passes through completely unchanged.
    CHECK(coerce(Value::Int(7), "").t == Value::T::Int);
    CHECK(coerce(Value::Int(7), "").i == 7);
    CHECK(coerce(Value::Str("hi"), "").s == "hi");

    // -> "int": every source type, including the ones that don't obviously
    // convert (num() defaults to 0.0 for non-numeric types).
    CHECK(coerce(Value::Int(5), "int").t == Value::T::Int);
    CHECK(coerce(Value::Int(5), "int").i == 5);
    CHECK(coerce(Value::Float(3.9), "int").i == 3); // truncates, does not round
    CHECK(coerce(Value::Float(-3.9), "int").i == -3); // truncation toward zero
    CHECK(coerce(Value::Bool(true), "int").i == 1);
    CHECK(coerce(Value::Bool(false), "int").i == 0);
    CHECK(coerce(Value::Char('A'), "int").i == 65);
    CHECK(coerce(Value::Str("anything"), "int").i == 0); // String.num() == 0
    CHECK(coerce(Value::Null_(), "int").i == 0);
    CHECK(coerce(Value::Arr({Value::Int(1)}), "int").i == 0); // Array.num() == 0

    // -> "float"
    CHECK(coerce(Value::Int(5), "float").t == Value::T::Float);
    CHECK(coerce(Value::Int(5), "float").f == 5.0);
    CHECK(coerce(Value::Float(2.5), "float").f == 2.5);
    CHECK(coerce(Value::Bool(true), "float").f == 1.0);
    CHECK(coerce(Value::Char('A'), "float").f == 65.0);
    CHECK(coerce(Value::Str("x"), "float").f == 0.0);

    // -> "bool"
    CHECK(coerce(Value::Int(0), "bool").b == false);
    CHECK(coerce(Value::Int(1), "bool").b == true);
    CHECK(coerce(Value::Int(-1), "bool").b == true);
    CHECK(coerce(Value::Float(0.0), "bool").b == false);
    CHECK(coerce(Value::Str(""), "bool").b == false);
    CHECK(coerce(Value::Str("x"), "bool").b == true);
    CHECK(coerce(Value::Null_(), "bool").b == false);
    CHECK(coerce(Value::Arr({}), "bool").b == false);      // empty array: falsy
    CHECK(coerce(Value::Arr({Value::Int(1)}), "bool").b == true); // non-empty: truthy

    // -> "string"
    CHECK(coerce(Value::Str("already"), "string").t == Value::T::String);
    CHECK(coerce(Value::Str("already"), "string").s == "already"); // passthrough, not re-wrapped
    CHECK(coerce(Value::Int(42), "string").t == Value::T::String);
    CHECK(coerce(Value::Int(42), "string").s == "42");
    CHECK(coerce(Value::Bool(true), "string").s == "true");
    CHECK(coerce(Value::Bool(false), "string").s == "false");
    CHECK(coerce(Value::Float(1.5), "string").s == "1.5");
    CHECK(coerce(Value::Char('Q'), "string").s == "Q");
    CHECK(coerce(Value::Null_(), "string").s == "null");

    // -> "char"
    CHECK(coerce(Value::Char('Z'), "char").t == Value::T::Char);
    CHECK(coerce(Value::Char('Z'), "char").s == "Z"); // passthrough, not re-wrapped
    CHECK(coerce(Value::Str("hello"), "char").s == "h"); // first character only
    CHECK(coerce(Value::Str(""), "char").s == std::string(1, '\0')); // empty -> NUL char
    // A non-string/char source goes through str() first, so an Int becomes
    // the first *digit character* of its decimal text, not its numeric byte
    // value -- an easy trap for anyone assuming char-from-int semantics.
    CHECK(coerce(Value::Int(65), "char").s == "6");
    CHECK(coerce(Value::Int(5), "char").s == "5");

    // Unknown / non-primitive declared type: value passes through completely
    // unchanged (Actor / Vector2 / Vector3 / arrays / class types).
    Value arr = Value::Arr({Value::Int(1), Value::Int(2)});
    Value coercedArr = coerce(arr, "int[]");
    CHECK(coercedArr.t == Value::T::Array);
    CHECK(coercedArr.arr->size() == 2);
    CHECK(coerce(Value::ActorRef(nullptr), "Actor").t == Value::T::Actor);
    CHECK(coerce(makeVector("Vector3", 1, 2, 3), "Vector3").t == Value::T::Object);

    return 0;
}

static int testIsVecAndVfield() {
    Value v2 = makeVector("Vector2", 1.0, 2.0, 3.0);
    Value v3 = makeVector("Vector3", 1.0, 2.0, 3.0);
    CHECK(isVec(v2));
    CHECK(isVec(v3));

    CHECK(!isVec(Value::Int(1)));
    CHECK(!isVec(Value::Float(1.0)));
    CHECK(!isVec(Value::Str("Vector3"))); // a string named "Vector3" is not a vector
    CHECK(!isVec(Value::Null_()));
    CHECK(!isVec(Value::Bool(true)));

    // An Object whose builtin isn't Vector2/Vector3 is not a vector (e.g. the
    // "live view" objects getNativeField/setNativeField work with).
    auto fogLike = std::make_shared<ScriptObject>();
    fogLike->builtin = "Fog";
    CHECK(!isVec(Value::Obj(fogLike)));

    // An Object with cls set (a real script-class instance) and empty
    // builtin is not a vector either.
    auto scriptLike = std::make_shared<ScriptObject>();
    CHECK(!isVec(Value::Obj(scriptLike))); // builtin == "" by default

    // vfield: present field returns its numeric value.
    CHECK(vfield(v3, "x") == 1.0);
    CHECK(vfield(v3, "y") == 2.0);
    CHECK(vfield(v3, "z") == 3.0);
    // vfield: absent field returns 0.0 rather than throwing.
    auto sparse = std::make_shared<ScriptObject>();
    sparse->builtin = "Vector3";
    sparse->fields["x"] = Value::Float(9.0);
    Value sparseVal = Value::Obj(sparse);
    CHECK(vfield(sparseVal, "x") == 9.0);
    CHECK(vfield(sparseVal, "y") == 0.0);
    CHECK(vfield(sparseVal, "z") == 0.0);
    CHECK(vfield(sparseVal, "nonexistent") == 0.0);

    // vfieldSet: writes a single component in place.
    Value writable = makeVector("Vector3", 1.0, 2.0, 3.0);
    vfieldSet(writable, "x", 99.0);
    CHECK(vfield(writable, "x") == 99.0);
    CHECK(vfield(writable, "y") == 2.0); // other components untouched
    vfieldSet(writable, "y", -5.0);
    vfieldSet(writable, "z", 0.25);
    CHECK(vfield(writable, "y") == -5.0);
    CHECK(vfield(writable, "z") == 0.25);
    // A brand-new field name not already present is simply added (matches
    // ScriptObject::fields being a plain unordered_map).
    vfieldSet(writable, "w", 7.0);
    CHECK(vfield(writable, "w") == 7.0);

    // vfieldSet on a non-vector Value is a silent no-op, not a crash.
    Value notVec = Value::Int(5);
    vfieldSet(notVec, "x", 1.0); // must not crash
    CHECK(notVec.t == Value::T::Int);
    CHECK(notVec.i == 5); // unchanged

    // Aliasing: two Values sharing the same underlying ScriptObject (as
    // produced by plain copy, since Value::obj is a shared_ptr) see a
    // vfieldSet through either handle -- this is the deliberately-preserved
    // reference-semantics behavior described in transpiration.txt.
    Value original = makeVector("Vector3", 1.0, 1.0, 1.0);
    Value alias = original; // shares the same ScriptObject
    vfieldSet(alias, "x", 42.0);
    CHECK(vfield(original, "x") == 42.0); // visible through the other handle too

    return 0;
}

static int testMakeVector() {
    Value v3 = makeVector("Vector3", 1.5, 2.5, 3.5);
    CHECK(v3.t == Value::T::Object);
    CHECK(v3.obj->builtin == "Vector3");
    CHECK(v3.obj->fields["x"].f == 1.5);
    CHECK(v3.obj->fields["y"].f == 2.5);
    CHECK(v3.obj->fields["z"].f == 3.5);

    // Vector2 always forces z to 0.0 regardless of what's passed in.
    Value v2 = makeVector("Vector2", 4.0, 5.0, 999.0);
    CHECK(v2.t == Value::T::Object);
    CHECK(v2.obj->builtin == "Vector2");
    CHECK(v2.obj->fields["x"].f == 4.0);
    CHECK(v2.obj->fields["y"].f == 5.0);
    CHECK(v2.obj->fields["z"].f == 0.0);

    // Every field is stored as Float, never Int, even for whole numbers.
    Value whole = makeVector("Vector3", 1.0, 0.0, -1.0);
    CHECK(whole.obj->fields["x"].t == Value::T::Float);
    CHECK(whole.obj->fields["y"].t == Value::T::Float);
    CHECK(whole.obj->fields["z"].t == Value::T::Float);

    return 0;
}

static int testArithScalar() {
    // Int op Int stays Int.
    CHECK(arith(Tok::Plus, Value::Int(2), Value::Int(3), 1).t == Value::T::Int);
    CHECK(arith(Tok::Plus, Value::Int(2), Value::Int(3), 1).i == 5);
    CHECK(arith(Tok::Minus, Value::Int(5), Value::Int(3), 1).i == 2);
    CHECK(arith(Tok::Star, Value::Int(4), Value::Int(3), 1).i == 12);
    CHECK(arith(Tok::Slash, Value::Int(7), Value::Int(2), 1).t == Value::T::Int);
    CHECK(arith(Tok::Slash, Value::Int(7), Value::Int(2), 1).i == 3); // integer division truncates
    CHECK(arith(Tok::Percent, Value::Int(7), Value::Int(2), 1).i == 1);
    CHECK(arith(Tok::Percent, Value::Int(-7), Value::Int(2), 1).i == -1); // C++ % semantics

    // Any Float operand promotes the result to Float.
    CHECK(arith(Tok::Plus, Value::Float(2.5), Value::Int(1), 1).t == Value::T::Float);
    CHECK(arith(Tok::Plus, Value::Float(2.5), Value::Int(1), 1).f == 3.5);
    CHECK(arith(Tok::Plus, Value::Int(1), Value::Float(2.5), 1).f == 3.5);
    CHECK(arith(Tok::Minus, Value::Float(5.5), Value::Float(2.0), 1).f == 3.5);
    CHECK(arith(Tok::Star, Value::Float(2.0), Value::Float(3.0), 1).f == 6.0);
    CHECK(arith(Tok::Slash, Value::Float(7.0), Value::Float(2.0), 1).f == 3.5);
    CHECK(arith(Tok::Percent, Value::Float(7.5), Value::Float(2.0), 1).f == 1.5); // fmod

    // Bool operands are numeric (num() -> 0.0/1.0) and produce Float results
    // since bothInt requires both operands to literally be T::Int.
    CHECK(arith(Tok::Plus, Value::Bool(true), Value::Bool(true), 1).t == Value::T::Float);
    CHECK(arith(Tok::Plus, Value::Bool(true), Value::Bool(true), 1).f == 2.0);

    // Division/modulo by zero throw, for both Int and Float operands.
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Slash, Value::Int(1), Value::Int(0), 1));
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Slash, Value::Float(1.0), Value::Float(0.0), 1));
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Percent, Value::Int(1), Value::Int(0), 1));
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Percent, Value::Float(1.0), Value::Float(0.0), 1));

    // An operator arith() doesn't handle (e.g. Lt -- normally intercepted
    // earlier in Interpreter::evalBinary before arith() is ever called)
    // throws "bad operator" rather than silently doing something wrong.
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Lt, Value::Int(1), Value::Int(2), 1));
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::AndAnd, Value::Int(1), Value::Int(2), 1));

    return 0;
}

static int testArithStringConcat() {
    // '+' with a String on either side concatenates via str(), regardless of
    // the other operand's type.
    CHECK(arith(Tok::Plus, Value::Str("a"), Value::Str("b"), 1).s == "ab");
    CHECK(arith(Tok::Plus, Value::Str("x="), Value::Int(5), 1).s == "x=5");
    CHECK(arith(Tok::Plus, Value::Int(5), Value::Str("=x"), 1).s == "5=x");
    CHECK(arith(Tok::Plus, Value::Str("v="), Value::Float(1.5), 1).s == "v=1.5");
    CHECK(arith(Tok::Plus, Value::Str("b="), Value::Bool(true), 1).s == "b=true");

    // '+' with a Char on either side ALSO takes the string-concat path, even
    // when the other operand is a plain number -- easy to miss since it
    // looks like it should be numeric ASCII addition.
    CHECK(arith(Tok::Plus, Value::Char('a'), Value::Char('b'), 1).t == Value::T::String);
    CHECK(arith(Tok::Plus, Value::Char('a'), Value::Char('b'), 1).s == "ab");
    CHECK(arith(Tok::Plus, Value::Int(5), Value::Char('a'), 1).t == Value::T::String);
    CHECK(arith(Tok::Plus, Value::Int(5), Value::Char('a'), 1).s == "5a");
    CHECK(arith(Tok::Plus, Value::Char('a'), Value::Int(5), 1).s == "a5");

    // Minus/Star/Slash with a String operand are NOT special-cased by the
    // string-concat branch (only Plus is) -- they fall through to num()-based
    // arithmetic, where a String's num() is 0.0.
    CHECK(arith(Tok::Minus, Value::Str("5"), Value::Int(2), 1).t == Value::T::Float);
    CHECK(arith(Tok::Minus, Value::Str("5"), Value::Int(2), 1).f == -2.0); // 0 - 2

    return 0;
}

static int testArithVector() {
    Value a3 = makeVector("Vector3", 1.0, 2.0, 3.0);
    Value b3 = makeVector("Vector3", 10.0, 20.0, 30.0);
    Value a2 = makeVector("Vector2", 1.0, 2.0, 0.0);
    Value b2 = makeVector("Vector2", 10.0, 20.0, 0.0);

    // Vector + Vector: component-wise, same kind in -> same kind out.
    Value sum3 = arith(Tok::Plus, a3, b3, 1);
    CHECK(sum3.obj->builtin == "Vector3");
    CHECK(vfield(sum3, "x") == 11.0);
    CHECK(vfield(sum3, "y") == 22.0);
    CHECK(vfield(sum3, "z") == 33.0);

    Value sum2 = arith(Tok::Plus, a2, b2, 1);
    CHECK(sum2.obj->builtin == "Vector2");
    CHECK(vfield(sum2, "x") == 11.0);
    CHECK(vfield(sum2, "y") == 22.0);

    // Vector2 mixed with Vector3: result kind is Vector3 if EITHER side is
    // Vector3 -- verified from both operand positions.
    Value mixedAB = arith(Tok::Plus, a2, b3, 1);
    CHECK(mixedAB.obj->builtin == "Vector3");
    Value mixedBA = arith(Tok::Plus, a3, b2, 1);
    CHECK(mixedBA.obj->builtin == "Vector3");

    // Vector - Vector, Vector * Vector (component-wise, not dot/cross product).
    Value diff = arith(Tok::Minus, b3, a3, 1);
    CHECK(vfield(diff, "x") == 9.0);
    CHECK(vfield(diff, "y") == 18.0);
    CHECK(vfield(diff, "z") == 27.0);
    Value prod = arith(Tok::Star, a3, b3, 1);
    CHECK(vfield(prod, "x") == 10.0);
    CHECK(vfield(prod, "y") == 40.0);
    CHECK(vfield(prod, "z") == 90.0);

    // Vector / Vector (component-wise).
    Value quot = arith(Tok::Slash, b3, a3, 1);
    CHECK(vfield(quot, "x") == 10.0);
    CHECK(vfield(quot, "y") == 10.0);
    CHECK(vfield(quot, "z") == 10.0);

    // Vector op scalar broadcasts the scalar to every component, from either
    // operand position.
    Value scaled = arith(Tok::Star, a3, Value::Float(2.0), 1);
    CHECK(vfield(scaled, "x") == 2.0);
    CHECK(vfield(scaled, "y") == 4.0);
    CHECK(vfield(scaled, "z") == 6.0);
    Value scaledRev = arith(Tok::Star, Value::Float(2.0), a3, 1);
    CHECK(vfield(scaledRev, "x") == 2.0);
    CHECK(vfield(scaledRev, "y") == 4.0);
    CHECK(vfield(scaledRev, "z") == 6.0);

    // Vector / zero-component vector throws, checked per-component
    // (x, y, and for Vector3 also z).
    Value zeroX = makeVector("Vector3", 0.0, 1.0, 1.0);
    Value zeroY = makeVector("Vector3", 1.0, 0.0, 1.0);
    Value zeroZ = makeVector("Vector3", 1.0, 1.0, 0.0);
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Slash, a3, zeroX, 1));
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Slash, a3, zeroY, 1));
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Slash, a3, zeroZ, 1));
    // Vector2 division ignores z entirely (z is always 0 for both operands,
    // so it must not spuriously trigger the zero-check for Vector2).
    Value okDiv2 = arith(Tok::Slash, b2, a2, 1);
    CHECK(vfield(okDiv2, "x") == 10.0);
    CHECK(vfield(okDiv2, "y") == 10.0);

    // An operator the vector branch doesn't support (e.g. modulo) throws.
    CHECK_THROWS_RUNTIME_ERROR(arith(Tok::Percent, a3, b3, 1));

    return 0;
}

static int testGetSetNativeFieldFog() {
    FogComponent fog;
    fog.color[0] = 0.1f;
    fog.color[1] = 0.2f;
    fog.color[2] = 0.3f;
    fog.start = 5.0f;
    fog.end = 40.0f;
    fog.heightRange = 2.0f;

    auto liveView = std::make_shared<ScriptObject>();
    liveView->builtin = "Fog";
    liveView->nativePtr = &fog;

    Value out;
    CHECK(getNativeField(*liveView, "start", out));
    CHECK(out.f == 5.0);
    CHECK(getNativeField(*liveView, "end", out));
    CHECK(out.f == 40.0);
    CHECK(getNativeField(*liveView, "height_range", out));
    CHECK(out.f == 2.0);
    CHECK(getNativeField(*liveView, "color", out));
    CHECK(out.obj->builtin == "Vector3");
    CHECK(vfield(out, "x") == (double)0.1f);
    CHECK(vfield(out, "y") == (double)0.2f);
    CHECK(vfield(out, "z") == (double)0.3f);

    // Unknown field name on a known type: not found.
    CHECK(!getNativeField(*liveView, "bogus", out));

    // set: scalar fields mutate the real component.
    CHECK(setNativeField(*liveView, "start", Value::Float(9.0)));
    CHECK(fog.start == 9.0f);
    CHECK(setNativeField(*liveView, "end", Value::Float(99.0)));
    CHECK(fog.end == 99.0f);
    CHECK(setNativeField(*liveView, "height_range", Value::Float(3.0)));
    CHECK(fog.heightRange == 3.0f);

    // set "color" with a proper Vector3 mutates all three components.
    Value newColor = makeVector("Vector3", 0.9, 0.8, 0.7);
    CHECK(setNativeField(*liveView, "color", newColor));
    CHECK(fog.color[0] == (float)0.9);
    CHECK(fog.color[1] == (float)0.8);
    CHECK(fog.color[2] == (float)0.7);

    // set "color" with a non-Object value: still reports handled (true), but
    // performs no mutation -- matches the original hand-written behavior.
    float before[3] = {fog.color[0], fog.color[1], fog.color[2]};
    CHECK(setNativeField(*liveView, "color", Value::Int(123)));
    CHECK(fog.color[0] == before[0]);
    CHECK(fog.color[1] == before[1]);
    CHECK(fog.color[2] == before[2]);

    // Unknown field name: not handled.
    CHECK(!setNativeField(*liveView, "bogus", Value::Int(1)));

    return 0;
}

static int testGetSetNativeFieldCameraScalars() {
    CameraComponent cam;
    cam.fovY = 55.0f;
    cam.nearZ = 0.1f;
    cam.farZ = 100.0f;
    cam.enabled = true;

    auto liveView = std::make_shared<ScriptObject>();
    liveView->builtin = "Camera";
    liveView->nativePtr = &cam;
    liveView->owner = nullptr; // scalar fields don't need an owner

    Value out;
    CHECK(getNativeField(*liveView, "fov", out));
    CHECK(out.f == 55.0);
    CHECK(getNativeField(*liveView, "near", out));
    CHECK(out.f == (double)0.1f);
    CHECK(getNativeField(*liveView, "far", out));
    CHECK(out.f == 100.0);
    CHECK(getNativeField(*liveView, "active", out));
    CHECK(out.t == Value::T::Bool);
    CHECK(out.b == true);

    CHECK(setNativeField(*liveView, "fov", Value::Float(90.0)));
    CHECK(cam.fovY == 90.0f);
    CHECK(setNativeField(*liveView, "near", Value::Float(0.5)));
    CHECK(cam.nearZ == 0.5f);
    CHECK(setNativeField(*liveView, "far", Value::Float(500.0)));
    CHECK(cam.farZ == 500.0f);

    // "active" = false with a null owner: the original code's `else if
    // (o.owner)` guard means NOTHING happens (not even a direct disable) --
    // an easy-to-miss edge case worth locking down explicitly.
    cam.enabled = true;
    CHECK(setNativeField(*liveView, "active", Value::Bool(false)));
    CHECK(cam.enabled == true); // unchanged: owner was null

    return 0;
}

static int testGetSetNativeFieldCameraActiveExclusivity() {
    // Build: root -> {a (camera, enabled), b (camera, disabled)}
    Actor3D root("root");
    auto aOwned = std::make_unique<Actor3D>("a");
    auto bOwned = std::make_unique<Actor3D>("b");
    Actor* a = root.addChild(std::move(aOwned));
    Actor* b = root.addChild(std::move(bOwned));

    auto* camA = static_cast<CameraComponent*>(a->addComponent(std::make_unique<CameraComponent>()));
    auto* camB = static_cast<CameraComponent*>(b->addComponent(std::make_unique<CameraComponent>()));
    camA->enabled = true;
    camB->enabled = false;

    auto liveViewB = std::make_shared<ScriptObject>();
    liveViewB->builtin = "Camera";
    liveViewB->nativePtr = camB;
    liveViewB->owner = b;

    // Setting b's "active" to true should enable b and disable every other
    // camera in the tree (here, a).
    CHECK(setNativeField(*liveViewB, "active", Value::Bool(true)));
    CHECK(camB->enabled == true);
    CHECK(camA->enabled == false);

    // Setting active=false with a real owner disables it directly (no
    // fallback search for another camera to activate).
    auto liveViewA = std::make_shared<ScriptObject>();
    liveViewA->builtin = "Camera";
    liveViewA->nativePtr = camA;
    liveViewA->owner = a;
    camA->enabled = true;
    CHECK(setNativeField(*liveViewA, "active", Value::Bool(false)));
    CHECK(camA->enabled == false);
    CHECK(camB->enabled == true); // untouched by a direct disable

    return 0;
}

static int testNativeFieldUnknownBuiltin() {
    ScriptObject anon;
    anon.builtin = "";
    Value out;
    CHECK(!getNativeField(anon, "start", out));
    CHECK(!setNativeField(anon, "start", Value::Int(1)));

    ScriptObject bogus;
    bogus.builtin = "TotallyUnknownType";
    CHECK(!getNativeField(bogus, "anything", out));
    CHECK(!setNativeField(bogus, "anything", Value::Int(1)));

    return 0;
}

static int testMainCameraFrom() {
    // Empty tree, no cameras anywhere: nullptr, not a crash.
    Actor3D lonely("lonely");
    CHECK(mainCameraFrom(&lonely) == nullptr);

    // root -> {a (no camera), b (enabled camera), c (disabled camera)}
    Actor3D root("root");
    Actor* a = root.addChild(std::make_unique<Actor3D>("a"));
    Actor* b = root.addChild(std::make_unique<Actor3D>("b"));
    Actor* c = root.addChild(std::make_unique<Actor3D>("c"));
    auto* camB = static_cast<CameraComponent*>(b->addComponent(std::make_unique<CameraComponent>()));
    auto* camC = static_cast<CameraComponent*>(c->addComponent(std::make_unique<CameraComponent>()));
    camB->enabled = true;
    camC->enabled = false;

    // Reached identically no matter which actor in the tree we start from
    // (sceneRootOf walks up to the root first).
    CHECK(mainCameraFrom(&root) == camB);
    CHECK(mainCameraFrom(a) == camB);
    CHECK(mainCameraFrom(b) == camB);
    CHECK(mainCameraFrom(c) == camB);

    // A disabled-only tree finds nothing.
    camB->enabled = false;
    CHECK(mainCameraFrom(&root) == nullptr);
    camB->enabled = true; // restore for subsequent checks

    // Nested: a camera several levels deep is still found via recursion.
    Actor3D deepRoot("deepRoot");
    Actor* mid = deepRoot.addChild(std::make_unique<Actor3D>("mid"));
    Actor* leaf = mid->addChild(std::make_unique<Actor3D>("leaf"));
    auto* deepCam = static_cast<CameraComponent*>(leaf->addComponent(std::make_unique<CameraComponent>()));
    deepCam->enabled = true;
    CHECK(mainCameraFrom(&deepRoot) == deepCam);
    CHECK(mainCameraFrom(leaf) == deepCam);

    return 0;
}

static int testActivateMainCamera() {
    // root -> {a (camera, enabled), b (camera, enabled), c (camera, disabled)}
    Actor3D root("root");
    Actor* a = root.addChild(std::make_unique<Actor3D>("a"));
    Actor* b = root.addChild(std::make_unique<Actor3D>("b"));
    Actor* c = root.addChild(std::make_unique<Actor3D>("c"));
    auto* camA = static_cast<CameraComponent*>(a->addComponent(std::make_unique<CameraComponent>()));
    auto* camB = static_cast<CameraComponent*>(b->addComponent(std::make_unique<CameraComponent>()));
    auto* camC = static_cast<CameraComponent*>(c->addComponent(std::make_unique<CameraComponent>()));
    camA->enabled = true;
    camB->enabled = true;
    camC->enabled = false;

    activateMainCamera(&root, *camC);
    CHECK(camC->enabled == true);
    CHECK(camA->enabled == false);
    CHECK(camB->enabled == false);

    // Reachable from any actor in the tree, not just the literal root.
    activateMainCamera(a, *camA);
    CHECK(camA->enabled == true);
    CHECK(camB->enabled == false);
    CHECK(camC->enabled == false);

    // A null `any` (sceneRootOf(nullptr) == nullptr) must return without
    // crashing AND without mutating the target camera -- the function
    // returns before reaching `cam.enabled = true` in that case.
    bool campBefore = camB->enabled;
    activateMainCamera(nullptr, *camB);
    CHECK(camB->enabled == campBefore); // unchanged, not forced true

    return 0;
}

static int testNegate() {
    CHECK(negate(Value::Int(5)).t == Value::T::Int);
    CHECK(negate(Value::Int(5)).i == -5);
    CHECK(negate(Value::Int(-5)).i == 5);
    CHECK(negate(Value::Int(0)).i == 0);
    // Any non-Int numeric type goes through Float, per the original ternary.
    CHECK(negate(Value::Float(2.5)).t == Value::T::Float);
    CHECK(negate(Value::Float(2.5)).f == -2.5);
    CHECK(negate(Value::Bool(true)).t == Value::T::Float);
    CHECK(negate(Value::Bool(true)).f == -1.0);
    CHECK(negate(Value::Char('A')).f == -65.0); // 'A' == 65
    CHECK(negate(Value::Str("x")).f == -0.0 || negate(Value::Str("x")).f == 0.0); // String.num()==0

    return 0;
}

static int testValueComparisons() {
    // Numeric vs numeric: compares by num(), across mixed numeric types.
    CHECK(valueEquals(Value::Int(5), Value::Int(5)).b == true);
    CHECK(valueEquals(Value::Int(5), Value::Float(5.0)).b == true); // cross-type numeric equality
    CHECK(valueEquals(Value::Int(5), Value::Bool(true)).b == false); // 5 != 1
    CHECK(valueEquals(Value::Bool(true), Value::Int(1)).b == true);
    CHECK(valueNotEquals(Value::Int(5), Value::Int(6)).b == true);
    CHECK(valueNotEquals(Value::Int(5), Value::Int(5)).b == false);

    // Non-numeric: compares by (str(), t) equality -- same text but
    // DIFFERENT types must NOT compare equal (e.g. int 5's str() is "5",
    // matching a String "5", but they must not be Value-equal).
    CHECK(valueEquals(Value::Str("5"), Value::Int(5)).b == false);
    CHECK(valueEquals(Value::Str("hi"), Value::Str("hi")).b == true);
    CHECK(valueEquals(Value::Str("hi"), Value::Str("bye")).b == false);
    CHECK(valueEquals(Value::Null_(), Value::Null_()).b == true);
    CHECK(valueNotEquals(Value::Str("5"), Value::Int(5)).b == true);

    // Ordering: numeric via num().
    CHECK(valueLess(Value::Int(1), Value::Int(2)).b == true);
    CHECK(valueLess(Value::Int(2), Value::Int(1)).b == false);
    CHECK(valueGreater(Value::Float(2.5), Value::Int(2)).b == true);
    CHECK(valueLessEq(Value::Int(2), Value::Int(2)).b == true);
    CHECK(valueLessEq(Value::Int(3), Value::Int(2)).b == false);
    CHECK(valueGreaterEq(Value::Int(2), Value::Int(2)).b == true);
    CHECK(valueGreaterEq(Value::Int(1), Value::Int(2)).b == false);

    return 0;
}

static int testArrayHelpers() {
    Value empty = Value::Arr({});
    CHECK(arrayLength(empty).t == Value::T::Int);
    CHECK(arrayLength(empty).i == 0);

    Value three = Value::Arr({Value::Int(1), Value::Int(2), Value::Int(3)});
    CHECK(arrayLength(three).i == 3);

    // Non-Array / null-backing: 0, not a crash.
    CHECK(arrayLength(Value::Int(5)).i == 0);
    Value nullBacked;
    nullBacked.t = Value::T::Array; // arr left null deliberately
    CHECK(arrayLength(nullBacked).i == 0);

    // arrayAdd: pushes and reports success for a real array.
    Value arr = Value::Arr({Value::Int(1)});
    CHECK(arrayAdd(arr, Value::Int(2)) == true);
    CHECK(arr.arr->size() == 2);
    CHECK((*arr.arr)[1].i == 2);

    // arrayAdd on a non-Array: false, no crash, and definitely no mutation.
    Value notArr = Value::Int(5);
    CHECK(arrayAdd(notArr, Value::Int(1)) == false);
    CHECK(notArr.i == 5);

    // arrayAdd on a null-backing Array Value: false (matches the
    // interpreter's own `!args.empty() && obj.arr` guard).
    CHECK(arrayAdd(nullBacked, Value::Int(1)) == false);

    return 0;
}

static int testMathCall() {
    std::vector<Value> args;

    args = {Value::Int(5), Value::Int(0), Value::Int(10)};
    CHECK(mathCall("clamp", args, 1).i == 5);
    args = {Value::Int(-5), Value::Int(0), Value::Int(10)};
    CHECK(mathCall("clamp", args, 1).i == 0);
    args = {Value::Int(50), Value::Int(0), Value::Int(10)};
    CHECK(mathCall("clamp", args, 1).i == 10);
    args = {Value::Float(5.5), Value::Float(0.0), Value::Float(10.0)};
    CHECK(mathCall("clamp", args, 1).t == Value::T::Float);

    args = {Value::Float(0.0), Value::Float(10.0), Value::Float(0.5)};
    CHECK(mathCall("lerp", args, 1).f == 5.0);

    args = {Value::Float(0.0)};
    CHECK(mathCall("sin", args, 1).f == 0.0);
    args = {Value::Float(0.0)};
    CHECK(mathCall("sine", args, 1).f == 0.0); // alias
    args = {Value::Float(0.0)};
    CHECK(mathCall("cos", args, 1).f == 1.0);
    args = {Value::Float(0.0)};
    CHECK(mathCall("cosine", args, 1).f == 1.0); // alias

    args = {Value::Float(4.0)};
    CHECK(mathCall("sqrt", args, 1).f == 2.0);
    args = {Value::Float(1.0), Value::Float(0.0)};
    CHECK(mathCall("exp", args, 1).f == std::exp(1.0));
    args = {Value::Float(2.0), Value::Float(3.0)};
    CHECK(mathCall("pow", args, 1).f == 8.0);

    args = {Value::Int(-5)};
    CHECK(mathCall("abs", args, 1).t == Value::T::Int);
    CHECK(mathCall("abs", args, 1).i == 5);
    args = {Value::Float(-5.5)};
    CHECK(mathCall("abs", args, 1).t == Value::T::Float);
    CHECK(mathCall("abs", args, 1).f == 5.5);

    args = {Value::Float(1.5)};
    CHECK(mathCall("floor", args, 1).f == 1.0);
    args = {Value::Float(1.5)};
    CHECK(mathCall("ceil", args, 1).f == 2.0);
    args = {Value::Float(1.5)};
    CHECK(mathCall("round", args, 1).f == 2.0);

    args = {Value::Int(3), Value::Int(7)};
    CHECK(mathCall("min", args, 1).i == 3);
    args = {Value::Int(3), Value::Int(7)};
    CHECK(mathCall("max", args, 1).i == 7);
    args = {Value::Float(3.0), Value::Float(7.0)};
    CHECK(mathCall("min", args, 1).t == Value::T::Float); // not all-Int -> Float

    args = {Value::Float(180.0)};
    double rad = mathCall("deg2rad", args, 1).f;
    CHECK(rad > 3.14 && rad < 3.15);
    args = {Value::Float(rad)};
    double deg = mathCall("rad2deg", args, 1).f;
    CHECK(deg > 179.9 && deg < 180.1);

    args = {};
    Value rf = mathCall("rand_f", args, 1);
    CHECK(rf.f >= 0.0 && rf.f < 1.0);
    Value ri = mathCall("rand_i", args, 1);
    CHECK(ri.i >= 0 && ri.i <= 0x7fffffff);
    args = {Value::Float(5.0), Value::Float(10.0)};
    Value rfr = mathCall("rand_f_range", args, 1);
    CHECK(rfr.f >= 5.0 && rfr.f <= 10.0);
    args = {Value::Int(5), Value::Int(10)};
    Value rir = mathCall("rand_i_range", args, 1);
    CHECK(rir.i >= 5 && rir.i <= 10);
    // Reversed range (hi < lo) is swapped, not an empty/invalid range.
    args = {Value::Int(10), Value::Int(5)};
    Value rirRev = mathCall("rand_i_range", args, 1);
    CHECK(rirRev.i >= 5 && rirRev.i <= 10);

    // Unknown function name throws.
    args = {};
    CHECK_THROWS_RUNTIME_ERROR(mathCall("not_a_real_function", args, 1));

    return 0;
}

int main() {
    if (testCoerce()) return 1;
    if (testIsVecAndVfield()) return 1;
    if (testMakeVector()) return 1;
    if (testArithScalar()) return 1;
    if (testArithStringConcat()) return 1;
    if (testArithVector()) return 1;
    if (testGetSetNativeFieldFog()) return 1;
    if (testGetSetNativeFieldCameraScalars()) return 1;
    if (testGetSetNativeFieldCameraActiveExclusivity()) return 1;
    if (testNativeFieldUnknownBuiltin()) return 1;
    if (testMainCameraFrom()) return 1;
    if (testActivateMainCamera()) return 1;
    if (testNegate()) return 1;
    if (testValueComparisons()) return 1;
    if (testArrayHelpers()) return 1;
    if (testMathCall()) return 1;

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
