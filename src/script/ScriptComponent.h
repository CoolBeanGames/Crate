#pragma once
#include "scene/Component.h"
#include "script/ClassInfo.h"
#include "script/Interpreter.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

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

    void writeFields(std::ostream& out, const std::function<int(const Actor*)>& idOf) const override;
    void readField(const std::string& key, const std::string& kind, const std::string& value,
                   const std::function<Actor*(int)>& actorById) override;

private:
    void ensureObject();
    void runHook(const char* hook, bool passDt, float dt);
    // A field declared as an actor/component type (see isPlainValueType in
    // the .cpp): drawn as a picker button + drag-drop target for a hierarchy
    // row, instead of the generic read-only str() label other fields get.
    void drawRefField(const std::string& label, const std::string& declType, Value& val);
    // A field declared `Scene` (see isPlainValueType in the .cpp): an asset-
    // reference, not a hierarchy one -- drag a .cscene tile from the Asset
    // Browser onto it, storing its path as a plain string. Value.instantiate()
    // (Runtime.h) reads that path back at runtime.
    void drawSceneRefField(const std::string& label, Value& val);

    ScriptContext* ctx_ = nullptr;
    const ClassInfo* cls_ = nullptr;
    std::shared_ptr<ScriptObject> obj_;
    uint32_t objGen_ = 0; // ClassInfo generation obj_ was built from
    // Snapshot of obj_->fields taken right after the most recent
    // Interpreter::instantiate(), i.e. each field's pure initialiser-produced
    // default with no Inspector edits applied. Compared against the live
    // value in ensureObject() on the next recompile to tell an untouched
    // field (safe to re-initialise to the new default) from one the user
    // has explicitly overridden (carried forward instead of being reset).
    std::unordered_map<std::string, Value> fieldDefaults_;
    bool started_ = false;
    std::string lastError_;
};

} // namespace crate::script
