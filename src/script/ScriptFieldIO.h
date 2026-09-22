#pragma once
// Value <-> scene-file text codec for a script component's fields (see
// scene/SceneIO.cpp for the overall file format, scene/FieldCodec.h for the
// plain-C++-type version this mirrors). Lives in crate_core alongside
// ScriptComponent/NativeScriptComponent, NOT crate_script_runtime -- unlike
// Runtime.h's stateless helpers, this needs ScriptContext::getComponent (to
// resolve a saved "cref" back to a live component) and has no reason to be
// linked into a generated per-namespace DLL.
#include "script/Value.h"

#include <functional>
#include <iosfwd>
#include <string>

namespace crate {
class Actor;
}

namespace crate::script {
struct ScriptContext;

// Writes one FIELD line for `key`/`v`. Actor-typed and native-component-view
// values are written by save-time local id (via `idOf`) so they survive a
// round trip even though the referenced actor may appear later in the file.
void writeValueField(std::ostream& out, const std::string& key, const Value& v,
                     const std::function<int(const crate::Actor*)>& idOf);

// Rebuilds the Value a writeValueField() line described. `kind`/`raw` are
// the FIELD line's type tag and rest-of-line payload. `actorById` resolves a
// saved local id back to a live Actor* (nullptr for -1 / unknown); `ctx` is
// needed only for a "cref" line (get_component-style resolution).
Value readValueField(const std::string& kind, const std::string& raw,
                     const std::function<crate::Actor*(int)>& actorById, ScriptContext* ctx);

} // namespace crate::script
