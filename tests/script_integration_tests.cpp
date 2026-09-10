// End-to-end: load scripts like the engine does, add one to an actor through
// the component registry, run the play lifecycle, and check it took effect.

#include "scene/Actor3D.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"

#include <cstdio>

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

    std::printf("ok  Mover ran: rotation.y = %.1f after play\n", y);
    return 0;
}
