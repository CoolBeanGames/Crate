// Round-trip checks for Scene::save()/Scene::load() (Scenes task, Phase 1:
// see scene/SceneIO.cpp for the file format). No framework: asserts + a pass
// counter, run via CTest, matching every other test in this suite.

#include "scene/Actor2D.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/NativeScriptComponent.h"
#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace crate;
using namespace crate::script;
namespace fs = std::filesystem;

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

static Value fieldOf(Component* c, const std::string& name) {
    if (auto* sc = dynamic_cast<ScriptComponent*>(c))
        return sc->object()->fields.at(name);
    if (auto* nc = dynamic_cast<NativeScriptComponent*>(c))
        return nc->object()->fields.at(name);
    std::fprintf(stderr, "fieldOf: not a script component\n");
    std::abort();
}

int main() {
    registerBuiltinComponents(); // normally done by EditorApp startup

    auto& sys = ScriptSystem::get();
    sys.loadFolder(CRATE_SCRIPTS_DIR);
    const std::string dir = sys.scriptsDir();

    const std::string path = dir + "/SceneIoProbe.cscript";
    {
        std::ofstream o(path, std::ios::binary);
        o << "class SceneIoProbe : Actor3D\n"
             "{\n"
             "    var count = 7;\n"
             "    var speed = 2.5;\n"
             "    var active = true;\n"
             "    var label = \"hello world\";\n"
             "    Actor buddy;\n"
             "    Light lamp;\n"
             "}\n";
    }
    struct Cleanup {
        ScriptSystem* sys;
        std::string path;
        ~Cleanup() {
            std::remove(path.c_str());
            sys->reload();
        }
    } cleanup{&sys, path};
    sys.reload();
    CHECK(sys.file("SceneIoProbe") != nullptr);

    // --- build a scene exercising every kind of thing SceneIO round-trips --
    Scene scene("IO Test Scene");
    auto* set = scene.add(std::make_unique<Actor3D>("Set"));

    auto lamp = std::make_unique<Actor3D>("Lamp");
    lamp->transform().position = {1.0f, 2.0f, 3.0f};
    lamp->transform().rotationEuler = {10.0f, 20.0f, 30.0f};
    lamp->transform().scale = {2.0f, 2.0f, 2.0f};
    {
        auto lc = std::make_unique<LightComponent>();
        lc->type = LightComponent::Type::Spot;
        lc->color[0] = 0.1f; lc->color[1] = 0.2f; lc->color[2] = 0.3f;
        lc->intensity = 1.7f;
        lc->range = 12.5f;
        lc->spotInnerDeg = 15.0f;
        lc->spotOuterDeg = 25.0f;
        lc->isStatic = true;
        lc->enabled = false;
        lamp->addComponent(std::move(lc));
    }
    Actor* lampPtr = scene.add(std::move(lamp), set);

    auto crate_ = std::make_unique<Actor3D>("Crate");
    {
        auto mr = std::make_unique<MeshRenderer>();
        mr->primitive = "Sphere";
        mr->texturePath = "assets/x.bmp";
        mr->tint[0] = 0.5f; mr->tint[1] = 0.6f; mr->tint[2] = 0.7f; mr->tint[3] = 0.8f;
        mr->castShadows = false;
        mr->radius = 3.25f;
        crate_->addComponent(std::move(mr));
        auto fog = std::make_unique<FogComponent>();
        fog->start = 4.0f; fog->end = 40.0f; fog->heightRange = 5.0f;
        crate_->addComponent(std::move(fog));
        auto vf = std::make_unique<VolumetricFogComponent>();
        vf->density = 0.66f;
        crate_->addComponent(std::move(vf));
        auto cam = std::make_unique<CameraComponent>();
        cam->fovY = 70.0f; cam->nearZ = 0.1f; cam->farZ = 250.0f;
        crate_->addComponent(std::move(cam));
        auto probe = std::make_unique<LightProbeComponent>();
        probe->bakedLight[0] = 0.9f; probe->bakedValid = true;
        crate_->addComponent(std::move(probe));
        auto spin = std::make_unique<SpinnerComponent>();
        spin->degreesPerSecond = 45.0f; spin->axis = 2;
        crate_->addComponent(std::move(spin));

        // Script component with plain fields + an actor-ref field + a
        // native-component-ref field (exactly what Inspector drag-drop
        // produces), exercising aref/cref round-tripping.
        auto comp = ComponentRegistry::get().create("SceneIoProbe");
        CHECK(comp != nullptr);
        Component* probeComp = crate_->addComponent(std::move(comp));
        if (auto* sc = dynamic_cast<ScriptComponent*>(probeComp)) {
            sc->object()->fields["count"] = Value::Int(99);
            sc->object()->fields["speed"] = Value::Float(3.5);
            sc->object()->fields["active"] = Value::Bool(false);
            sc->object()->fields["label"] = Value::Str("round trip");
            sc->object()->fields["buddy"] = Value::ActorRef(lampPtr);
            auto lo = std::make_shared<ScriptObject>();
            lo->builtin = "Light";
            lo->nativePtr = lampPtr->getComponent<LightComponent>();
            lo->owner = lampPtr;
            sc->object()->fields["lamp"] = Value::Obj(lo);
        } else {
            CHECK(false);
        }
    }
    scene.add(std::move(crate_), set);

    auto* cast = scene.add(std::make_unique<Actor>("Cast"));
    auto sprite = std::make_unique<SpriteActor>("Ghost");
    sprite->texturePath = "assets/ghost.png";
    sprite->tint[0] = 0.2f; sprite->tint[3] = 0.4f;
    scene.add(std::move(sprite), cast);

    auto* hud = scene.add(std::make_unique<Actor2D>("Hud"));
    auto ui = std::make_unique<UIControlActor>("Btn");
    ui->label = "Go";
    ui->size[0] = 111.0f; ui->size[1] = 22.0f;
    ui->setVisible(false);
    scene.add(std::move(ui), hud);

    const std::string scenePath = dir + "/_scene_io_test.cscene";
    std::string err;
    CHECK(scene.save(scenePath, &err));
    std::printf("ok  Scene::save wrote %s\n", scenePath.c_str());

    Scene loaded = Scene::load(scenePath, &err);
    std::remove(scenePath.c_str());
    CHECK(err.empty());
    CHECK(loaded.name() == "IO Test Scene");
    CHECK(loaded.actorCount() == scene.actorCount());

    Actor* lSet = loaded.atPath("Set");
    CHECK(lSet != nullptr);
    Actor* lLamp = loaded.atPath("Set/Lamp");
    CHECK(lLamp != nullptr);
    CHECK(std::fabs(lLamp->transform().position.x - 1.0f) < 1e-4f);
    CHECK(std::fabs(lLamp->transform().position.z - 3.0f) < 1e-4f);
    CHECK(std::fabs(lLamp->transform().rotationEuler.y - 20.0f) < 1e-4f);
    CHECK(std::fabs(lLamp->transform().scale.x - 2.0f) < 1e-4f);
    auto* lc = lLamp->getComponent<LightComponent>();
    CHECK(lc != nullptr);
    CHECK(lc->type == LightComponent::Type::Spot);
    CHECK(std::fabs(lc->color[1] - 0.2f) < 1e-4f);
    CHECK(std::fabs(lc->intensity - 1.7f) < 1e-4f);
    CHECK(std::fabs(lc->range - 12.5f) < 1e-4f);
    CHECK(std::fabs(lc->spotInnerDeg - 15.0f) < 1e-4f);
    CHECK(lc->isStatic == true);
    CHECK(lc->enabled == false); // Component::enabled round-trips via the COMP line
    std::printf("ok  Light component fields + enabled flag round-tripped\n");

    Actor* lCrate = loaded.atPath("Set/Crate");
    CHECK(lCrate != nullptr);
    auto* mr = lCrate->getComponent<MeshRenderer>();
    CHECK(mr != nullptr);
    CHECK(mr->primitive == "Sphere");
    CHECK(mr->texturePath == "assets/x.bmp");
    CHECK(std::fabs(mr->tint[2] - 0.7f) < 1e-4f);
    CHECK(std::fabs(mr->tint[3] - 0.8f) < 1e-4f);
    CHECK(mr->castShadows == false);
    CHECK(std::fabs(mr->radius - 3.25f) < 1e-4f);
    auto* fog = lCrate->getComponent<FogComponent>();
    CHECK(fog && std::fabs(fog->heightRange - 5.0f) < 1e-4f);
    auto* vf = lCrate->getComponent<VolumetricFogComponent>();
    CHECK(vf && std::fabs(vf->density - 0.66f) < 1e-4f);
    auto* cam = lCrate->getComponent<CameraComponent>();
    CHECK(cam && std::fabs(cam->fovY - 70.0f) < 1e-4f);
    auto* probe = lCrate->getComponent<LightProbeComponent>();
    CHECK(probe && probe->bakedValid && std::fabs(probe->bakedLight[0] - 0.9f) < 1e-4f);
    auto* spin = lCrate->getComponent<SpinnerComponent>();
    CHECK(spin && spin->axis == 2 && std::fabs(spin->degreesPerSecond - 45.0f) < 1e-4f);
    std::printf("ok  MeshRenderer/Fog/VolumetricFog/Camera/LightProbe/Spinner fields round-tripped\n");

    Component* lProbeComp = nullptr;
    for (const auto& c : lCrate->components())
        if (dynamic_cast<ScriptComponent*>(c.get()) || dynamic_cast<NativeScriptComponent*>(c.get()))
            lProbeComp = c.get();
    CHECK(lProbeComp != nullptr);
    CHECK(fieldOf(lProbeComp, "count").i == 99);
    CHECK(std::fabs(fieldOf(lProbeComp, "speed").f - 3.5) < 1e-6);
    CHECK(fieldOf(lProbeComp, "active").b == false);
    CHECK(fieldOf(lProbeComp, "label").s == "round trip");
    Value buddy = fieldOf(lProbeComp, "buddy");
    CHECK(buddy.t == Value::T::Actor);
    CHECK(buddy.actor == lLamp); // actor-ref (aref) resolved to the SAME loaded actor
    Value lampRef = fieldOf(lProbeComp, "lamp");
    CHECK(lampRef.t == Value::T::Object && lampRef.obj && lampRef.obj->nativePtr == lc); // cref resolved live
    std::printf("ok  script fields incl. actor-ref (aref) and native-component-ref (cref) round-tripped\n");

    Actor* lGhost = loaded.atPath("Cast/Ghost");
    CHECK(lGhost != nullptr);
    CHECK(std::string(lGhost->typeName()) == "SPRITE");
    auto* lSprite = dynamic_cast<SpriteActor*>(lGhost);
    CHECK(lSprite && lSprite->texturePath == "assets/ghost.png");
    CHECK(lSprite && std::fabs(lSprite->tint[3] - 0.4f) < 1e-4f);

    Actor* lBtn = loaded.atPath("Hud/Btn");
    CHECK(lBtn != nullptr);
    CHECK(std::string(lBtn->typeName()) == "UI");
    CHECK(lBtn->visible() == false);
    auto* lUi = dynamic_cast<UIControlActor*>(lBtn);
    CHECK(lUi && lUi->label == "Go");
    CHECK(lUi && std::fabs(lUi->size[0] - 111.0f) < 1e-4f);
    std::printf("ok  Actor-level extra fields (SpriteActor/UIControlActor) + visible flag round-tripped\n");

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
