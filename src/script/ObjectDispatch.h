#pragma once
#include "script/Interpreter.h"
#include "script/Value.h"

#include <memory>
#include <string>
#include <vector>

namespace crate::script {

// Generic, by-name field/method access on an ARBITRARY ScriptObject,
// dispatching correctly whether it's an interpreted script instance
// (cls != nullptr), a compiled script instance live view (nativePtr +
// compiledInfo), or a BuiltinComponent live view (nativePtr only, e.g.
// Fog/Camera). This is the shared mechanism both Interpreter.cpp (for a
// general/cross-object reference it doesn't already special-case) and
// generated native code (for get_component results, Callables, signals,
// this.base -- anything not statically known to be `this`) use, so
// get_component/signals/references work uniformly regardless of whether
// the CALLER or the TARGET happens to be interpreted or compiled. See
// transpiration.txt, "Transplation" Phase 9a.
//
// Part of crate_script_runtime (like Interpreter.cpp, which this needs for
// the interpreted-target case) -- deliberately free of any engine
// singleton access, so it's safe to link into a script DLL.

// Throws RuntimeError if `obj` is null or `name` isn't found on it (matches
// Interpreter::evalMember's own fail-fast contract for a general member
// read).
Value getObjectMember(const std::shared_ptr<ScriptObject>& obj, const std::string& name, int line);

// Returns false (and does not throw) if `name` isn't a recognized member on
// `obj` -- callers produce their own appropriately-worded RuntimeError,
// matching each call site's existing message conventions (see
// Interpreter::assign() and CodeGen's assignTo()).
bool trySetObjectMember(const std::shared_ptr<ScriptObject>& obj, const std::string& name,
                        const Value& v);

// Calls `method` on `obj` (interpreted or compiled), non-resumable (a
// do_async inside the called method runs synchronously to completion --
// resumable=true is reserved for the top-level hook-invocation entry point
// only, see Interpreter::call()). Throws RuntimeError if `obj` is null, has
// no `method`, or isn't a callable kind of ScriptObject at all. ALSO tries,
// before dispatching to a declared method, the Godot-3-style signal API
// (emit_signal/connect/disconnect/is_connected) and the get_component
// escape hatch -- exactly like Interpreter::evalCall's own inline shortcut
// block used to (now consolidated here, Phase 9d, so it isn't duplicated
// between the interpreter and generated code) -- but ONLY when `obj`
// itself doesn't already declare a method of that name.
Value callObjectMethod(ScriptContext* ctx, const std::shared_ptr<ScriptObject>& obj,
                       const std::string& method, std::vector<Value> args, int line);

// ---- Signals (Godot-style): connect / disconnect / emit / is_connected /
// get_connections, and first-class Callable invocation (Phase 9d) ----
// Moved out of Interpreter (which were private, self_/ctx_-free member
// functions already, exactly like actorMember was -- see Runtime.cpp) so
// generated native code shares the IDENTICAL implementation, not a second
// copy. Interpreter's own signalCall/emitSignal/invokeCallable are now thin
// wrappers wired to these.

// `sig` must be a Value::T::Signal (a SignalRef, e.g. from `this.mySignal`
// or `someObj.get_component(...).mySignal`). Throws RuntimeError if
// sig.obj is null or `method` isn't one of connect/disconnect/is_connected/
// emit/get_connections.
Value signalCall(ScriptContext* ctx, const Value& sig, const std::string& method,
                 std::vector<Value> args, int line);

// Invokes a Value::T::Callable (a bound method reference, e.g. from a bare
// own-method-name-as-value or a signal connection). Returns Value::Null_()
// (a no-op, matching Godot) if the bound target has been freed; throws
// RuntimeError if `fn` isn't actually a Callable.
Value invokeCallable(ScriptContext* ctx, const Value& fn, std::vector<Value> args, int line);

// Fires every Callable connected to `owner`'s `name` signal, in connection
// order. No-op if `owner` is null or has no connections for `name`.
void emitSignal(ScriptContext* ctx, const std::shared_ptr<ScriptObject>& owner,
               const std::string& name, std::vector<Value> args, int line);

// ---- Value-level generic dispatch (Phase 9b) ----
// A "general receiver" in generated native code (anything not statically
// known at codegen time to be `this`/bare `transform`/`actor`/Math/Input --
// e.g. a get_component() result, or a field holding an Actor reference
// assigned via the Inspector's drag-drop picker) evaluates to an arbitrary
// crate::script::Value, not necessarily an Object. These three mirror
// Interpreter::evalMember/evalCall/assign's own fallback dispatch (the part
// reached AFTER their syntactic special cases, which depend on identifier
// text, not the evaluated value, and so stay separately hand-written at
// each call site) over Object/Actor/Array/Signal/Callable/TypeRef
// (Camera.main, and -- Phase 9e -- any `static class`'s singleton via
// ctx->getStatic) receivers, so CodeGen has exactly one dispatcher to call
// regardless of what the receiver turns out to be at runtime.

// Member read. Throws RuntimeError for a null/unsupported receiver or an
// unknown member (matches Interpreter::evalMember's fail-fast contract).
// `ctx` is needed for a TypeRef receiver that names a `static class`
// (ctx->getStatic) -- pass nullptr only when the caller can prove `v` will
// never be such a TypeRef (no CodeGen call site does; every one has a
// ctx_ in scope). `callerOwner` is the CALLING script's own actor
// (generated code's `owner_`) -- needed only for Camera.main (Phase 9c):
// unlike every other receiver kind, a Camera TypeRef Value carries no
// actor of its own to search from, so mainCameraFrom() walks up from the
// caller instead, exactly like Interpreter::evalMember's own TypeRef
// "Camera"+"main" branch (which uses self_->owner) does.
Value getValueMember(ScriptContext* ctx, const Value& v, const std::string& name, int line,
                     crate::Actor* callerOwner);

// Member write. Returns false (does not throw) if `v`'s receiver kind or
// `name` isn't recognized -- caller produces its own appropriately-worded
// RuntimeError, matching trySetObjectMember's contract. `ctx` is needed for
// the same static-class TypeRef case as getValueMember.
bool trySetValueMember(ScriptContext* ctx, const Value& v, const std::string& name, const Value& val);

// Method call: Object (declared method / get_component / the Godot-3
// signal shortcuts, all via callObjectMethod), Actor (get_component only),
// Array (.add/.length), Signal (connect/disconnect/is_connected/emit/
// get_connections, via signalCall), Callable (.call()/.emit(), via
// invokeCallable), plus the universal .str(). Throws RuntimeError for a
// null/unsupported receiver or an unknown method.
Value callValueMethod(ScriptContext* ctx, const Value& v, const std::string& method,
                      std::vector<Value> args, int line);

} // namespace crate::script
