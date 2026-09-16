#include "script/NativeModule.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace crate::script {

NativeModule NativeModule::load(const std::string& dllPath, const std::vector<std::string>& classNames) {
    NativeModule mod;
    mod.path_ = dllPath;

    // Shadow-copied filenames (see ScriptBuild::buildNamespace) mean this is
    // always a distinct file per build generation, so an ordinary
    // LoadLibraryA is safe here -- there is no "already loaded, need to
    // reload" case to special-case.
    HMODULE h = LoadLibraryA(dllPath.c_str());
    if (!h) {
        DWORD err = GetLastError();
        mod.error_ = "LoadLibrary failed for '" + dllPath + "' (GetLastError=" + std::to_string(err) +
                    ")";
        return mod;
    }

    std::vector<NativeClassExport> exports;
    exports.reserve(classNames.size());
    for (const auto& name : classNames) {
        NativeClassExport e;
        e.className = name;
        e.classInfo = reinterpret_cast<GetClassInfoFn>(GetProcAddress(h, ("GetClassInfo_" + name).c_str()));
        // A class is EITHER Component-shaped (CreateInstance_X/
        // DestroyInstance_X) OR static (CreateStatic_X/DestroyStatic_X),
        // never both -- try both pairs and use whichever CodeGen actually
        // emitted for this class (Phase 9e), so this loader needs no
        // separate "is this one static" input from the caller at all.
        e.create = reinterpret_cast<CreateInstanceFn>(GetProcAddress(h, ("CreateInstance_" + name).c_str()));
        e.destroy = reinterpret_cast<DestroyInstanceFn>(GetProcAddress(h, ("DestroyInstance_" + name).c_str()));
        e.createStatic = reinterpret_cast<CreateStaticFn>(GetProcAddress(h, ("CreateStatic_" + name).c_str()));
        e.destroyStatic = reinterpret_cast<DestroyStaticFn>(GetProcAddress(h, ("DestroyStatic_" + name).c_str()));
        const bool hasInstancePair = e.create && e.destroy;
        const bool hasStaticPair = e.createStatic && e.destroyStatic;
        if (!e.classInfo || !(hasInstancePair || hasStaticPair) || (hasInstancePair && hasStaticPair)) {
            mod.error_ = "missing (or ambiguous) export(s) for class '" + name + "' in '" + dllPath + "'";
            FreeLibrary(h);
            return mod;
        }
        e.isStatic = hasStaticPair;
        exports.push_back(e);
    }

    mod.handle_ = h;
    mod.exports_ = std::move(exports);
    return mod;
}

NativeModule::NativeModule(NativeModule&& other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)), exports_(std::move(other.exports_)),
      error_(std::move(other.error_)) {
    other.handle_ = nullptr;
}

NativeModule& NativeModule::operator=(NativeModule&& other) noexcept {
    if (this == &other)
        return *this;
    if (handle_)
        FreeLibrary(static_cast<HMODULE>(handle_));
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    exports_ = std::move(other.exports_);
    error_ = std::move(other.error_);
    other.handle_ = nullptr;
    return *this;
}

NativeModule::~NativeModule() {
    if (handle_)
        FreeLibrary(static_cast<HMODULE>(handle_));
}

const NativeClassExport* NativeModule::find(const std::string& className) const {
    for (const auto& e : exports_)
        if (e.className == className)
            return &e;
    return nullptr;
}

} // namespace crate::script
