// Regression tests for user-reported scripting bugs (Zen tasks 45, 46).

#include "scene/Actor3D.h"
#include "scene/ComponentRegistry.h"
#include "scene/Scene.h"
#include "script/ScriptComponent.h"
#include "script/ScriptSystem.h"

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

static Actor* attach(Scene& s, script::ScriptSystem& sys, const char* src, const char* cls) {
    std::string err;
    if (!sys.compile(src, &err)) {
        std::printf("FAIL compile %s: %s\n", cls, err.c_str());
        ++fails;
        return nullptr;
    }
    Actor* a = s.add(std::make_unique<Actor3D>("Subject"));
    a->transform().position = {0, 0, 0};
    a->addComponent(ComponentRegistry::get().create(cls));
    return a;
}

int main() {
    auto& sys = script::ScriptSystem::get();

    // Task 45: `transform.position = <vector expr>` must move the actor.
    {
        Scene s("t45");
        Actor* a = attach(s, sys, R"(class Bug45 : Actor
{
    float move_speed = 3;
    func update(float delta)
    {
        Vector3 p = transform.position + Vector3(0, 1, 0);
        transform.position = p;
    }
})", "Bug45");
        CHECK(a != nullptr);
        if (a) {
            s.startPlay();
            for (int i = 0; i < 5; ++i)
                s.tick(0.1f);
            auto* sc = dynamic_cast<script::ScriptComponent*>(a->components()[0].get());
            CHECK(sc->error().empty());
            CHECK(a->transform().position.y == 5.0f); // +1 per frame, 5 frames
        }
    }

    // Task 46: `transform.position.y += 1` must not crash and must move the actor.
    {
        Scene s("t46");
        Actor* a = attach(s, sys, R"(class Bug46 : Actor
{
    func update(float delta)
    {
        transform.position.y += 1;
    }
})", "Bug46");
        CHECK(a != nullptr);
        if (a) {
            s.startPlay();
            for (int i = 0; i < 5; ++i)
                s.tick(0.1f);
            auto* sc = dynamic_cast<script::ScriptComponent*>(a->components()[0].get());
            CHECK(sc->error().empty());
            CHECK(a->transform().position.y == 5.0f);
        }
    }

    if (fails == 0)
        std::printf("ok  script bug regressions (tasks 45, 46)\n");
    return fails ? 1 : 0;
}
