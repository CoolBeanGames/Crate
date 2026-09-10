// End-to-end: load scripts like the engine does, add one to an actor through
// the component registry, run the play lifecycle, and check it took effect.

#include "scene/Actor3D.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"

#include <cstdio>
#include <fstream>

using namespace crate;

#ifndef CRATE_SCRIPTS_DIR
#define CRATE_SCRIPTS_DIR "."
#endif

int main() {
    auto& sys = script::ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);

    // The shipped Mover script must have compiled and registered a component.
    bool registered = false;
    for (const auto& e : ComponentRegistry::get().entries())
        if (e.name == "Mover")
            registered = true;
    if (!registered) {
        std::printf("FAIL: 'Mover' not registered as a component\n");
        return 1;
    }

    Scene scene("t");
    Actor* a = scene.add(std::make_unique<Actor3D>("Subject"));
    a->transform().rotationEuler = {0, 0, 0};

    auto comp = ComponentRegistry::get().create("Mover");
    if (!comp) {
        std::printf("FAIL: could not create Mover component\n");
        return 1;
    }
    a->addComponent(std::move(comp));

    // Play: start, then a few update frames.
    scene.startPlay();
    for (int i = 0; i < 10; ++i)
        scene.tick(0.1f); // Mover adds speed*delta to rotation.y each frame

    float y = a->transform().rotationEuler.y;
    // speed defaults to 20, 10 frames * 0.1s -> 20 degrees.
    if (y < 15.0f || y > 25.0f) {
        std::printf("FAIL: expected rotation.y ~20, got %f\n", y);
        return 1;
    }

    // --- New Script: named, template compiles + registers ----------
    {
        std::string name = sys.newScript("Widget");
        if (name != "Widget") {
            std::printf("FAIL: newScript(\"Widget\") -> '%s'\n", name.c_str());
            if (!name.empty())
                std::remove(sys.file(name)->path.c_str());
            return 1;
        }
        bool inRegistry = false;
        for (const auto& e : ComponentRegistry::get().entries())
            if (e.name == name)
                inRegistry = true;
        if (!inRegistry || !sys.file(name)) {
            std::printf("FAIL: new script '%s' not registered / no file\n", name.c_str());
            return 1;
        }
        // the template must have the three lifecycle hooks
        auto* ci = sys.types().at(name).get();
        if (!ci->findFunction("start") || !ci->findFunction("update") ||
            !ci->findFunction("physics_update")) {
            std::printf("FAIL: template missing lifecycle functions\n");
            return 1;
        }
        std::remove(sys.file(name)->path.c_str()); // don't leave it in the repo
        std::printf("ok  newScript created '%s' (compiles, registered)\n", name.c_str());
    }

    // --- recompile in place: pointer stable, object rebuilds (task 27) ---
    {
        std::string err;
        sys.compile("class LiveEdit : Actor3D { var tag = 1; func update(float d) {} }", &err);
        const void* p1 = sys.types().at("LiveEdit").get();

        Scene s2("s2");
        Actor* host = s2.add(std::make_unique<Actor3D>("H"));
        host->addComponent(ComponentRegistry::get().create("LiveEdit"));
        s2.startPlay();
        s2.tick(0.1f);
        auto* sc = dynamic_cast<script::ScriptComponent*>(host->components()[0].get());
        if (sc->object()->fields.at("tag").i != 1) {
            std::printf("FAIL: initial field wrong\n");
            return 1;
        }

        // edit the class while it's attached and playing
        sys.compile("class LiveEdit : Actor3D { var tag = 42; func update(float d) {} }", &err);
        const void* p2 = sys.types().at("LiveEdit").get();
        if (p1 != p2) {
            std::printf("FAIL: ClassInfo pointer changed on recompile\n");
            return 1;
        }
        s2.tick(0.1f); // ensureObject() should notice the new generation
        if (sc->object()->fields.at("tag").i != 42) {
            std::printf("FAIL: field did not pick up the edit (got %lld)\n",
                        (long long)sc->object()->fields.at("tag").i);
            return 1;
        }
        std::printf("ok  live recompile: pointer stable, object rebuilt with new code\n");
    }

    // --- reload: pick up an on-disk add, then a delete (task 28) --------
    {
        const std::string dir = sys.scriptsDir();
        const std::string p = dir + "/Dropped.cscript";
        {
            std::ofstream o(p, std::ios::binary);
            o << "class Dropped : Actor { func update(float d) {} }";
        }
        sys.reload();
        if (!sys.file("Dropped") || !ComponentRegistry::get().has("Dropped")) {
            std::printf("FAIL: reload did not pick up a new script\n");
            std::remove(p.c_str());
            return 1;
        }
        std::remove(p.c_str());
        sys.reload();
        if (sys.file("Dropped") || ComponentRegistry::get().has("Dropped")) {
            std::printf("FAIL: reload did not drop a deleted script\n");
            return 1;
        }
        std::printf("ok  reload: new script added, deleted script removed\n");
    }

    // --- class rename cleans up the old menu entry (task 29) -----------
    {
        const std::string dir = sys.scriptsDir();
        const std::string p = dir + "/Renamer.cscript";
        {
            std::ofstream o(p, std::ios::binary);
            o << "class Alpha : Actor { func update(float d) {} }";
        }
        sys.reload();
        auto* f = sys.file("Alpha");
        if (!f || !ComponentRegistry::get().has("Alpha")) {
            std::printf("FAIL: Alpha not loaded\n");
            std::remove(p.c_str());
            return 1;
        }
        std::string nowName =
            sys.setSource("Alpha", "class Beta : Actor { func update(float d) {} }");
        std::remove(p.c_str());
        if (nowName != "Beta" || ComponentRegistry::get().has("Alpha") ||
            !ComponentRegistry::get().has("Beta")) {
            std::printf("FAIL: rename cleanup (name=%s, hasAlpha=%d, hasBeta=%d)\n", nowName.c_str(),
                        ComponentRegistry::get().has("Alpha"), ComponentRegistry::get().has("Beta"));
            return 1;
        }
        std::printf("ok  class rename: old component entry removed, new one added\n");
    }

    std::printf("ok  Mover ran: rotation.y = %.1f after play\n", y);
    return 0;
}
