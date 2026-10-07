#include "ui/theme.h"

#include <windows.h>

#include <cstdio>
#include <cstring>

namespace theme {
namespace {

const struct Accent kAccents[kAccentCount] = {
    { "Blue",    ImVec4(0.298f, 0.553f, 1.000f, 1.f) },   // #4C8DFF
    { "Violet",  ImVec4(0.545f, 0.361f, 0.965f, 1.f) },   // #8B5CF6
    { "Teal",    ImVec4(0.078f, 0.722f, 0.651f, 1.f) },   // #14B8A6
    { "Green",   ImVec4(0.133f, 0.773f, 0.369f, 1.f) },   // #22C55E
    { "Amber",   ImVec4(0.961f, 0.620f, 0.043f, 1.f) },   // #F59E0B
    { "Rose",    ImVec4(0.957f, 0.247f, 0.369f, 1.f) },   // #F43F5E
    { "Cyan",    ImVec4(0.024f, 0.714f, 0.831f, 1.f) },   // #06B6D4
    { "Silver",  ImVec4(0.800f, 0.820f, 0.860f, 1.f) },
};

int g_accent = 0;

bool FileExists(const char* path)
{
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

const struct Accent& AccentAt(int index)
{
    if (index < 0 || index >= kAccentCount) index = 0;
    return kAccents[index];
}

void SetAccent(int index) { g_accent = (index >= 0 && index < kAccentCount) ? index : 0; }

ImVec4 Accent() { return kAccents[g_accent].color; }

ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImVec4 AccentHover() { return Mix(Accent(), ImVec4(1, 1, 1, 1), 0.14f); }
ImVec4 AccentActive() { return Mix(Accent(), ImVec4(0, 0, 0, 1), 0.18f); }

ImVec4 AccentSoft(float alpha)
{
    ImVec4 c = Accent();
    c.w = alpha;
    return c;
}

ImU32 U32(const ImVec4& c, float alphaMul)
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul));
}

void ApplySettingsStyle(float dpiScale)
{
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark(&s);

    s.WindowPadding = ImVec2(0, 0);
    s.FramePadding = ImVec2(10, 6);
    s.CellPadding = ImVec2(8, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.IndentSpacing = 18;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 12;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.PopupBorderSize = 1;
    s.FrameBorderSize = 0;
    s.WindowRounding = 0;
    s.ChildRounding = 10;
    s.FrameRounding = 7;
    s.PopupRounding = 8;
    s.ScrollbarRounding = 8;
    s.GrabRounding = 7;
    s.TabRounding = 7;
    s.SeparatorTextBorderSize = 1;
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    s.SelectableTextAlign = ImVec2(0.f, 0.5f);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kTextFaint;
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.110f, 0.125f, 0.157f, 0.98f);
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = kFrameHover;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_TitleBg] = kSurface;
    c[ImGuiCol_TitleBgActive] = kSurface;
    c[ImGuiCol_TitleBgCollapsed] = kSurface;
    c[ImGuiCol_MenuBarBg] = kSurface;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kFrameHover;
    c[ImGuiCol_ScrollbarGrabHovered] = kFrameActive;
    c[ImGuiCol_ScrollbarGrabActive] = kTextFaint;
    c[ImGuiCol_CheckMark] = Accent();
    c[ImGuiCol_SliderGrab] = Accent();
    c[ImGuiCol_SliderGrabActive] = AccentHover();
    c[ImGuiCol_Button] = kFrame;
    c[ImGuiCol_ButtonHovered] = kFrameHover;
    c[ImGuiCol_ButtonActive] = kFrameActive;
    c[ImGuiCol_Header] = AccentSoft(0.18f);
    c[ImGuiCol_HeaderHovered] = kFrameHover;
    c[ImGuiCol_HeaderActive] = AccentSoft(0.28f);
    c[ImGuiCol_Separator] = kBorder;
    c[ImGuiCol_SeparatorHovered] = kBorder;
    c[ImGuiCol_SeparatorActive] = Accent();
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = kFrame;
    c[ImGuiCol_TabHovered] = kFrameHover;
    c[ImGuiCol_TabSelected] = AccentSoft(0.30f);
    c[ImGuiCol_PlotLines] = Accent();
    c[ImGuiCol_PlotHistogram] = Accent();
    c[ImGuiCol_TextSelectedBg] = AccentSoft(0.35f);
    c[ImGuiCol_NavCursor] = Accent();
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

    s.ScaleAllSizes(dpiScale);
    s.FontScaleDpi = dpiScale;
}

Fonts LoadFonts(const char* language, float baseSize)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    Fonts f;

    char fontsDir[MAX_PATH] = {};
    GetWindowsDirectoryA(fontsDir, MAX_PATH);
    strcat_s(fontsDir, "\\Fonts\\");
    auto path = [&](const char* file) {
        static char buf[MAX_PATH * 2];
        snprintf(buf, sizeof(buf), "%s%s", fontsDir, file);
        return buf;
    };

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;

    if (FileExists(path("segoeui.ttf")))
        f.body = io.Fonts->AddFontFromFileTTF(path("segoeui.ttf"), baseSize, &cfg);
    if (!f.body) {
        ImFontConfig def;
        def.SizePixels = baseSize;
        f.body = io.Fonts->AddFontDefault(&def);
    }

    // Icons merged into the body font, nudged down to sit on the text baseline.
    {
        const char* iconFile = FileExists(path("SegoeIcons.ttf")) ? "SegoeIcons.ttf"
                             : FileExists(path("segmdl2.ttf"))    ? "segmdl2.ttf" : nullptr;
        if (iconFile) {
            ImFontConfig ic;
            ic.MergeMode = true;
            ic.GlyphOffset = ImVec2(0, baseSize * 0.12f);
            ic.GlyphMinAdvanceX = baseSize * 1.1f;
            static const ImWchar iconRanges[] = { 0xE700, 0xF8FF, 0 };
            f.hasIcons = io.Fonts->AddFontFromFileTTF(path(iconFile), baseSize * 0.92f, &ic, iconRanges) != nullptr;
        }
    }

    // CJK fallbacks only for the languages that need them; the files are large.
    const char* cjk = nullptr;
    if (language && _strnicmp(language, "zh", 2) == 0) cjk = FileExists(path("msyh.ttc")) ? "msyh.ttc" : "simhei.ttf";
    else if (language && _strnicmp(language, "ja", 2) == 0) cjk = FileExists(path("YuGothM.ttc")) ? "YuGothM.ttc" : "msgothic.ttc";
    else if (language && _strnicmp(language, "ko", 2) == 0) cjk = "malgun.ttf";
    if (cjk && FileExists(path(cjk))) {
        ImFontConfig cc;
        cc.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(path(cjk), baseSize, &cc);
    }

    if (FileExists(path("seguisb.ttf")))
        f.strong = io.Fonts->AddFontFromFileTTF(path("seguisb.ttf"), baseSize, &cfg);
    if (f.strong && cjk && FileExists(path(cjk))) {
        // Translated headings use the strong font too.
        ImFontConfig cc;
        cc.MergeMode = true;
        io.Fonts->AddFontFromFileTTF(path(cjk), baseSize, &cc);
    }
    if (!f.strong) f.strong = f.body;

    io.FontDefault = f.body;
    return f;
}

} // namespace theme
