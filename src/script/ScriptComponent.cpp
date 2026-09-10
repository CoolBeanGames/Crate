#include "script/ScriptComponent.h"

#include "core/Log.h"
#include "scene/Actor.h"

#include "imgui.h"

namespace crate::script {

void ScriptComponent::ensureObject() {
    if (!obj_ && cls_) {
        obj_ = Interpreter::instantiate(ctx_, cls_, actor());
    }
    if (obj_)
        obj_->owner = actor();
}

std::shared_ptr<ScriptObject> ScriptComponent::object() {
    ensureObject();
    return obj_;
}

void ScriptComponent::start() {
    ensureObject();
    if (!obj_)
        return;
    try {
        Interpreter(ctx_, obj_).call("start");
        started_ = true;
        lastError_.clear();
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        CR_ERROR("script", std::string(typeName()) + ".start: " + ex.what());
    }
}

void ScriptComponent::update(float dt) {
    ensureObject();
    if (!obj_ || !lastError_.empty())
        return;
    try {
        if (!started_) {
            Interpreter(ctx_, obj_).call("start");
            started_ = true;
        }
        Interpreter(ctx_, obj_).call("update", {Value::Float(dt)});
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        CR_ERROR("script", std::string(typeName()) + ".update: " + ex.what());
    }
}

void ScriptComponent::physicsUpdate(float dt) {
    ensureObject();
    if (!obj_ || !lastError_.empty())
        return;
    try {
        Interpreter(ctx_, obj_).call("physics_update", {Value::Float(dt)});
    } catch (const std::exception& ex) {
        lastError_ = ex.what();
        CR_ERROR("script", std::string(typeName()) + ".physics_update: " + ex.what());
    }
}

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
