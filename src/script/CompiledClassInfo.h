#pragma once
#include "script/Value.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace crate::script {

struct ScriptContext;

// Per-class reflection table a natively-compiled script class exports
// alongside its CreateInstance_<Class>/DestroyInstance_<Class> factory
// functions (see transpiration.txt, "Transplation" Phase 4/5). This is what
// lets generic, non-generated code (the Inspector's field sync during Play,
// and eventually get_component/signals' by-name dispatch) work uniformly
// across every compiled script class without per-class code, the same way
// ScriptObject::fields (an unordered_map) already does for the interpreter.
//
// `instance` in every function pointer below is the concrete generated
// <Name>_Native* for this class, passed as void* to keep this header
// (and the extern "C" ABI surface) free of any per-class generated type.

struct FieldAccessor {
    const char* name;
    const char* declaredType; // "" for var, else "int"/"Vector3"/a class name --
                              // the same strings ClassInfo::decl->fields carries
    Value (*get)(void* instance);
    void (*set)(void* instance, const Value& v);
};

struct MethodAccessor {
    const char* name;
    // ScriptContext* is accepted (though unused by Phase 5's own generated
    // wrappers, which already close over the instance's own held context)
    // so a future generic by-name cross-object dispatch consumer
    // (get_component/signals) doesn't need this signature to change again.
    Value (*invoke)(void* instance, ScriptContext* ctx, std::vector<Value> args);
};

struct CompiledClassInfo {
    const char* className;
    const char* baseClassName; // "" for a direct Actor/Actor2D/Actor3D base

    const FieldAccessor* fields;
    size_t fieldCount;
    const MethodAccessor* methods;
    size_t methodCount;

    // Undeclared-field auto-vivification escape hatch (mirrors
    // Interpreter::lvalue()'s implicit field creation): returns the
    // instance's dynamic overflow map by reference, so a name not present
    // in `fields` above can still be read/written generically.
    std::unordered_map<std::string, Value>& (*overflow)(void* instance);

    // Declared `signal foo();` names (Phase 9d), so a member read for one
    // of them on an EXTERNALLY-obtained reference (e.g. a get_component()
    // result) resolves to a Value::T::Signal, exactly like a bare field
    // name resolves to its value -- mirrors ClassInfo::hasSignal() for the
    // interpreted side.
    const char* const* signalNames;
    size_t signalCount;

    // The canonical ScriptObject wrapper for THIS instance (Phase 9d) --
    // every generated class constructs exactly ONE of these in its
    // constructor (see CodeGen.cpp's `selfView_` member) and this accessor
    // always returns that SAME shared_ptr, never a fresh one. This is what
    // makes signals/first-class references work AT ALL for a compiled
    // instance: a signal's connections live on a specific ScriptObject
    // (ScriptObject::connections), so get_component() and `this` must both
    // resolve to the identical object, or a connection made through one
    // reference would be invisible to an emit through another.
    std::shared_ptr<ScriptObject> (*selfView)(void* instance);
};

} // namespace crate::script
