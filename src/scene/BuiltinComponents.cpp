#include "scene/BuiltinComponents.h"
#include "core/Log.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

namespace crate {

void MeshRenderer::drawInspector() {
    int mode = usePrimitive ? 0 : 1;
    ImGui::RadioButton("Primitive", &mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Model", &mode, 1);
    usePrimitive = (mode == 0);

    if (usePrimitive) {
        const char* prims[] = {"Cube", "Sphere", "Cylinder", "Capsule", "Plane", "Quad"};
        if (ImGui::BeginCombo("Shape", primitive.c_str())) {
            for (const char* p : prims)
                if (ImGui::Selectable(p, primitive == p))
                    primitive = p;
            ImGui::EndCombo();
        }
    }
    // Model / Texture / Material are edited by the editor as asset-picker
    // fields (the component can't reach the asset libraries).
    ImGui::ColorEdit4("Tint", tint);
    ImGui::Checkbox("Cast Shadows", &castShadows);
    ImGui::Checkbox("Receive Shadows", &receiveShadows);
}

void LightComponent::drawInspector() {
    const char* kinds[] = {"Directional", "Point", "Spot"};
    int k = static_cast<int>(type);
    if (ImGui::Combo("Type", &k, kinds, 3))
        type = static_cast<Type>(k);
    ImGui::ColorEdit3("Color", color);
    ImGui::DragFloat("Intensity", &intensity, 0.05f, 0.0f, 50.0f);
    if (type != Type::Directional)
        ImGui::DragFloat("Range", &range, 0.1f, 0.01f, 500.0f);
    if (type == Type::Spot) {
        ImGui::DragFloat("Spot inner", &spotInnerDeg, 0.5f, 0.0f, 89.0f);
        ImGui::DragFloat("Spot outer", &spotOuterDeg, 0.5f, 0.0f, 89.0f);
        if (spotOuterDeg < spotInnerDeg)
            spotOuterDeg = spotInnerDeg;
    }
    ImGui::TextDisabled("+Z (the normal arrow) is the light's forward direction");
}

void SpinnerComponent::start() {
    CR_GAME("spinner", std::string("Spinner started on '") + actor()->name() + "'");
}

void SpinnerComponent::update(float dt) {
    float d = degreesPerSecond * dt;
    Vec3& r = actor()->transform().rotationEuler;
    (&r.x)[axis < 0 || axis > 2 ? 1 : axis] += d;
}

void SpinnerComponent::drawInspector() {
    ImGui::DragFloat("Deg / sec", &degreesPerSecond, 1.0f);
    const char* axes[] = {"X", "Y", "Z"};
    ImGui::Combo("Axis", &axis, axes, 3);
}

} // namespace crate
