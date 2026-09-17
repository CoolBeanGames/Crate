#pragma once
#include "script/Token.h"
#include "script/Value.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace crate {
class Actor;
class CameraComponent;
} // namespace crate

namespace crate::script {

struct ScriptContext;

// Thrown for any script-runtime failure (bad operator, out-of-range index,
// division by zero, unknown identifier, ...). Shared by the tree-walking
// interpreter and, eventually, natively-compiled script code (see
// transpiration.txt), so both report errors the same way.
struct RuntimeError : std::runtime_error {
    int line;
    RuntimeError(std::string msg, int ln) : std::runtime_error(std::move(msg)), line(ln) {}
};

// ---- Value coercion / vector helpers ----
// These are the stateless pieces of script semantics -- no dependency on a
// live Interpreter, an actor, or any engine singleton -- factored out so
// both the interpreter and (eventually) generated native code call the exact
// same logic instead of two independently-maintained copies.

// Coerce a value to a declared type name (best effort; keeps the value on a
// mismatch rather than erroring, in the spirit of a loose scripting language).
Value coerce(Value v, const std::string& ty);

// Build a Vector2/Vector3 value.
Value makeVector(const std::string& kind, double x, double y, double z);

// True for a Vector2 / Vector3 aggregate value.
bool isVec(const Value& v);
double vfield(const Value& v, const char* f);
// Write a single component of a vector Value in place (no-op if `v` isn't a
// Vector2/Vector3 Object -- mirrors vfield()'s "missing -> silently do
// nothing" leniency rather than throwing). Since makeVector()'s ScriptObject
// is shared via shared_ptr (see Value::obj), this mutates every alias of
// `v`, matching the same reference semantics plain field writes on any
// ScriptObject already have -- deliberately preserved, see
// transpiration.txt's Vector aliasing decision.
void vfieldSet(const Value& v, const char* f, double x);

// Arithmetic core shared by binary expressions and compound assignment.
// Handles numbers, string concat (+), and Vector2/Vector3 math. Throws
// RuntimeError on an invalid operation (bad operator, division/modulo by
// zero, mismatched vector dimensions on division by a zero component).
Value arith(Tok op, const Value& a, const Value& b, int line);

// Unary minus: Int stays Int, everything else numeric goes through Float.
// A small free function (rather than inlined duplicate-operand text) so
// callers -- generated native code especially -- can pass a possibly
// side-effecting operand expression exactly once.
Value negate(const Value& a);

// Value equality/ordering, exactly mirroring Interpreter::evalBinary's
// comparison cases: numeric types compare by num(); everything else compares
// by (str(), t) equality (ordering falls back to raw num(), 0.0 for
// non-numeric types, matching Value::num()'s own default). Each takes both
// operands by const reference so a caller never needs to duplicate a
// possibly side-effecting operand's source text to use it twice.
Value valueEquals(const Value& a, const Value& b);
Value valueNotEquals(const Value& a, const Value& b);
Value valueLess(const Value& a, const Value& b);
Value valueGreater(const Value& a, const Value& b);
Value valueLessEq(const Value& a, const Value& b);
Value valueGreaterEq(const Value& a, const Value& b);

// array.length (as either a bare member read or a zero-arg method call) --
// 0 for a non-Array or a null backing vector, matching the interpreter's own
// null-guarded reads.
Value arrayLength(const Value& v);
// array.add(item): pushes and returns true if `v` is a real Array; returns
// false (does nothing) otherwise, matching the interpreter's silent
// fallthrough for a non-Array/null-backing receiver.
bool arrayAdd(const Value& v, const Value& item);

// Math.<fn>(args) -- the free-function form of Interpreter::mathCall, moved
// here (it never touched Interpreter's self_/ctx_/scopes_) so both the
// interpreter and generated native code share one implementation. Throws
// RuntimeError for an unknown function name.
Value mathCall(const std::string& fn, std::vector<Value>& args, int line);

// ---- Native (non-script) component field reflection ----
// Fog/Camera (and, once natively-compiled script classes exist, those too)
// are exposed through get_component() as a "live view": ScriptObject::builtin
// names which kind and ScriptObject::nativePtr points at the real instance,
// so reads/writes go straight to it instead of a disposable fields-map
// snapshot. getNativeField/setNativeField consult a small per-type table
// (see Runtime.cpp) instead of a hardcoded if-chain, so a new native-
// reachable type means adding a table entry, not editing this dispatch code.
bool getNativeField(const ScriptObject& o, const std::string& name, Value& out);
bool setNativeField(ScriptObject& o, const std::string& name, const Value& v);

// Reads a member off an arbitrary Actor* (position/rotation/scale/forward/
// right/up/name) -- the free-function form of what was
// Interpreter::actorMember, moved here (Phase 9b) so generated code can
// read a member off a GENERAL Actor-typed value (e.g. a field holding an
// Actor reference assigned via the Inspector's drag-drop picker), not just
// the statically-known bare `transform`/`actor` identifiers CodeGen already
// special-cases. Throws RuntimeError for a null actor or an unknown member.
Value actorMember(crate::Actor* a, const std::string& name, int line);

// Walks up an actor's parent chain to the top -- the scene's implicit root
// Actor (see Scene::root()). Shared by Camera.main's resolution and, since
// get_root() is a bare global (like Camera.main) reached by walking up from
// the CALLING script's own actor rather than from an explicit receiver, by
// get_root() itself (see Interpreter::builtinCall / CodeGen's global-call
// emission). Returns nullptr for a null actor.
crate::Actor* sceneRootOf(crate::Actor* a);

// Input.Mouse.<member> -- a bare global chain read via member access
// (parsed/evaluated structurally, like Camera.main and Input.<method>(...);
// "Input"/"Mouse" are never themselves evaluated as values). Shares
// ScriptContext::inputQuery's existing what-codes: 0/1/2 = pressed/
// just_pressed/just_released for a reserved button name ("mouse_left"/
// "mouse_right"/"mouse_middle"), 5/6 = mouse delta x/y, 7/8 = scroll delta
// x/y (see ScriptSystem.cpp's inputQuery lambda and Input::pollMouse()).
// Throws RuntimeError for an unrecognized member.
Value inputMouseMember(ScriptContext* ctx, const std::string& name, int line);

// Value.instantiate() -- called on a "Scene"-typed field (a plain string
// holding a .cscene asset path; see ScriptComponent's Inspector asset-
// picker for such a field). Loads that scene and attaches it as a new
// child under `callerOwner`'s scene root (i.e. sceneRootOf(callerOwner),
// matching get_root()'s own anchor point), returning an Actor reference to
// the new instance, or Null on failure (a bad/empty path, most commonly).
Value valueInstantiate(ScriptContext* ctx, const Value& pathValue, crate::Actor* callerOwner, int line);

// ---- Camera.main resolution (task 77) ----
// Camera.main isn't reached from a specific actor -- it's a bare global, so
// the scene it searches is found by walking up from the calling script's own
// actor to the root.
CameraComponent* mainCameraFrom(crate::Actor* any);
void activateMainCamera(crate::Actor* any, CameraComponent& cam);

} // namespace crate::script
