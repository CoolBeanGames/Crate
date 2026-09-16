#pragma once
#include "script/Ast.h"
#include "script/ClassInfo.h"

#include <string>
#include <unordered_set>

namespace crate::script {

// Translates a single cScript ClassDecl into standalone C++ source text (a
// header and a .cpp) that calls into Runtime.h helpers instead of being
// re-interpreted -- see transpiration.txt, "Transplation" Phase 2.
//
// SCOPE (Phase 2 -- deliberately limited, see transpiration.txt for the
// full rationale): supports a real, useful subset of cScript that needs
// nothing beyond a class's own declared fields/methods and the actor it's
// attached to -- literals, arithmetic, vectors, local variables, control
// flow (if/switch/do-while; do_async compiles as an ordinary synchronous
// loop for now, matching the interpreter's own fallback for a *nested*
// do_async -- frame-stepped top-level resumption is Phase 6), the class's
// own fields/methods (including undeclared-field auto-vivification, via a
// dynamic overflow map, exactly mirroring Interpreter::lvalue()),
// transform/actor access, Math.*, Input.*, and print()/type_of()/str().
//
// Classes based on Actor/Actor2D/Actor3D, OR on another script class --
// same OR different namespace/module (Phase 9f: real C++ inheritance,
// `class Derived_Native : public Base_Native`, with __declspec(dllexport/
// dllimport) so it works whether the base ends up in the same DLL or a
// different one) -- are accepted. CodeGen itself has no notion of
// namespaces at all; the generated inheritance syntax here is namespace-
// agnostic either way. Only the BUILD orchestration (include paths /
// import libs / topological build order across namespaces, so a base is
// always built before a namespace that depends on it) lives in
// ScriptBuild.cpp, the one place that actually knows about namespaces.
// Abstract classes are refused (they don't map onto a single concrete
// native Component/static instance).
struct CodeGenResult {
    bool ok = false;
    std::string className; // sanitized identifier base used for <ClassName>_Native etc.
    std::string header;    // <ClassName>.gen.h contents
    std::string source;    // <ClassName>.gen.cpp contents
    std::string error;     // set iff !ok (first problem found, with a line number if known)
};

// The same identifier-sanitization generateClass() itself uses internally
// for <ClassName>_Native/export-suffix names (alnum/underscore only,
// prefixed with '_' if it would otherwise start with a digit or be
// empty). Exposed so ScriptBuild.cpp can compute the IDENTICAL
// -DCRATE_GEN_BUILDING_<suffix> macro name for a class without
// duplicating (and risking drifting from) this logic.
std::string sanitizeClassName(const std::string& name);

// Takes the full ClassInfo (not just its owned ClassDecl), not just for its
// `decl` -- Phase 9f needs `baseClass` (the resolved script base's own
// ClassInfo*, or null) to walk the inheritance chain for field/method/
// signal resolution and to know the base's generated C++ type name.
//
// `knownClassNames` (Phase 9b): every script class name ScriptSystem knows
// about at build time (across ALL namespaces, not just this one), so
// `type_of(SomeOtherScriptClass)` resolves to a real TypeRef instead of
// falling through to the dynamic-overflow-map guess Identifier's fallback
// makes for an unrecognized bare name. Defaults to empty for callers (tests)
// that only care about this class's own name / the fixed builtin list.
CodeGenResult generateClass(const ClassInfo& classInfo,
                            const std::unordered_set<std::string>& knownClassNames = {});

} // namespace crate::script
