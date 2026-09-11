// Rendering task 59: static lights only reach meshes/probes via a bake.
// bakeLighting() is pure CPU (no D3D11 device needed), so it's unit-testable.

#include "render/Renderer.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/Scene.h"

#include <cmath>
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

int main() {
    Scene scene("bake");
    Actor* lightActor = scene.add(std::make_unique<Actor3D>("StaticLight"));
    lightActor->transform().position = {0, 2, 0};
    auto lc = std::make_unique<LightComponent>();
    lc->type = LightComponent::Type::Point;
    lc->isStatic = true;
    lc->range = 10.0f;
    lc->intensity = 2.0f;
    lc->color[0] = 1.0f; lc->color[1] = 1.0f; lc->color[2] = 1.0f;
    lightActor->addComponent(std::move(lc));

    Actor* mesh = scene.add(std::make_unique<Actor3D>("Lit"));
    mesh->transform().position = {0, 0, 0};
    mesh->addComponent(std::make_unique<MeshRenderer>());
    auto* mr = mesh->getComponent<MeshRenderer>();

    Actor* probeActor = scene.add(std::make_unique<Actor3D>("Probe"));
    probeActor->transform().position = {3, 0, 0};
    probeActor->addComponent(std::make_unique<LightProbeComponent>());
    auto* probe = probeActor->getComponent<LightProbeComponent>();

    // A second, dynamic (non-static) light must NOT contribute to the bake.
    Actor* dynLightActor = scene.add(std::make_unique<Actor3D>("DynLight"));
    dynLightActor->transform().position = {0, 2, 0};
    auto dlc = std::make_unique<LightComponent>();
    dlc->type = LightComponent::Type::Point;
    dlc->isStatic = false;
    dlc->intensity = 5.0f;
    dynLightActor->addComponent(std::move(dlc));

    CHECK(!mr->bakedValid);
    CHECK(!probe->bakedValid);

    Renderer r; // no device attached; bakeLighting is pure CPU
    r.bakeLighting(scene);

    CHECK(mr->bakedValid);
    CHECK(probe->bakedValid);
    // The mesh sits right under the static light -> a bright bake.
    CHECK(mr->bakedLight[0] > 0.3f);
    CHECK(std::isfinite(mr->bakedLight[0]));
    // The probe is farther away (3 units, range 10) -> dimmer but non-zero.
    CHECK(probe->bakedLight[0] > 0.0f);
    CHECK(probe->bakedLight[0] < mr->bakedLight[0]);

    // Baking twice is idempotent (no accumulation).
    float first = mr->bakedLight[0];
    r.bakeLighting(scene);
    CHECK(std::fabs(mr->bakedLight[0] - first) < 1e-4f);

    if (fails == 0)
        std::printf("ok  static-light baking: meshes + probes, dynamic lights excluded\n");
    return fails ? 1 : 0;
}
