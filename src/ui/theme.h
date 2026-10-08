// Visual identity: the dark palette, accent colors, ImGui style and fonts.
#pragma once

#include "imgui.h"

namespace theme {

// ── Palette ──
inline const ImVec4 kBg          = ImVec4(0.059f, 0.067f, 0.082f, 1.f);   // #0F1115
inline const ImVec4 kSurface     = ImVec4(0.082f, 0.094f, 0.118f, 1.f);   // #15181E  sidebar, header
inline const ImVec4 kCard        = ImVec4(0.102f, 0.118f, 0.149f, 1.f);   // #1A1E26
inline const ImVec4 kCardHover   = ImVec4(0.129f, 0.149f, 0.192f, 1.f);   // #212631
inline const ImVec4 kFrame       = ImVec4(0.137f, 0.157f, 0.200f, 1.f);   // #232833
inline const ImVec4 kFrameHover  = ImVec4(0.169f, 0.192f, 0.251f, 1.f);   // #2B3140
inline const ImVec4 kFrameActive = ImVec4(0.196f, 0.224f, 0.286f, 1.f);   // #323949
inline const ImVec4 kBorder      = ImVec4(0.165f, 0.184f, 0.227f, 1.f);   // #2A2F3A
inline const ImVec4 kText        = ImVec4(0.910f, 0.918f, 0.941f, 1.f);   // #E8EAF0
inline const ImVec4 kTextDim     = ImVec4(0.604f, 0.631f, 0.698f, 1.f);   // #9AA1B2
inline const ImVec4 kTextFaint   = ImVec4(0.392f, 0.420f, 0.486f, 1.f);   // #646B7C
inline const ImVec4 kGood        = ImVec4(0.239f, 0.863f, 0.518f, 1.f);   // #3DDC84
inline const ImVec4 kWarn        = ImVec4(1.000f, 0.765f, 0.302f, 1.f);   // #FFC34D
inline const ImVec4 kBad         = ImVec4(1.000f, 0.361f, 0.361f, 1.f);   // #FF5C5C
inline const ImVec4 kKofi        = ImVec4(1.000f, 0.369f, 0.357f, 1.f);   // #FF5E5B  Ko-fi's own red

struct Accent { const char* name; ImVec4 color; };
inline constexpr int kAccentCount = 8;
const Accent& AccentAt(int index);

ImVec4 Accent();                         // current accent
ImVec4 AccentHover();
ImVec4 AccentActive();
ImVec4 AccentSoft(float alpha = 0.16f);  // translucent accent for selections
void   SetAccent(int index);

ImU32  U32(const ImVec4& c, float alphaMul = 1.f);
ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t);

// Applies the settings-window style, scaled for the window's DPI.
void ApplySettingsStyle(float dpiScale);

// ── Fonts ──
struct Fonts {
    ImFont* body = nullptr;         // Segoe UI + icon glyphs merged
    ImFont* strong = nullptr;       // Segoe UI Semibold (headings, HUD numbers)
    bool    hasIcons = false;
};

// Loads fonts into the current context's atlas. `language` picks CJK fallbacks.
Fonts LoadFonts(const char* language, float baseSize);

// Icon code points (Segoe Fluent Icons / Segoe MDL2 Assets), UTF-8 encoded.
namespace icon {
inline constexpr const char* Display   = "\xEE\x9F\xB4";   // U+E7F4
inline constexpr const char* Palette   = "\xEE\x9E\x90";   // U+E790
inline constexpr const char* Sensors   = "\xEE\xA5\x90";   // U+E950
inline constexpr const char* Keyboard  = "\xEE\x9D\xA5";   // U+E765
inline constexpr const char* Settings  = "\xEE\x9C\x93";   // U+E713
inline constexpr const char* Info      = "\xEE\xA5\x86";   // U+E946
inline constexpr const char* Globe     = "\xEE\x9D\xB4";   // U+E774
inline constexpr const char* Check     = "\xEE\x9C\xBE";   // U+E73E
inline constexpr const char* ChevronDn = "\xEE\x9C\x8D";   // U+E70D
inline constexpr const char* Warning   = "\xEE\x9E\xBA";   // U+E7BA
inline constexpr const char* Eye       = "\xEE\x9E\xB3";   // U+E7B3
inline constexpr const char* Hide      = "\xEE\xB4\x9A";   // U+ED1A
inline constexpr const char* Power     = "\xEE\x9F\xA8";   // U+E7E8
inline constexpr const char* Refresh   = "\xEE\x9C\xAC";   // U+E72C
inline constexpr const char* Pulse     = "\xEE\xA7\x99";   // U+E9D9
inline constexpr const char* Close     = "\xEE\xA2\xBB";   // U+E8BB
inline constexpr const char* Download  = "\xEE\xA2\x96";   // U+E896
inline constexpr const char* Heart     = "\xEE\xAD\x92";   // U+EB52 HeartFill
}

} // namespace theme
