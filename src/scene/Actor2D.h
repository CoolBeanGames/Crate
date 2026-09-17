#pragma once
#include "scene/Actor.h"
#include "scene/FieldCodec.h"

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

    void writeFields(std::ostream& out) const override {
        writeFieldString(out, "texturePath", texturePath);
        writeFieldVec3(out, "tint", tint[0], tint[1], tint[2]);
        writeFieldFloat(out, "tintA", tint[3]);
    }
    void readField(const std::string& key, const std::string&, const std::string& value) override {
        if (key == "texturePath") texturePath = value;
        else if (key == "tint") { float v[3]; parseFieldVec3(value, v); tint[0]=v[0]; tint[1]=v[1]; tint[2]=v[2]; }
        else if (key == "tintA") tint[3] = (float)fieldF(value);
    }

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

    void writeFields(std::ostream& out) const override {
        writeFieldString(out, "label", label);
        writeFieldVec2(out, "size", size[0], size[1]);
    }
    void readField(const std::string& key, const std::string&, const std::string& value) override {
        if (key == "label") label = value;
        else if (key == "size") { float v[2]; parseFieldVec2(value, v); size[0]=v[0]; size[1]=v[1]; }
    }

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
