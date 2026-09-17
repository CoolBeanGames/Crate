#include "script/ScriptComponent.h"

#include "core/Log.h"
#include "scene/Actor.h"
#include "script/ScriptFieldIO.h"

#include "imgui.h"

#include <cstring>
#include <functional>
#include <vector>

namespace crate::script {

namespace {
crate::Actor* findActorById(crate::Actor& node, uint64_t id) {
    if (node.id() == id)
        return &node;
    for (const auto& c : node.children())
        if (crate::Actor* hit = findActorById(*c, id))
            return hit;
    return nullptr;
}

void collectActors(crate::Actor& node, const std::function<bool(crate::Actor&)>& test,
                   std::vector<crate::Actor*>& out) {
    if (test(node))
        out.push_back(&node);
    for (const auto& c : node.children())
        collectActors(*c, test, out);
}

// Value types with dedicated widgets below (and "" for `var`) -- anything
// else declared as a field's type (Actor/Actor2D/Actor3D, a native component
// like Camera or Fog, or another script class) is a reference the Inspector
// should let you point at something in the scene instead of just printing
// whatever str() says about it.
bool isPlainValueType(const std::string& t) {
    return t.empty() || t == "int" || t == "float" || t == "bool" || t == "char" ||
          t == "string" || t == "array" || t == "Vector2" || t == "Vector3";
}
} // namespace

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

void ScriptComponent::writeFields(std::ostream& out, const std::function<int(const Actor*)>& idOf) const {
    const_cast<ScriptComponent*>(this)->ensureObject();
    if (!obj_)
        return;
    for (const auto& [name, val] : obj_->fields)
        writeValueField(out, name, val, idOf);
}

void ScriptComponent::readField(const std::string& key, const std::string& kind, const std::string& value,
                                const std::function<Actor*(int)>& actorById) {
    ensureObject();
    if (!obj_)
        return;
    obj_->fields[key] = readValueField(kind, value, actorById, ctx_);
}

// Picker + drag-drop target for a field declared as an actor/component type.
// `declType` is "Actor"/"Actor2D"/"Actor3D" for a plain actor reference, or
// any other type name (a native component like "Camera"/"Fog", or another
// script class) resolved through ctx_->getComponent -- the same lookup
// get_component(type_of(X)) and Camera.main already use, so this works for
// any component type without needing to special-case each one here.
void ScriptComponent::drawRefField(const std::string& label, const std::string& declType,
                                   Value& val) {
    const bool isActorType = declType == "Actor" || declType == "Actor2D" || declType == "Actor3D" ||
                             declType == "Transform";
    crate::Actor* root = sceneRootOf(actor());

    std::string current = "(none)";
    if (isActorType && val.t == Value::T::Actor && val.actor)
        current = val.actor->name();
    else if (!isActorType && val.t == Value::T::Object && val.obj)
        current = (val.obj->owner ? val.obj->owner->name() : std::string("?")) + " (" + declType + ")";

    ImGui::PushID(label.c_str());
    ImGui::SetNextItemWidth(-70);
    ImGui::Button(current.c_str());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CRATE_ACTOR")) {
            uint64_t id;
            std::memcpy(&id, p->Data, sizeof(id));
            if (crate::Actor* picked = root ? findActorById(*root, id) : nullptr) {
                if (isActorType)
                    val = Value::ActorRef(picked);
                else if (ctx_->getComponent) {
                    if (auto so = ctx_->getComponent(picked, declType))
                        val = Value::Obj(so);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Pick"))
        ImGui::OpenPopup("pick_ref");
    if (ImGui::BeginPopup("pick_ref")) {
        std::vector<crate::Actor*> candidates;
        if (root) {
            collectActors(*root,
                         [&](crate::Actor& a) {
                             if (isActorType)
                                 return true;
                             return ctx_->getComponent && ctx_->getComponent(&a, declType) != nullptr;
                         },
                         candidates);
        }
        if (ImGui::Selectable("(none)"))
            val = Value::Null_();
        for (crate::Actor* c : candidates) {
            if (ImGui::Selectable(c->name().c_str())) {
                if (isActorType)
                    val = Value::ActorRef(c);
                else if (ctx_->getComponent) {
                    if (auto so = ctx_->getComponent(c, declType))
                        val = Value::Obj(so);
                }
            }
        }
        if (candidates.empty())
            ImGui::TextDisabled(isActorType ? "(no actors in the scene)"
                                            : "(no actor has this component)");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopID();
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
            std::string declType;
            if (cls_->decl)
                for (const auto& fd : cls_->decl->fields)
                    if (fd.name == name) {
                        declType = fd.type;
                        break;
                    }
            if (!isPlainValueType(declType)) {
                drawRefField(name, declType, val);
            } else if (val.t == Value::T::Int) {
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
