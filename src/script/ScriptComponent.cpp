#include "script/ScriptComponent.h"

#include "core/Log.h"
#include "scene/Actor.h"

#include "imgui.h"

namespace crate::script {

void ScriptComponent::ensureObject() {
    // The class was recompiled since we built the object -> rebuild it so field
    // initialisers re-run and stale do_async state is dropped.
    if (obj_ && cls_ && cls_->generation != objGen_) {
        obj_.reset();
        started_ = false;
        lastError_.clear();
    }
    if (!obj_ && cls_) {
        obj_ = Interpreter::instantiate(ctx_, cls_, actor());
        objGen_ = cls_->generation;
    }
    if (obj_)
        obj_->owner = actor();
}

std::shared_ptr<ScriptObject> ScriptComponent::object() {
    ensureObject();
    return obj_;
}

// Run one lifecycle hook, trapping every failure so a bad script can never
// crash the editor -- the error is recorded and the component goes quiet.
void ScriptComponent::runHook(const char* hook, bool passDt, float dt) {
    if (!lastError_.empty())
        return;
    try {
        ensureObject();
        if (!obj_)
            return;
        std::vector<Value> args;
        if (passDt)
            args.push_back(Value::Float(dt));
        Interpreter(ctx_, obj_).call(hook, std::move(args));
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        CR_ERROR("script", std::string(typeName()) + "." + hook + ": " + ex.what());
    } catch (...) {
        lastError_ = "unknown error";
        CR_ERROR("script", std::string(typeName()) + "." + hook + ": unknown error");
    }
}

void ScriptComponent::start() {
    runHook("start", false, 0.0f);
    if (lastError_.empty())
        started_ = true;
}

void ScriptComponent::update(float dt) {
    if (!started_ && lastError_.empty()) {
        runHook("start", false, 0.0f);
        if (lastError_.empty())
            started_ = true;
    }
    runHook("update", true, dt);
}

void ScriptComponent::physicsUpdate(float dt) { runHook("physics_update", true, dt); }

void ScriptComponent::drawInspector() {
    if (!cls_) {
        ImGui::TextDisabled("(script type missing)");
        return;
    }
    ImGui::TextDisabled("script  |  base: %s", cls_->base.c_str());
    if (!lastError_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImColor(0xC0, 0x32, 0x26).Value);
        ImGui::TextWrapped("error: %s", lastError_.c_str());
        ImGui::PopStyleColor();
        if (ImGui::SmallButton("Reset")) {
            lastError_.clear();
            obj_.reset();
            started_ = false;
        }
    }
    ensureObject();
    if (obj_) {
        for (auto& [name, val] : obj_->fields) {
            if (val.t == Value::T::Int) {
                int v = (int)val.i;
                if (ImGui::DragInt(name.c_str(), &v))
                    val.i = v;
            } else if (val.t == Value::T::Float) {
                float v = (float)val.f;
                if (ImGui::DragFloat(name.c_str(), &v))
                    val.f = v;
            } else if (val.t == Value::T::Bool) {
                if (ImGui::Checkbox(name.c_str(), &val.b)) {
                }
            } else {
                ImGui::LabelText(name.c_str(), "%s", val.str().c_str());
            }
        }
    }
}

} // namespace crate::script
