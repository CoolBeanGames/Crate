// Task 146: a disabled actor's light was still contributing to the scene --
// Renderer::collectLights only checked LightComponent::enabled and
// Actor::visible(), never Actor::enabled(). activeLightCount() is pure CPU
// (no D3D11 device needed), same testability precedent as bakeLighting().

#include "render/Renderer.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/Scene.h"

#include <cstdio>

using namespace crate;

static int fails = 0;
#define CHECK(c)                                                                                    \
    do {                                                                                            \
        if (!(c)) {                                                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);                                \
            ++fails;                                                                                \
        }                                                                                           \
    } while (0)

static Actor* addLight(Scene& scene, const char* name, LightComponent::Type type) {
    Actor* a = scene.add(std::make_unique<Actor3D>(name));
    auto lc = std::make_unique<LightComponent>();
    lc->type = type;
    lc->intensity = 1.0f;
    a->addComponent(std::move(lc));
    return a;
}

int main() {
    // A single enabled, visible light with an enabled, visible LightComponent
    // is collected -- the ordinary, unmodified default state.
    {
        Scene scene("default-state");
        addLight(scene, "Sun", LightComponent::Type::Directional);
        Renderer r;
        CHECK(r.activeLightCount(scene) == 1);
    }

    // Actor-level Enabled=false must exclude the light (task 146's exact
    // repro: unchecking the Inspector's actor "Enabled" checkbox on a light
    // did nothing before this fix).
    {
        Scene scene("actor-disabled");
        Actor* sun = addLight(scene, "Sun", LightComponent::Type::Directional);
        sun->setEnabled(false);
        Renderer r;
        CHECK(r.activeLightCount(scene) == 0);
    }

    // Actor-level Visible=false must still exclude the light (pre-existing
    // behavior, confirm the fix didn't regress it).
    {
        Scene scene("actor-invisible");
        Actor* lamp = addLight(scene, "Lamp", LightComponent::Type::Point);
        lamp->setVisible(false);
        Renderer r;
        CHECK(r.activeLightCount(scene) == 0);
    }

    // Component-level LightComponent::enabled=false must still exclude the
    // light regardless of the actor's own enabled/visible state (pre-existing
    // behavior, confirm the fix didn't regress it).
    {
        Scene scene("component-disabled");
        Actor* lamp = addLight(scene, "Lamp", LightComponent::Type::Point);
        lamp->getComponent<LightComponent>()->enabled = false;
        Renderer r;
        CHECK(r.activeLightCount(scene) == 0);
    }

    // Actor enabled but a child's light disabled: only the enabled one
    // contributes -- confirms the fix applies per-actor during the tree walk,
    // not as some global short-circuit.
    {
        Scene scene("mixed");
        addLight(scene, "Sun", LightComponent::Type::Directional);
        Actor* lamp = addLight(scene, "Lamp", LightComponent::Type::Point);
        lamp->setEnabled(false);
        Renderer r;
        CHECK(r.activeLightCount(scene) == 1);
    }

    // Re-enabling a previously-disabled actor's light must bring it back --
    // confirms this isn't a one-way/cached state.
    {
        Scene scene("re-enable");
        Actor* sun = addLight(scene, "Sun", LightComponent::Type::Directional);
        sun->setEnabled(false);
        Renderer r;
        CHECK(r.activeLightCount(scene) == 0);
        sun->setEnabled(true);
        CHECK(r.activeLightCount(scene) == 1);
    }

    // The fix applies during the recursive tree walk, not just to root-level
    // actors: a disabled light nested two levels deep is still excluded, and
    // its enabled sibling still contributes.
    {
        Scene scene("nested");
        Actor* parent = scene.add(std::make_unique<Actor3D>("Parent"));
        Actor* child = parent->addChild(std::make_unique<Actor3D>("Child"));
        auto lc = std::make_unique<LightComponent>();
        lc->type = LightComponent::Type::Spot;
        lc->intensity = 1.0f;
        child->addComponent(std::move(lc));
        addLight(scene, "Other", LightComponent::Type::Directional);
        Renderer r;
        CHECK(r.activeLightCount(scene) == 2);
        child->setEnabled(false);
        CHECK(r.activeLightCount(scene) == 1);
    }

    if (fails == 0)
        std::printf("light_enabled_tests: all checks passed\n");
    return fails == 0 ? 0 : 1;
}
