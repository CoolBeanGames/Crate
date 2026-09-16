#pragma once

namespace crate::script {

// Per-class metadata a natively-compiled script class exports alongside its
// CreateInstance_<Class>/DestroyInstance_<Class> factory functions (see
// transpiration.txt, "Transplation" Phase 4/5).
//
// PHASE 4 SCOPE: only className/baseClassName are populated -- enough for
// NativeClassRegistry to key entries by name and, later, for isA()-style
// checks. Phase 5 extends this struct with field/method accessor tables
// (get_component-by-name, Inspector-during-Play sync, the undeclared-field
// overflow-map escape hatch) and CodeGen.cpp is revisited to populate them;
// this struct's layout is expected to grow, which is safe because every
// script DLL is always rebuilt from the CURRENTLY RUNNING engine's
// CodeGen.cpp (see ScriptBuild::buildNamespace), so there is never a
// version-skewed DLL floating around with an older struct layout.
struct CompiledClassInfo {
    const char* className;
    const char* baseClassName; // "" for a direct Actor/Actor2D/Actor3D base
};

} // namespace crate::script
