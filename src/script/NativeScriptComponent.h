#pragma once
#include "scene/Component.h"
#include "script/ClassInfo.h"
#include "script/CompiledClassInfo.h"
#include "script/Interpreter.h"
#include "script/NativeModule.h"

#include <memory>
#include <string>

namespace crate::script {

// A component backed by a NATIVELY-COMPILED cScript class -- the
// Play-mode counterpart to ScriptComponent (which stays exactly as-is and
// keeps handling every script while Play is stopped; see
// transpiration.txt, "Transplation" Phase 5).
//
// Owns BOTH representations of the same script instance at once:
//   - `native_`: the actual compiled crate::Component this class's hooks
//     (start/update/physicsUpdate) really execute on -- created lazily on
//     first use (Component::actor() isn't valid until AFTER this object is
//     constructed and added to an Actor, exactly like ScriptComponent's own
//     ensureObject() defers to actor() rather than taking an owner in its
//     constructor).
//   - `obj_`: an ordinary INTERPRETED ScriptObject (via the same
//     Interpreter::instantiate() ScriptComponent already uses), which
//     exists PURELY so drawInspector() -- copied byte-for-byte from
//     ScriptComponent::drawInspector() below, including its private
//     helpers -- keeps working completely unmodified, whether Play is
//     running or not.
// Every hook call pushes any edits made to obj_->fields (e.g. through the
// Inspector while Play is running) into the native instance via the
// CompiledClassInfo field-accessor table BEFORE running, then pulls the
// native instance's current values back into obj_->fields AFTER running,
// so the Inspector always reflects what the native code just did. This is
// the reflection table's whole reason for existing (see CompiledClassInfo.h).
class NativeScriptComponent : public Component {
public:
    NativeScriptComponent(ScriptContext* ctx, const ClassInfo* cls, NativeClassExport exp)
        : ctx_(ctx), cls_(cls), exp_(std::move(exp)) {}
    ~NativeScriptComponent() override;

    const char* typeName() const override {
        return cls_ ? cls_->name.c_str() : exp_.className.c_str();
    }
    const ClassInfo* classInfo() const { return cls_; }
    // The interpreted mirror -- same accessor shape as
    // ScriptComponent::object(), for symmetry with existing callers/tests.
    std::shared_ptr<ScriptObject> object();

    void start() override;
    void update(float dt) override;
    void physicsUpdate(float dt) override;
    void drawInspector() override;

    std::unique_ptr<Component> clone() const override {
        return std::make_unique<NativeScriptComponent>(ctx_, cls_, exp_);
    }

private:
    void ensureNative(); // lazily CreateInstance_<Class>(ctx_, actor())
    void ensureObject();  // lazily Interpreter::instantiate (mirrors ScriptComponent)
    void pushFieldsToNative(); // obj_->fields (incl. overflow) -> native_
    void pullFieldsFromNative(); // native_ -> obj_->fields (incl. overflow)
    // Byte-for-byte copy of ScriptComponent::drawRefField -- see .cpp.
    void drawRefField(const std::string& label, const std::string& declType, Value& val);

    ScriptContext* ctx_ = nullptr;
    const ClassInfo* cls_ = nullptr; // interpreted ClassInfo; Inspector declared-type lookups only
    NativeClassExport exp_;
    crate::Component* native_ = nullptr; // owned; destroyed via exp_.destroy, never `delete`
    const CompiledClassInfo* compiledInfo_ = nullptr; // cached once native_ exists
    std::shared_ptr<ScriptObject> obj_; // interpreted mirror, for Inspector display/edit only
    uint32_t objGen_ = 0;
    bool started_ = false;
};

} // namespace crate::script
