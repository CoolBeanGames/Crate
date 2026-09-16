#pragma once
#include "script/CompiledClassInfo.h"

#include <string>
#include <vector>

namespace crate {
class Component;
class Actor;
} // namespace crate

namespace crate::script {

struct ScriptContext;

// Function-pointer types matching the extern "C" ABI CodeGen.cpp emits per
// class (see transpiration.txt, "Transplation" Phase 4, static variant
// Phase 9e):
//   CreateInstance_<Class>(ScriptContext*, Actor*) -> Component*   (Component-shaped)
//   DestroyInstance_<Class>(Component*) -> void
//   CreateStatic_<Class>(ScriptContext*) -> void*                 (static class)
//   DestroyStatic_<Class>(void*) -> void
//   GetClassInfo_<Class>() -> const CompiledClassInfo*             (either kind)
using CreateInstanceFn = crate::Component* (*)(ScriptContext*, crate::Actor*);
using DestroyInstanceFn = void (*)(crate::Component*);
using CreateStaticFn = void* (*)(ScriptContext*);
using DestroyStaticFn = void (*)(void*);
using GetClassInfoFn = const CompiledClassInfo* (*)();

struct NativeClassExport {
    std::string className;
    bool isStatic = false; // which pair below is populated
    // Component-shaped exports (isStatic == false):
    CreateInstanceFn create = nullptr;
    DestroyInstanceFn destroy = nullptr;
    // Static-class exports (isStatic == true):
    CreateStaticFn createStatic = nullptr;
    DestroyStaticFn destroyStatic = nullptr;
    // Populated for either kind.
    GetClassInfoFn classInfo = nullptr;
};

// Wraps one loaded native script DLL (the output of
// crate::editor::scriptbuild::buildNamespace). Resolves the three exports
// above for exactly the class names the caller supplies -- the set
// ScriptBuild recorded as having been compiled into this DLL -- and
// FreeLibrary()s the module when the NativeModule is destroyed.
//
// Move-only (owns a live OS module handle); default-constructed instances
// are "empty" (ok() == false) so callers can hold one before a load
// attempt without a sentinel/optional wrapper.
class NativeModule {
public:
    // Loads `dllPath` and resolves every export in `classNames`. On ANY
    // failure (LoadLibrary fails, or an expected export is missing), the
    // returned NativeModule has ok() == false, exports() is empty, and the
    // library (if it was even loaded) has already been released -- never a
    // partially-resolved, partially-loaded module.
    static NativeModule load(const std::string& dllPath, const std::vector<std::string>& classNames);

    NativeModule() = default;
    NativeModule(const NativeModule&) = delete;
    NativeModule& operator=(const NativeModule&) = delete;
    NativeModule(NativeModule&& other) noexcept;
    NativeModule& operator=(NativeModule&& other) noexcept;
    ~NativeModule();

    bool ok() const { return handle_ != nullptr; }
    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }
    const std::vector<NativeClassExport>& exports() const { return exports_; }
    const NativeClassExport* find(const std::string& className) const;

private:
    void* handle_ = nullptr; // HMODULE, kept opaque so <windows.h> stays out of this header
    std::string path_;
    std::vector<NativeClassExport> exports_;
    std::string error_;
};

} // namespace crate::script
