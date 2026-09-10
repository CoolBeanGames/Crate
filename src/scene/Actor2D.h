#pragma once
#include "scene/Actor.h"

namespace crate {

// Base for screen- and world-space 2D content: sprites and UI controls.
class Actor2D : public Actor {
public:
    explicit Actor2D(std::string name = "Actor2D") : Actor(std::move(name)) {}
    const char* typeName() const override { return "ACTOR2D"; }

protected:
    Actor* cloneSelf() const override { return new Actor2D(name_); }
};

// A 2D image drawn in the world or on a canvas.
class SpriteActor : public Actor2D {
public:
    explicit SpriteActor(std::string name = "Sprite") : Actor2D(std::move(name)) {}
    const char* typeName() const override { return "SPRITE"; }

    std::string texturePath;
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};

protected:
    Actor* cloneSelf() const override {
        auto* s = new SpriteActor(name_);
        s->texturePath = texturePath;
        for (int i = 0; i < 4; ++i)
            s->tint[i] = tint[i];
        return s;
    }
};

// A UI control (button, panel, label). UI is just actors on a canvas.
class UIControlActor : public Actor2D {
public:
    explicit UIControlActor(std::string name = "UIControl") : Actor2D(std::move(name)) {}
    const char* typeName() const override { return "UI"; }

    std::string label = "Button";
    float size[2] = {160.0f, 40.0f};

protected:
    Actor* cloneSelf() const override {
        auto* u = new UIControlActor(name_);
        u->label = label;
        u->size[0] = size[0];
        u->size[1] = size[1];
        return u;
    }
};

} // namespace crate
