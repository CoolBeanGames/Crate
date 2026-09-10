#pragma once
#include "scene/Component.h"
#include "script/ClassInfo.h"
#include "script/Interpreter.h"

#include <cstdint>
#include <memory>
#include <string>

namespace crate::script {

// A component backed by a cScript class. One is created per actor the script is
// added to; it forwards the Start / Update / PhysicsUpdate lifecycle into the
// interpreter.
class ScriptComponent : public Component {
public:
    ScriptComponent(ScriptContext* ctx, const ClassInfo* cls) : ctx_(ctx), cls_(cls) {}

    const char* typeName() const override { return cls_ ? cls_->name.c_str() : "Script"; }
    const ClassInfo* classInfo() const { return cls_; }
    const std::string& error() const { return lastError_; }
    std::shared_ptr<ScriptObject> object();

    void start() override;
    void update(float dt) override;
    void physicsUpdate(float dt) override;
    void drawInspector() override;

    std::unique_ptr<Component> clone() const override {
        return std::make_unique<ScriptComponent>(ctx_, cls_);
    }

private:
    void ensureObject();
    void runHook(const char* hook, bool passDt, float dt);

    ScriptContext* ctx_ = nullptr;
    const ClassInfo* cls_ = nullptr;
    std::shared_ptr<ScriptObject> obj_;
    uint32_t objGen_ = 0; // ClassInfo generation obj_ was built from
    bool started_ = false;
    std::string lastError_;
};

} // namespace crate::script
