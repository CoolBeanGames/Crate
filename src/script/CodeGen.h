#pragma once
#include "script/Ast.h"

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
// Only classes based directly on Actor/Actor2D/Actor3D are accepted --
// script-to-script inheritance needs the cross-class registry (Phase 4/5)
// and is refused for now. Constructs needing that registry --
// get_component(...), this.base.<method>(), signals (declare/connect/
// emit), and bare method/signal references used as first-class values --
// are refused with a clear CodeGenResult::error rather than silently
// emitting incorrect code. Static and abstract classes are refused too
// (they don't map onto a single concrete native Component instance).
struct CodeGenResult {
    bool ok = false;
    std::string className; // sanitized identifier base used for <ClassName>_Native etc.
    std::string header;    // <ClassName>.gen.h contents
    std::string source;    // <ClassName>.gen.cpp contents
    std::string error;     // set iff !ok (first problem found, with a line number if known)
};

// `knownClassNames` (Phase 9b): every script class name ScriptSystem knows
// about at build time (across ALL namespaces, not just this one), so
// `type_of(SomeOtherScriptClass)` resolves to a real TypeRef instead of
// falling through to the dynamic-overflow-map guess Identifier's fallback
// makes for an unrecognized bare name. Defaults to empty for callers (tests)
// that only care about this class's own name / the fixed builtin list.
CodeGenResult generateClass(const ClassDecl& decl,
                            const std::unordered_set<std::string>& knownClassNames = {});

} // namespace crate::script
