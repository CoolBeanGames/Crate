#include "script/NativeScriptComponent.h"

#include "core/Log.h"
#include "scene/Actor.h"
#include "script/ScriptFieldIO.h"

#include "imgui.h"

#include <cstring>
#include <functional>
#include <vector>

namespace crate::script {

// ---------------------------------------------------------------------------
// The following anonymous-namespace helpers, drawRefField(), and
// drawInspector() are a DELIBERATE byte-for-byte copy of
// ScriptComponent.cpp's own (see that file) -- per transpiration.txt Phase
// 5's explicit instruction to keep the Inspector experience identical
// whether a script is running interpreted or compiled. They operate purely
// on obj_/cls_/ctx_, exactly as ScriptComponent's versions do, and are
// duplicated rather than shared to keep the two components fully
// independent (ScriptComponent is never touched by this work) -- a future
// cleanup could factor them into a common free-function helper if desired,
// but that is not required for behavior parity and is not done here.
// ---------------------------------------------------------------------------

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

bool isPlainValueType(const std::string& t) {
    return t.empty() || t == "int" || t == "float" || t == "bool" || t == "char" ||
          t == "string" || t == "array" || t == "Vector2" || t == "Vector3";
}
} // namespace

// ---------------------------------------------------------------------------
// Native/interpreted dual-representation plumbing (new for Phase 5).
// ---------------------------------------------------------------------------

NativeScriptComponent::~NativeScriptComponent() {
    if (native_ && exp_.destroy)
        exp_.destroy(native_);
}

void NativeScriptComponent::ensureNative() {
    if (native_ || !exp_.create)
        return;
    native_ = exp_.create(ctx_, actor());
    compiledInfo_ = exp_.classInfo ? exp_.classInfo() : nullptr;
}

void NativeScriptComponent::ensureObject() {
    // Same recompile-detection contract as ScriptComponent::ensureObject():
    // if the INTERPRETED class was recompiled, rebuild the interpreted
    // mirror so its declared-type lookups (drawInspector) and default
    // values stay current. Does not affect `native_`, which only changes
    // when the whole namespace's DLL is rebuilt and reloaded (Phase 7).
    if (obj_ && cls_ && cls_->generation != objGen_) {
        obj_.reset();
        started_ = false;
    }
    if (!obj_ && cls_) {
        obj_ = Interpreter::instantiate(ctx_, cls_, actor());
        objGen_ = cls_->generation;
    }
    if (obj_)
        obj_->owner = actor();
}

std::shared_ptr<ScriptObject> NativeScriptComponent::object() {
    ensureObject();
    return obj_;
}

void NativeScriptComponent::pushFieldsToNative() {
    if (!native_ || !compiledInfo_ || !obj_)
        return;
    for (size_t i = 0; i < compiledInfo_->fieldCount; ++i) {
        const auto& f = compiledInfo_->fields[i];
        auto it = obj_->fields.find(f.name);
        if (it != obj_->fields.end())
            f.set(native_, it->second);
    }
    // Undeclared (auto-vivified) fields -> the native instance's own
    // overflow map, mirroring Interpreter::lvalue()'s implicit field
    // creation on the interpreted side.
    if (compiledInfo_->overflow) {
        auto& ovf = compiledInfo_->overflow(native_);
        for (auto& [name, val] : obj_->fields) {
            bool declared = false;
            for (size_t i = 0; i < compiledInfo_->fieldCount; ++i)
                if (compiledInfo_->fields[i].name == name) {
                    declared = true;
                    break;
                }
            if (!declared)
                ovf[name] = val;
        }
    }
}

void NativeScriptComponent::pullFieldsFromNative() {
    if (!native_ || !compiledInfo_ || !obj_)
        return;
    for (size_t i = 0; i < compiledInfo_->fieldCount; ++i) {
        const auto& f = compiledInfo_->fields[i];
        obj_->fields[f.name] = f.get(native_);
    }
    if (compiledInfo_->overflow) {
        auto& ovf = compiledInfo_->overflow(native_);
        for (auto& [name, val] : ovf)
            obj_->fields[name] = val;
    }
}

void NativeScriptComponent::start() {
    ensureNative();
    ensureObject();
    if (!native_)
        return;
    pushFieldsToNative();
    native_->start();
    pullFieldsFromNative();
    started_ = true;
}

void NativeScriptComponent::update(float dt) {
    ensureNative();
    ensureObject();
    if (!native_)
        return;
    if (!started_) {
        pushFieldsToNative();
        native_->start();
        pullFieldsFromNative();
        started_ = true;
    }
    pushFieldsToNative();
    native_->update(dt);
    pullFieldsFromNative();
}

void NativeScriptComponent::physicsUpdate(float dt) {
    ensureNative();
    ensureObject();
    if (!native_)
        return;
    pushFieldsToNative();
    native_->physicsUpdate(dt);
    pullFieldsFromNative();
}

void NativeScriptComponent::writeFields(std::ostream& out, const std::function<int(const Actor*)>& idOf) const {
    const_cast<NativeScriptComponent*>(this)->ensureObject();
    if (!obj_)
        return;
    for (const auto& [name, val] : obj_->fields)
        writeValueField(out, name, val, idOf);
}

void NativeScriptComponent::readField(const std::string& key, const std::string& kind,
                                      const std::string& value,
                                      const std::function<Actor*(int)>& actorById) {
    ensureObject();
    if (!obj_)
        return;
    obj_->fields[key] = readValueField(kind, value, actorById, ctx_);
}

// ---------------------------------------------------------------------------
// drawRefField / drawInspector -- copied from ScriptComponent.cpp, see the
// note at the top of this file.
// ---------------------------------------------------------------------------

void NativeScriptComponent::drawRefField(const std::string& label, const std::string& declType,
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
            // See the identical fix in ScriptComponent::drawRefField(): actors
            // commonly share a name, and an unguarded Selectable(name) ID
            // collides across candidates with the same name.
            ImGui::PushID(c);
            if (ImGui::Selectable(c->name().c_str())) {
                if (isActorType)
                    val = Value::ActorRef(c);
                else if (ctx_->getComponent) {
                    if (auto so = ctx_->getComponent(c, declType))
                        val = Value::Obj(so);
                }
            }
            ImGui::PopID();
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

void NativeScriptComponent::drawSceneRefField(const std::string& label, Value& val) {
    std::string current = (val.t == Value::T::String && !val.s.empty()) ? val.s : std::string("(none)");
    ImGui::PushID(label.c_str());
    ImGui::SetNextItemWidth(-70);
    ImGui::Button(current.c_str());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("CRATE_SCENE_PATH"))
            val = Value::Str(std::string(static_cast<const char*>(p->Data)));
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(label.c_str());
    ImGui::PopID();
}

void NativeScriptComponent::drawInspector() {
    if (!cls_) {
        ImGui::TextDisabled("(script type missing)");
        return;
    }
    ImGui::TextDisabled("script (native)  |  base: %s", cls_->base.c_str());
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
            if (declType == "Scene") {
                drawSceneRefField(name, val);
            } else if (!isPlainValueType(declType)) {
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
