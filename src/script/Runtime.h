#pragma once
#include "script/Token.h"
#include "script/Value.h"

#include <stdexcept>
#include <string>

namespace crate {
class Actor;
class CameraComponent;
} // namespace crate

namespace crate::script {

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

// Arithmetic core shared by binary expressions and compound assignment.
// Handles numbers, string concat (+), and Vector2/Vector3 math. Throws
// RuntimeError on an invalid operation (bad operator, division/modulo by
// zero, mismatched vector dimensions on division by a zero component).
Value arith(Tok op, const Value& a, const Value& b, int line);

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

// ---- Camera.main resolution (task 77) ----
// Camera.main isn't reached from a specific actor -- it's a bare global, so
// the scene it searches is found by walking up from the calling script's own
// actor to the root.
CameraComponent* mainCameraFrom(crate::Actor* any);
void activateMainCamera(crate::Actor* any, CameraComponent& cam);

} // namespace crate::script
