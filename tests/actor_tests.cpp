// Lightweight checks for the Actor / Actor2D / Actor3D hierarchy and Transform
// inheritance. No framework: asserts + a pass counter, run via CTest.

#include "scene/Actor2D.h"
#include "scene/Actor3D.h"
#include "scene/BuiltinComponents.h"
#include "scene/Scene.h"

#include <cassert>
#include <cmath>
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
    SpriteActor sprite("s");
    CHECK(std::string(a.typeName()) == "ACTOR");
    CHECK(std::string(a2.typeName()) == "ACTOR2D");
    CHECK(std::string(a3.typeName()) == "ACTOR3D");
    CHECK(std::string(sprite.typeName()) == "SPRITE");
    CHECK(a3.id() != a.id()); // unique ids

    // Polymorphism through Actor*.
    Actor* poly = &sprite;
    CHECK(dynamic_cast<Actor2D*>(poly) != nullptr);
    CHECK(dynamic_cast<Actor3D*>(poly) == nullptr);

    // Scene add + count.
    Scene s("t");
    CHECK(s.actorCount() == 0);
    auto* parent = s.add(std::make_unique<Actor3D>("parent"));
    parent->transform().position = {10.0f, 0.0f, 0.0f};
    parent->transform().scale = {2.0f, 2.0f, 2.0f};
    CHECK(s.actorCount() == 1);

    // Child inherits parent world transform.
    auto* child = s.add(std::make_unique<Actor3D>("child"), parent);
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

    // --- reparent keeps world transform -----------------------------------
    Scene r("r");
    auto* p1 = r.add(std::make_unique<Actor3D>("p1"));
    p1->transform().position = {5.0f, 0.0f, 0.0f};
    auto* p2 = r.add(std::make_unique<Actor3D>("p2"));
    p2->transform().position = {0.0f, 8.0f, 0.0f};
    auto* leaf = r.add(std::make_unique<Actor3D>("leaf"), p1);
    leaf->transform().position = {1.0f, 0.0f, 0.0f};
    Transform before = leaf->worldTransform();
    CHECK(r.reparent(leaf, p2, -1, true));
    CHECK(leaf->parent() == p2);
    Transform after = leaf->worldTransform();
    CHECK(std::abs(after.position.x - before.position.x) < 0.001f);
    CHECK(std::abs(after.position.y - before.position.y) < 0.001f);

    // Reparent rejects cycles.
    CHECK(!r.reparent(p2, leaf, -1, true));

    // Unparent to root.
    CHECK(r.reparent(leaf, nullptr, -1, true));
    CHECK(leaf->parent() == &r.root());

    // --- duplicate makes a deep, independent copy -------------------------
    Scene d("d");
    auto* orig = d.add(std::make_unique<Actor3D>("orig"));
    {
        auto mr = std::make_unique<MeshRenderer>();
        mr->primitive = "Sphere";
        orig->addComponent(std::move(mr));
    }
    d.add(std::make_unique<Actor3D>("origChild"), orig);
    int countBefore = d.actorCount();
    Actor* dup = d.duplicate(orig);
    CHECK(dup != nullptr);
    CHECK(d.actorCount() == countBefore * 2);
    CHECK(dup->children().size() == 1);
    CHECK(dup->getComponent<MeshRenderer>() != nullptr);
    CHECK(dup->getComponent<MeshRenderer>()->primitive == "Sphere");
    CHECK(dup->id() != orig->id());

    // --- enabled flag is independent of visible --------------------------
    orig->setEnabled(false);
    CHECK(!orig->enabled());
    CHECK(orig->visible());

    // --- components: attach, tick, clone --------------------------------
    Scene cs("cs");
    auto* host = cs.add(std::make_unique<Actor3D>("host"));
    auto* spin = static_cast<SpinnerComponent*>(
        host->addComponent(std::make_unique<SpinnerComponent>()));
    spin->degreesPerSecond = 100.0f;
    spin->axis = 1;
    CHECK(host->getComponent<SpinnerComponent>() == spin);
    CHECK(spin->actor() == host);

    float y0 = host->transform().rotationEuler.y;
    cs.startPlay();
    cs.tick(0.5f); // 100 deg/s * 0.5s = 50 deg
    CHECK(std::abs(host->transform().rotationEuler.y - (y0 + 50.0f)) < 0.01f);

    // Clone carries components with their settings.
    std::unique_ptr<Actor> hostCopy = host->clone();
    auto* spinCopy = hostCopy->getComponent<SpinnerComponent>();
    CHECK(spinCopy != nullptr);
    CHECK(spinCopy != spin);
    CHECK(spinCopy->degreesPerSecond == 100.0f);
    CHECK(spinCopy->actor() == hostCopy.get());

    host->removeComponent(spin);
    CHECK(host->getComponent<SpinnerComponent>() == nullptr);

    // --- AUUIDs: unique per instance, path map follows moves --------------
    {
        Actor x("x"), y("y");
        CHECK(!x.auid().empty());
        CHECK(x.auid() != y.auid());
        CHECK(x.auid().size() == 36); // 8-4-4-4-12

        Scene sc("s");
        Actor* set = sc.add(std::make_unique<Actor3D>("SET"));
        Actor* cast = sc.add(std::make_unique<Actor3D>("CAST"));
        Actor* crate = sc.add(std::make_unique<Actor3D>("Crate"), set);

        CHECK(sc.pathOf(crate) == "SET/Crate");
        CHECK(sc.atPath("SET/Crate") == crate);
        CHECK(sc.atPath("nope") == nullptr);

        std::string crateAuid = crate->auid();
        sc.reparent(crate, cast);
        CHECK(sc.pathOf(crate) == "CAST/Crate");     // path changed
        CHECK(crate->auid() == crateAuid);           // identity did not
        CHECK(sc.atPath("SET/Crate") == nullptr);
        CHECK(sc.atPath("CAST/Crate") == crate);

        // Duplicate is a new instance -> new AUUID.
        Actor* dup = sc.duplicate(crate);
        CHECK(dup->auid() != crate->auid());

        // Every entry in the table is a distinct AUUID.
        auto table = sc.actorTable();
        CHECK(table.size() == static_cast<size_t>(sc.actorCount()));
        for (size_t i = 0; i < table.size(); ++i)
            for (size_t j = i + 1; j < table.size(); ++j)
                CHECK(table[i].second != table[j].second);

        // Play-mode snapshot preserves AUUIDs so Stop restores identity.
        Scene snap = sc.clone();
        CHECK(snap.atPath("CAST/Crate")->auid() == crateAuid);
    }

    std::printf("ok  %d checks passed\n", g_checks);
    return 0;
}
