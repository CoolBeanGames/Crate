#pragma once
#include <array>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace crate {

// A basic, always-on profiler (Zen task 90). Named "sections" (compute FPS,
// render, physics, scripts) each keep a rolling history of the last
// kHistoryLen frames' durations for a small sparkline, plus the most recent
// value. Per-component timing (physics/script update/physicsUpdate calls)
// additionally aggregates two ways -- by owning actor and by component type
// -- from the SAME samples, so "specific scripts" and "specific objects"
// breakdowns come from one instrumentation point (Actor::updateComponents/
// physicsUpdateComponents) instead of scattering timers through every
// component type.
//
// Not thread-safe -- everything here runs on the main thread, matching the
// rest of the engine.
class Profiler {
public:
    static constexpr int kHistoryLen = 120; // ~2 seconds at 60fps

    static Profiler& get();

    // Call once at the very start of a frame, before anything timed.
    void beginFrame();

    // Records one sample (milliseconds) for a top-level named section (e.g.
    // "render", "physics", "scripts"). Sections are created on first use.
    void addSample(const char* section, double milliseconds);

    // Records one component's update/physicsUpdate time, aggregated by both
    // the owning actor's name and the component's typeName().
    void addComponentSample(const std::string& actorName, const std::string& componentType,
                            double milliseconds);

    struct SectionView {
        double lastMs = 0.0;
        double avgMs = 0.0; // over the recorded history
    };
    // nullptr if `section` was never sampled.
    const SectionView* section(const std::string& name) const;
    std::vector<std::string> sectionNames() const;
    // Copies up to kHistoryLen samples, oldest-first, into `out` (caller-
    // sized to kHistoryLen) -- ready for e.g. ImGui::PlotLines. Returns how
    // many were actually written (0 if `name` was never sampled).
    int historyOrdered(const std::string& name, float* out) const;

    struct Breakdown {
        std::string name;
        double ms = 0.0; // this frame's total
    };
    // Highest-time-first. Rebuilt fresh each beginFrame() from that frame's
    // addComponentSample() calls -- a snapshot of "this frame", not a
    // rolling average (per-object/per-script time is spiky enough that a
    // smoothed average would hide exactly what you're looking for).
    std::vector<Breakdown> byActor() const;
    std::vector<Breakdown> byComponentType() const;

private:
    Profiler() = default;

    struct Section {
        std::array<float, kHistoryLen> history{};
        int count = 0; // how many of `history` are real samples (< kHistoryLen until it wraps)
        int next = 0;  // ring buffer write cursor
        double lastMs = 0.0;
    };
    std::unordered_map<std::string, Section> sections_;
    std::vector<std::string> sectionOrder_; // insertion order, for stable UI ordering

    std::unordered_map<std::string, double> actorMsThisFrame_;
    std::unordered_map<std::string, double> componentMsThisFrame_;
};

// RAII scope timer: `PROFILE_SCOPE("physics");` records elapsed wall-clock
// time into that section when the enclosing scope ends.
class ProfileScope {
public:
    explicit ProfileScope(const char* section)
        : section_(section), start_(std::chrono::steady_clock::now()) {}
    ~ProfileScope() {
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start_).count();
        Profiler::get().addSample(section_, ms);
    }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    const char* section_;
    std::chrono::steady_clock::time_point start_;
};

} // namespace crate
