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
// no `method`, or isn't a callable kind of ScriptObject at all.
Value callObjectMethod(ScriptContext* ctx, const std::shared_ptr<ScriptObject>& obj,
                       const std::string& method, std::vector<Value> args, int line);

} // namespace crate::script
