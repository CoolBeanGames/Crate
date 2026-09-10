// Lightweight checks for the Actor / Actor2D / Actor3D hierarchy and Transform
// inheritance. No framework: asserts + a pass counter, run via CTest.

#include "scene/Actor2D.h"
#include "scene/Actor3D.h"
#include "scene/Scene.h"

#include <cassert>
#include <cstdio>

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
    // Base types exist and report their labels.
    Actor a("a");
    Actor2D a2("a2");
    Actor3D a3("a3");
    MeshActor mesh("m");
    CHECK(std::string(a.typeName()) == "ACTOR");
    CHECK(std::string(a2.typeName()) == "ACTOR2D");
    CHECK(std::string(a3.typeName()) == "ACTOR3D");
    CHECK(std::string(mesh.typeName()) == "MESH");
    CHECK(mesh.id() != a.id()); // unique ids

    // Polymorphism through Actor*.
    Actor* poly = &mesh;
    CHECK(dynamic_cast<Actor3D*>(poly) != nullptr);
    CHECK(dynamic_cast<Actor2D*>(poly) == nullptr);

    // Scene add + count.
    Scene s("t");
    CHECK(s.actorCount() == 0);
    auto* parent = s.add(std::make_unique<Actor3D>("parent"));
    parent->transform().position = {10.0f, 0.0f, 0.0f};
    parent->transform().scale = {2.0f, 2.0f, 2.0f};
    CHECK(s.actorCount() == 1);

    // Child inherits parent world transform.
    auto* child = s.add(std::make_unique<MeshActor>("child"), parent);
    child->transform().position = {1.0f, 0.0f, 0.0f};
    CHECK(s.actorCount() == 2);
    CHECK(child->parent() == parent);
    Transform w = child->worldTransform();
    CHECK(w.position.x == 12.0f); // 10 + 1 * 2
    CHECK(w.scale.x == 2.0f);

    // Selection.
    s.select(child);
    CHECK(s.selected() == child);

    // Reparent guards against cycles.
    parent->reparent(child);
    CHECK(parent->parent() != child);

    // Removing a subtree updates the count and clears stale selection.
    s.remove(parent);
    CHECK(s.actorCount() == 0);
    CHECK(s.selected() == nullptr);

    // Sample scene builds.
    Scene sample = Scene::makeSample();
    CHECK(sample.actorCount() > 4);

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
