#pragma once

namespace crate {

// Applies the Zen design language to ImGui: near-black workspace, dark layered
// panels, quiet borders, warm-white text, and violet as the primary accent.
void ApplyZenTheme();

// Accent colours reused by individual widgets (ImU32, packed as ImGui expects).
namespace zen {
constexpr unsigned int kViolet = 0xFFFF7C8B; // #8B7CFF  (ABGR)
constexpr unsigned int kBug    = 0xFF2632C0; // dark red
constexpr unsigned int kAmber  = 0xFF2CA8E0; // locks / due dates
constexpr unsigned int kText   = 0xFFFAF6F4; // #F4F6FA
constexpr unsigned int kTextDim = 0xFF9A93A0;
} // namespace zen

} // namespace crate
