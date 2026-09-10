#include "scene/BuiltinComponents.h"
#include "core/Log.h"

#include "imgui.h"

namespace crate {

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
