#include "editor/Theme.h"
#include "imgui.h"

namespace crate {

static ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

void ApplyZenTheme() {
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding = 10.0f;
    s.ChildRounding = 8.0f;
    s.FrameRounding = 8.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 8.0f;
    s.TabRounding = 8.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.WindowTitleAlign = ImVec2(0.0f, 0.5f);

    ImVec4* c = s.Colors;
    const ImVec4 workspace = rgb(0x0B, 0x0D, 0x12);
    const ImVec4 panel     = rgb(0x12, 0x15, 0x1C);
    const ImVec4 panel2    = rgb(0x16, 0x1A, 0x22);
    const ImVec4 raised    = rgb(0x1D, 0x22, 0x2C);
    const ImVec4 border    = rgb(0x26, 0x2C, 0x38);
    const ImVec4 text      = rgb(0xF4, 0xF6, 0xFA);
    const ImVec4 textDim   = rgb(0x8E, 0x93, 0xA3);
    const ImVec4 violet    = rgb(0x8B, 0x7C, 0xFF);
    const ImVec4 violetDim = rgb(0x8B, 0x7C, 0xFF, 0.35f);

    c[ImGuiCol_Text]                 = text;
    c[ImGuiCol_TextDisabled]         = textDim;
    c[ImGuiCol_WindowBg]             = workspace;
    c[ImGuiCol_ChildBg]              = panel;
    c[ImGuiCol_PopupBg]              = panel2;
    c[ImGuiCol_Border]               = border;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = panel2;
    c[ImGuiCol_FrameBgHovered]       = raised;
    c[ImGuiCol_FrameBgActive]        = raised;
    c[ImGuiCol_TitleBg]              = panel;
    c[ImGuiCol_TitleBgActive]        = panel;
    c[ImGuiCol_TitleBgCollapsed]     = panel;
    c[ImGuiCol_MenuBarBg]            = panel;
    c[ImGuiCol_ScrollbarBg]          = workspace;
    c[ImGuiCol_ScrollbarGrab]        = raised;
    c[ImGuiCol_ScrollbarGrabHovered] = border;
    c[ImGuiCol_ScrollbarGrabActive]  = violet;
    c[ImGuiCol_CheckMark]            = violet;
    c[ImGuiCol_SliderGrab]           = violet;
    c[ImGuiCol_SliderGrabActive]     = violet;
    c[ImGuiCol_Button]               = raised;
    c[ImGuiCol_ButtonHovered]        = violetDim;
    c[ImGuiCol_ButtonActive]         = violet;
    c[ImGuiCol_Header]               = violetDim;
    c[ImGuiCol_HeaderHovered]        = violetDim;
    c[ImGuiCol_HeaderActive]         = violet;
    c[ImGuiCol_Separator]            = border;
    c[ImGuiCol_SeparatorHovered]     = violet;
    c[ImGuiCol_SeparatorActive]      = violet;
    c[ImGuiCol_ResizeGrip]           = raised;
    c[ImGuiCol_ResizeGripHovered]    = violetDim;
    c[ImGuiCol_ResizeGripActive]     = violet;
    c[ImGuiCol_Tab]                  = panel;
    c[ImGuiCol_TabHovered]           = violetDim;
    c[ImGuiCol_TabSelected]          = raised;
    c[ImGuiCol_TabSelectedOverline]  = violet;
    c[ImGuiCol_TabDimmed]            = panel;
    c[ImGuiCol_TabDimmedSelected]    = panel2;
    c[ImGuiCol_DockingPreview]       = violetDim;
    c[ImGuiCol_DockingEmptyBg]       = workspace;
    c[ImGuiCol_TextSelectedBg]       = violetDim;
    c[ImGuiCol_NavHighlight]         = violet;
    c[ImGuiCol_TableHeaderBg]        = panel2;
    c[ImGuiCol_TableBorderStrong]    = border;
    c[ImGuiCol_TableBorderLight]     = border;
}

} // namespace crate
