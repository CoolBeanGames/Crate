#pragma once
#include "scene/Actor.h"
#include "scene/Component.h"

namespace crate {

// A tiny demonstration component: spins its actor about an axis while playing.
// Proves the Start/Update lifecycle and shows up in the Add Component menu.
class SpinnerComponent : public Component {
public:
    const char* typeName() const override { return "Spinner"; }

    void start() override;
    void update(float dt) override;
    void drawInspector() override;

    std::unique_ptr<Component> clone() const override {
        return std::make_unique<SpinnerComponent>(*this);
    }

    float degreesPerSecond = 90.0f;
    int axis = 1; // 0=X 1=Y 2=Z
};

} // namespace crate
