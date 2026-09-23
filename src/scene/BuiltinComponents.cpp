#include "scene/BuiltinComponents.h"
#include "core/Log.h"
#include "scene/FieldCodec.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>

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
        // Sized independently of the actor's Transform.scale (task 84) so a
        // later physics collider can read the same numbers without also
        // picking up whatever the transform's scale is used for elsewhere.
        if (primitive == "Sphere") {
            ImGui::DragFloat("Radius", &radius, 0.05f, 0.001f, 1000.0f);
        } else if (primitive == "Cylinder" || primitive == "Capsule") {
            ImGui::DragFloat("Radius", &radius, 0.05f, 0.001f, 1000.0f);
            ImGui::DragFloat("Height", &height, 0.05f, 0.001f, 1000.0f);
        } else if (primitive == "Plane" || primitive == "Quad") {
            ImGui::DragFloat2("Size", planeSize, 0.05f, 0.001f, 1000.0f);
            if (primitive == "Plane") {
                ImGui::DragInt("Subdivisions", &planeSubdivisions, 1, 1, 64);
                planeSubdivisions = std::max(1, planeSubdivisions);
            }
        } else { // Cube
            ImGui::DragFloat3("Size", boxSize, 0.05f, 0.001f, 1000.0f);
        }
    }
    // Model / Texture / Material are edited by the editor as asset-picker
    // fields (the component can't reach the asset libraries).
    ImGui::ColorEdit4("Tint", tint);
    ImGui::Checkbox("Cast Shadows", &castShadows);
    ImGui::Checkbox("Receive Shadows", &receiveShadows);
}

void MeshRenderer::writeFields(std::ostream& out, const std::function<int(const Actor*)>&) const {
    writeFieldBool(out, "usePrimitive", usePrimitive);
    writeFieldString(out, "primitive", primitive);
    writeFieldString(out, "meshPath", meshPath);
    writeFieldString(out, "materialRef", materialRef);
    writeFieldString(out, "texturePath", texturePath);
    writeFieldVec3(out, "tint", tint[0], tint[1], tint[2]);
    writeFieldFloat(out, "tintA", tint[3]);
    writeFieldBool(out, "castShadows", castShadows);
    writeFieldBool(out, "receiveShadows", receiveShadows);
    writeFieldVec3(out, "boxSize", boxSize[0], boxSize[1], boxSize[2]);
    writeFieldFloat(out, "radius", radius);
    writeFieldFloat(out, "height", height);
    writeFieldVec2(out, "planeSize", planeSize[0], planeSize[1]);
    writeFieldInt(out, "planeSubdivisions", planeSubdivisions);
}
void MeshRenderer::readField(const std::string& key, const std::string&, const std::string& value,
                             const std::function<Actor*(int)>&) {
    if (key == "usePrimitive") usePrimitive = fieldB(value);
    else if (key == "primitive") primitive = value;
    else if (key == "meshPath") meshPath = value;
    else if (key == "materialRef") materialRef = value;
    else if (key == "texturePath") texturePath = value;
    else if (key == "tint") { float v[3]; parseFieldVec3(value, v); tint[0]=v[0]; tint[1]=v[1]; tint[2]=v[2]; }
    else if (key == "tintA") tint[3] = (float)fieldF(value);
    else if (key == "castShadows") castShadows = fieldB(value);
    else if (key == "receiveShadows") receiveShadows = fieldB(value);
    else if (key == "boxSize") { float v[3]; parseFieldVec3(value, v); boxSize[0]=v[0]; boxSize[1]=v[1]; boxSize[2]=v[2]; }
    else if (key == "radius") radius = (float)fieldF(value);
    else if (key == "height") height = (float)fieldF(value);
    else if (key == "planeSize") { float v[2]; parseFieldVec2(value, v); planeSize[0]=v[0]; planeSize[1]=v[1]; }
    else if (key == "planeSubdivisions") planeSubdivisions = std::max(1, (int)fieldI(value));
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
    ImGui::Checkbox("Static", &isStatic);
    if (isStatic)
        ImGui::TextDisabled("Static: only affects meshes/probes via Bake Lightmaps");
}

void LightComponent::writeFields(std::ostream& out, const std::function<int(const Actor*)>&) const {
    writeFieldInt(out, "type", (long long)type);
    writeFieldVec3(out, "color", color[0], color[1], color[2]);
    writeFieldFloat(out, "intensity", intensity);
    writeFieldFloat(out, "range", range);
    writeFieldFloat(out, "spotInnerDeg", spotInnerDeg);
    writeFieldFloat(out, "spotOuterDeg", spotOuterDeg);
    writeFieldBool(out, "isStatic", isStatic);
}
void LightComponent::readField(const std::string& key, const std::string&, const std::string& value,
                               const std::function<Actor*(int)>&) {
    if (key == "type") type = (Type)fieldI(value);
    else if (key == "color") { float v[3]; parseFieldVec3(value, v); color[0]=v[0]; color[1]=v[1]; color[2]=v[2]; }
    else if (key == "intensity") intensity = (float)fieldF(value);
    else if (key == "range") range = (float)fieldF(value);
    else if (key == "spotInnerDeg") spotInnerDeg = (float)fieldF(value);
    else if (key == "spotOuterDeg") spotOuterDeg = (float)fieldF(value);
    else if (key == "isStatic") isStatic = fieldB(value);
}

