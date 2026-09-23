#pragma once

// Single source of truth for the engine's version string (task 136's title
// bar, and anywhere else that ever needs to display it). Bump this alongside
// each tagged GitHub release.
namespace crate {
constexpr const char* kCrateVersion = "0.11.0";
} // namespace crate
