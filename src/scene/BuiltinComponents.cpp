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
    } else {
        ImGui::InputText("Model Path", &meshPath);
    }
    ImGui::InputText("Texture", &texturePath);
    ImGui::ColorEdit4("Tint", tint);
    ImGui::Checkbox("Cast Shadows", &castShadows);
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
