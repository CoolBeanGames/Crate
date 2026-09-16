// Coverage for src/script/NativeModule.h/.cpp and NativeClassRegistry.h/.cpp
// (transpiration.txt, "Transplation" Phase 4): build a trivial one-class
// namespace via Phase 3's real ScriptBuild pipeline, load the resulting DLL,
// drive it two ways -- (1) the raw extern "C" ABI directly, and (2) through
// crate::ComponentRegistry exactly as the Add-Component menu / Scene tick
// loop already uses it -- verify observable field/transform effects
// (mirroring script_integration_tests.cpp's Mover pattern, since Phase 4
// has no generic field-reflection table yet -- that's Phase 5), then tear
// everything down and confirm no crash.
//
// Skips the whole test gracefully (not a failure) if no MSVC toolchain is
// discoverable, matching script_build_tests.cpp's convention.
//
// No framework: asserts + a pass counter, run via CTest.

#include "editor/ScriptBuild.h"
#include "scene/Actor3D.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/NativeClassRegistry.h"
#include "script/NativeModule.h"
#include "script/ScriptSystem.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace crate;
using namespace crate::script;
using namespace crate::editor::scriptbuild;

#ifndef CRATE_SCRIPTS_DIR
#define CRATE_SCRIPTS_DIR "."
#endif

static int g_checks = 0;
#define CHECK(cond)                                                                   \
    do {                                                                             \
        ++g_checks;                                                                  \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            return 1;                                                                \
        }                                                                           \
    } while (0)

