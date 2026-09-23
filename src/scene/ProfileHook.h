#pragma once
#include <string>

namespace crate {

// Optional hook Actor::updateComponents()/physicsUpdateComponents() call with
// each component's timing, if installed. Deliberately a plain injectable
// function pointer rather than a direct call into core::Profiler: Actor.cpp
// lives in the crate_script_runtime target, which is built singleton-free on
// purpose (see CMakeLists.txt's note on that target -- the same reasoning
// that keeps it away from Console::get()/ComponentRegistry::get()/etc.
// applies equally to a new Profiler::get() singleton, since this code may
// one day be statically linked into a second, separate native-script DLL).
// The editor (crate_core, which IS allowed to reach engine singletons)
// installs the real hook at startup; null means profiling is off (e.g. in a
// context, like a unit test, that never installs one).
using ComponentProfileHook = void (*)(const std::string& actorName, const std::string& componentType,
                                      double milliseconds);
extern ComponentProfileHook g_componentProfileHook;

} // namespace crate