void FogComponent::drawInspector() {
    ImGui::ColorEdit3("Color", color);
    ImGui::DragFloat("Start", &start, 0.1f, 0.0f, 1000.0f);
    if (end < start)
        end = start + 0.01f;
    ImGui::DragFloat("End", &end, 0.1f, 0.01f, 2000.0f);
    ImGui::DragFloat("Height Range", &heightRange, 0.1f, 0.0f, 500.0f);
    ImGui::TextDisabled(
        "Height Range: fades out this many units above this actor's own height\n"
        "(0 = disabled, pure distance fog)");
    ImGui::TextDisabled("Only the first Fog found in the scene is used.");
}

void FogComponent::writeFields(std::ostream& out, const std::function<int(const Actor*)>&) const {
    writeFieldVec3(out, "color", color[0], color[1], color[2]);
    writeFieldFloat(out, "start", start);
    writeFieldFloat(out, "end", end);
    writeFieldFloat(out, "heightRange", heightRange);
}
void FogComponent::readField(const std::string& key, const std::string&, const std::string& value,
                             const std::function<Actor*(int)>&) {
    if (key == "color") { float v[3]; parseFieldVec3(value, v); color[0]=v[0]; color[1]=v[1]; color[2]=v[2]; }
    else if (key == "start") start = (float)fieldF(value);
    else if (key == "end") end = (float)fieldF(value);
    else if (key == "heightRange") heightRange = (float)fieldF(value);
}

void VolumetricFogComponent::drawInspector() {
    ImGui::ColorEdit3("Color", color);
    ImGui::DragFloat("Density", &density, 0.01f, 0.0f, 1.0f);
    ImGui::TextDisabled("Applies to the whole world; this actor's transform is unused.");
    ImGui::TextDisabled("Only the first Volumetric Fog found in the scene is used.");
}

void VolumetricFogComponent::writeFields(std::ostream& out, const std::function<int(const Actor*)>&) const {
    writeFieldVec3(out, "color", color[0], color[1], color[2]);
    writeFieldFloat(out, "density", density);
}
void VolumetricFogComponent::readField(const std::string& key, const std::string&, const std::string& value,
                                       const std::function<Actor*(int)>&) {
    if (key == "color") { float v[3]; parseFieldVec3(value, v); color[0]=v[0]; color[1]=v[1]; color[2]=v[2]; }
    else if (key == "density") density = (float)fieldF(value);
}

void CameraComponent::drawInspector() {
    ImGui::DragFloat("Field of View", &fovY, 0.5f, 1.0f, 179.0f);
    ImGui::DragFloat("Near", &nearZ, 0.01f, 0.001f, 1000.0f);
    if (farZ < nearZ)
        farZ = nearZ + 0.01f;
    ImGui::DragFloat("Far", &farZ, 1.0f, 0.01f, 100000.0f);
    ImGui::TextDisabled(enabled ? "Active: Game View renders from this camera."
                                : "Inactive: right-click this component to make it active.");
    ImGui::TextDisabled("Only one Camera in the scene is ever active at a time.");
}

void CameraComponent::writeFields(std::ostream& out, const std::function<int(const Actor*)>&) const {
    writeFieldFloat(out, "fovY", fovY);
    writeFieldFloat(out, "nearZ", nearZ);
    writeFieldFloat(out, "farZ", farZ);
}
void CameraComponent::readField(const std::string& key, const std::string&, const std::string& value,
                                const std::function<Actor*(int)>&) {
    if (key == "fovY") fovY = (float)fieldF(value);
    else if (key == "nearZ") nearZ = (float)fieldF(value);
    else if (key == "farZ") farZ = (float)fieldF(value);
}

void LightProbeComponent::drawInspector() {
    ImGui::TextDisabled(bakedValid ? "baked" : "not baked yet - Object > Bake Lightmaps");
    ImGui::ColorButton("##baked", ImVec4(bakedLight[0], bakedLight[1], bakedLight[2], 1.0f));
}

void LightProbeComponent::writeFields(std::ostream& out, const std::function<int(const Actor*)>&) const {
    writeFieldVec3(out, "bakedLight", bakedLight[0], bakedLight[1], bakedLight[2]);
    writeFieldBool(out, "bakedValid", bakedValid);
}
void LightProbeComponent::readField(const std::string& key, const std::string&, const std::string& value,
                                    const std::function<Actor*(int)>&) {
    if (key == "bakedLight") { float v[3]; parseFieldVec3(value, v); bakedLight[0]=v[0]; bakedLight[1]=v[1]; bakedLight[2]=v[2]; }
    else if (key == "bakedValid") bakedValid = fieldB(value);
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

void SpinnerComponent::writeFields(std::ostream& out, const std::function<int(const Actor*)>&) const {
    writeFieldFloat(out, "degreesPerSecond", degreesPerSecond);
    writeFieldInt(out, "axis", axis);
}
void SpinnerComponent::readField(const std::string& key, const std::string&, const std::string& value,
                                 const std::function<Actor*(int)>&) {
    if (key == "degreesPerSecond") degreesPerSecond = (float)fieldF(value);
    else if (key == "axis") axis = (int)fieldI(value);
}

} // namespace crate
