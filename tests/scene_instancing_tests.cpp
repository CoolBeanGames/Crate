// Scenes task, Step 1: scene instancing + nesting (Godot-style). Covers
// Scene::instantiate(), the INSTANCE line round-trip through save()/load(),
// an instance's own transform being independent of its source, an instance
// nested inside another instance (arbitrary depth), and the cycle guard for
// a scene that would (directly or transitively) instance itself.
//
// No framework: asserts + a pass counter, run via CTest, matching every
// other test in this suite.

#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace crate;

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
    registerBuiltinComponents(); // normally done by EditorApp startup

    const std::string leafPath = "_leaf_scene_test.cscene";
    const std::string midPath = "_mid_scene_test.cscene";
    const std::string selfPath = "_self_scene_test.cscene";
    struct Cleanup {
        ~Cleanup() {
            std::remove("_leaf_scene_test.cscene");
            std::remove("_mid_scene_test.cscene");
            std::remove("_self_scene_test.cscene");
        }
    } cleanup;

    // --- a small "leaf" scene: one actor with a Light component ----------
    {
        Scene leaf("Leaf");
        auto lamp = std::make_unique<Actor3D>("Lamp");
        lamp->transform().position = {1.0f, 2.0f, 3.0f};
        lamp->addComponent(std::make_unique<LightComponent>());
        leaf.add(std::move(lamp));
        std::string err;
        CHECK(leaf.save(leafPath, &err));
    }

    // --- instantiate the leaf scene directly, reposition the instance ----
    {
        Scene world("World");
        std::string err;
        Actor* inst = world.instantiate(leafPath, nullptr, "LeafInstance", &err);
        CHECK(inst != nullptr);
        CHECK(err.empty());
        CHECK(inst->isInstanceRoot());
        CHECK(inst->instanceSource() == leafPath);
        CHECK(inst->name() == "LeafInstance");
        inst->transform().position = {10.0f, 0.0f, 0.0f}; // instance moved independent of source

        // The instance's children came from the source scene.
        CHECK(inst->children().size() == 1);
        CHECK(inst->children()[0]->name() == "Lamp");
        CHECK(inst->children()[0]->getComponent<LightComponent>() != nullptr);

        // Round-trip: save the world, reload it, confirm the instance
        // reference (not a full copy) survived and its own transform stuck.
        const std::string worldPath = "_world_scene_test.cscene";
        CHECK(world.save(worldPath, &err));
        Scene reloaded = Scene::load(worldPath, &err);
        std::remove(worldPath.c_str());
        CHECK(err.empty());
        Actor* reloadedInst = reloaded.atPath("LeafInstance");
        CHECK(reloadedInst != nullptr);
        CHECK(reloadedInst->isInstanceRoot());
        CHECK(reloadedInst->instanceSource() == leafPath);
        CHECK(reloadedInst->transform().position.x == 10.0f); // survived the round trip
        CHECK(reloadedInst->children().size() == 1);
        CHECK(reloadedInst->children()[0]->name() == "Lamp");
        CHECK(reloadedInst->children()[0]->getComponent<LightComponent>() != nullptr);
        std::printf("ok  instantiate() + save/load round-trip: instance reference, own "
                    "transform, and source-provided children all survive\n");
    }

    // --- nesting: a "mid" scene instances the leaf; a "world" instances ---
    // the mid scene, so the leaf's Lamp is TWO instance-levels deep.
    {
        Scene mid("Mid");
        std::string err;
        Actor* leafInst = mid.instantiate(leafPath, nullptr, "LeafInMid", &err);
        CHECK(leafInst != nullptr && err.empty());
        CHECK(mid.save(midPath, &err));

        Scene world("World2");
        Actor* midInst = world.instantiate(midPath, nullptr, "MidInWorld", &err);
        CHECK(midInst != nullptr && err.empty());
        CHECK(midInst->children().size() == 1);
        Actor* nestedLeaf = midInst->children()[0].get();
        CHECK(nestedLeaf->name() == "LeafInMid");
        CHECK(nestedLeaf->isInstanceRoot());
        CHECK(nestedLeaf->children().size() == 1);
        CHECK(nestedLeaf->children()[0]->name() == "Lamp");
        CHECK(nestedLeaf->children()[0]->getComponent<LightComponent>() != nullptr);
        std::printf("ok  nested instancing: an instance-of-an-instance resolves the full "
                    "chain (World2 -> MidInWorld -> LeafInMid -> Lamp)\n");
    }

    // --- cycle guard: a scene that instances itself must not hang --------
    {
        Scene self("Self");
        std::string err;
        // Write a placeholder first so the file exists, then instantiate it
        // into itself and re-save -- this is exactly the malformed case the
        // load-time cycle guard (not the instantiate-time one) must catch,
        // since save() happily writes whatever's in memory.
        CHECK(self.save(selfPath, &err));
        Actor* selfInst = self.instantiate(selfPath, nullptr, "SelfRef", &err);
        CHECK(selfInst != nullptr);
        CHECK(self.save(selfPath, &err)); // now the file references itself

        Scene reloaded = Scene::load(selfPath, &err);
        (void)reloaded; // must return promptly (no stack overflow / infinite loop)
        std::printf("ok  cycle guard: a scene instancing itself loads without hanging\n");
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