int main() {
    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    if (!ToolchainEnv::get().available()) {
        std::printf("(no MSVC toolchain found -- native_module_tests skipped: %s)\n",
                    ToolchainEnv::get().error().c_str());
        return 0;
    }

    const std::string ns = "NmTestNamespace";
    const std::string pMover = dir + "/NmTestMover.cscript";
    {
        std::ofstream o(pMover, std::ios::binary);
        // Mirrors the shipped Mover.cscript / script_integration_tests.cpp's
        // pattern exactly: an observable, engine-visible side effect
        // (rotation.y) that doesn't require any field-reflection table to
        // verify -- Phase 4 doesn't have one yet (Phase 5).
        o << "class NmTestMover : Actor3D { var speed = 20; func update(float delta) { "
             "transform.rotation.y += speed * delta; } }";
    }
    sys.reload();
    CHECK(sys.file("NmTestMover") != nullptr);
    sys.setNamespace("NmTestMover", ns);

    struct Cleanup {
        ScriptSystem* sys;
        std::string p, scriptsDir, ns;
        ~Cleanup() {
            NativeClassRegistry::get().unloadAll(); // safe even if nothing was loaded
            std::remove(p.c_str());
            sys->reload();
            std::error_code ec;
            std::filesystem::remove_all(scriptsDir + "/.crate_build/gen/" + ns, ec);
            std::error_code ec2;
            for (const auto& entry :
                std::filesystem::directory_iterator(scriptsDir + "/.crate_build/bin", ec2)) {
                if (ec2)
                    break;
                if (entry.path().filename().string().rfind(ns + "_v", 0) == 0)
                    std::filesystem::remove(entry.path(), ec);
            }
            const std::string manifestPath = scriptsDir + "/.crate_build/manifest.tsv";
            std::ifstream in(manifestPath, std::ios::binary);
            if (in) {
                std::string kept, line;
                while (std::getline(in, line))
                    if (line.rfind(ns + "\t", 0) != 0)
                        kept += line + "\n";
                in.close();
                std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
                out << kept;
            }
        }
    } cleanup{&sys, pMover, dir, ns};

    BuildResult build = buildNamespace(ns, dir);
    if (!build.ok)
        std::printf("  buildNamespace error: %s\n", build.error.c_str());
    CHECK(build.ok);
    std::printf("ok  namespace built: %s\n", build.dllPath.c_str());

    // ---- (1) raw ABI, driven directly ----------------------------------
    {
        NativeModule mod = NativeModule::load(build.dllPath, {"NmTestMover"});
        if (!mod.ok())
            std::printf("  NativeModule::load error: %s\n", mod.error().c_str());
        CHECK(mod.ok());
        CHECK(mod.exports().size() == 1);
        std::printf("ok  NativeModule::load() resolves CreateInstance/DestroyInstance/GetClassInfo\n");

        const NativeClassExport* exp = mod.find("NmTestMover");
        CHECK(exp != nullptr);
        CHECK(exp->create != nullptr);
        CHECK(exp->destroy != nullptr);
        CHECK(exp->classInfo != nullptr);
        const NativeClassExport& e = *exp;

        const CompiledClassInfo* ci = e.classInfo();
        CHECK(ci != nullptr);
        CHECK(std::string(ci->className) == "NmTestMover");
        std::printf("ok  GetClassInfo_NmTestMover() reports the correct class name\n");

        Actor3D actor("subject");
        actor.transform().rotationEuler.y = 0.0f;
        Component* comp = e.create(&sys.context(), &actor);
        CHECK(comp != nullptr);
        CHECK(std::string(comp->typeName()) == "NmTestMover");

        for (int i = 0; i < 10; ++i)
            comp->update(0.1f); // speed(20) * delta(0.1) * 10 frames -> 20 degrees

        float y = actor.transform().rotationEuler.y;
        CHECK(y > 15.0f && y < 25.0f);
        std::printf("ok  driving update() through the raw Component* interface moves the real Actor "
                    "(rotation.y = %.2f)\n",
                    y);

        e.destroy(comp);
        std::printf("ok  DestroyInstance_NmTestMover() does not crash\n");
        // `mod` goes out of scope here -> ~NativeModule() -> FreeLibrary();
        // reaching the end of this block without crashing is itself part of
        // what this test verifies.
    }
    std::printf("ok  NativeModule teardown (FreeLibrary) did not crash\n");

    // ---- (2) through ComponentRegistry, exactly like the Add-Component
    // menu and the real Scene tick loop already use it -------------------
    {
        bool loaded = NativeClassRegistry::get().loadNamespace(ns, build.dllPath, {"NmTestMover"});
        CHECK(loaded);
        CHECK(NativeClassRegistry::get().isLoaded(ns));
        CHECK(ComponentRegistry::get().has("NmTestMover"));
        std::printf("ok  NativeClassRegistry::loadNamespace() registers into ComponentRegistry\n");

        {
            // Scoped so the Scene (and every Component it owns, including
            // the native one) is destroyed BEFORE this test's Cleanup guard
            // calls NativeClassRegistry::unloadAll() below -- a native
            // component's vtable/code lives inside the DLL, so destroying
            // it AFTER FreeLibrary would be a use-after-free. This ordering
            // requirement is exactly why Phase 7 (Stop handling) must tear
            // down all native components before ever unloading their DLL;
            // noted in transpiration.txt for that phase.
            Scene scene("t");
            Actor* a = scene.add(std::make_unique<Actor3D>("Subject2"));
            auto comp = ComponentRegistry::get().create("NmTestMover");
            CHECK(comp != nullptr);
            a->addComponent(std::move(comp));

            scene.startPlay();
            for (int i = 0; i < 10; ++i)
                scene.tick(0.1f);

            float y = a->transform().rotationEuler.y;
            CHECK(y > 15.0f && y < 25.0f);
            std::printf("ok  ComponentRegistry::create() + Scene::tick() moves the Actor identically "
                        "(rotation.y = %.2f)\n",
                        y);
        }

        NativeClassRegistry::get().unloadNamespace(ns);
        CHECK(!NativeClassRegistry::get().isLoaded(ns));
        // unloadNamespace() restores the INTERPRETED registration rather
        // than removing the class outright -- see
        // NativeClassRegistry.cpp's restoreInterpretedRegistration() and
        // native_reflection_tests.cpp, which covers this specifically. The
        // class must stay addable (running interpreted) after a Stop
        // (Phase 7), not vanish from the Add-Component menu.
        CHECK(ComponentRegistry::get().has("NmTestMover"));
        std::printf("ok  unloadNamespace() frees the module and restores the interpreted "
                    "registration (class stays addable)\n");
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
