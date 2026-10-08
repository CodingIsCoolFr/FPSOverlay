#include "ui/widgets.h"
#include "ui/theme.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace ui {
namespace {

bool    g_rtl = false;
ImFont* g_body = nullptr;
ImFont* g_strong = nullptr;
bool    g_icons = false;

struct ComboRowState { float rowBottom = 0; float startX = 0; } g_comboRow;

float Anim(ImGuiID id, float target, float speed = 16.f)
{
    float* t = ImGui::GetStateStorage()->GetFloatRef(id, target);
    const float dt = ImGui::GetIO().DeltaTime;
    *t += (target - *t) * std::min(1.f, dt * speed);
    if (std::fabs(*t - target) < 0.002f) *t = target;
    return *t;
}

// Right-to-left strings arrive already shaped into visual order (the whole paragraph reversed
// onto one line). Wrapping them front-to-back like ImGui does would put the end of the sentence
// on the first line, so lines are filled from the end of the visual string instead.
void WrapRtl(ImFont* font, float size, const char* text, float maxW, std::vector<std::string>& lines)
{
    lines.clear();
    const std::string all(text, ImGui::FindRenderedTextEnd(text));
    size_t pstart = 0;
    for (;;) {
        const size_t nl = all.find('\n', pstart);
        const std::string para = all.substr(pstart, nl == std::string::npos ? std::string::npos : nl - pstart);
        std::vector<std::string> words;
        size_t w0 = 0;
        while (w0 <= para.size()) {
            const size_t sp = para.find(' ', w0);
            const std::string word = para.substr(w0, sp == std::string::npos ? std::string::npos : sp - w0);
            if (!word.empty()) words.push_back(word);
            if (sp == std::string::npos) break;
            w0 = sp + 1;
        }
        int end = (int)words.size() - 1;
        if (end < 0) lines.emplace_back();
        while (end >= 0) {
            std::string line = words[end];
            int i = end - 1;
            while (i >= 0) {
                const std::string candidate = words[i] + " " + line;
                if (font->CalcTextSizeA(size, FLT_MAX, 0.f, candidate.c_str()).x > maxW) break;
                line = candidate;
                --i;
            }
            lines.push_back(line);
            end = i;
        }
        if (nl == std::string::npos) break;
        pstart = nl + 1;
    }
}

ImVec2 TextSize(ImFont* font, float size, const char* text, float wrap = -1.f)
{
    if (wrap > 0 && g_rtl) {
        std::vector<std::string> lines;
        WrapRtl(font, size, text, wrap, lines);
        float w = 0;
        for (const auto& l : lines) w = std::max(w, font->CalcTextSizeA(size, FLT_MAX, 0.f, l.c_str()).x);
        const float lineH = font->CalcTextSizeA(size, FLT_MAX, 0.f, "A").y;
        return ImVec2(w, (float)lines.size() * lineH);
    }
    return font->CalcTextSizeA(size, FLT_MAX, wrap > 0 ? wrap : 0.f, text, ImGui::FindRenderedTextEnd(text));
}

void DrawText(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, const ImVec4& col, const char* text, float wrap = 0.f)
{
    if (wrap > 0 && g_rtl) {
        std::vector<std::string> lines;
        WrapRtl(font, size, text, wrap, lines);
        float blockW = 0;
        for (const auto& l : lines) blockW = std::max(blockW, font->CalcTextSizeA(size, FLT_MAX, 0.f, l.c_str()).x);
        const float lineH = font->CalcTextSizeA(size, FLT_MAX, 0.f, "A").y;
        float y = pos.y;
        for (const auto& l : lines) {
            const float lw = font->CalcTextSizeA(size, FLT_MAX, 0.f, l.c_str()).x;
            dl->AddText(font, size, ImVec2(std::floor(pos.x + blockW - lw), std::floor(y)), theme::U32(col), l.c_str());
            y += lineH;
        }
        return;
    }
    dl->AddText(font, size, ImVec2(std::floor(pos.x), std::floor(pos.y)), theme::U32(col), text,
                ImGui::FindRenderedTextEnd(text), wrap);
}

// Start/end x for content of width w inside [x0, x0 + avail].
float StartX(float x0, float avail, float w) { return g_rtl ? x0 + avail - w : x0; }
float EndX(float x0, float avail, float w) { return g_rtl ? x0 : x0 + avail - w; }

void DrawSwitch(ImDrawList* dl, ImVec2 pos, float w, float h, float t, bool hovered, bool disabled)
{
    const ImVec4 off = hovered ? theme::kFrameActive : theme::kFrameHover;
    ImVec4 track = theme::Mix(off, theme::Accent(), t);
    if (disabled) track.w *= 0.45f;
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), theme::U32(track), h * 0.5f);
    const float r = h * 0.5f - h * 0.14f;
    const float travel = w - h;
    const float knobT = g_rtl ? 1.f - t : t;
    const ImVec2 c(pos.x + h * 0.5f + travel * knobT, pos.y + h * 0.5f);
    ImVec4 knob = theme::Mix(theme::kTextDim, ImVec4(1, 1, 1, 1), t);
    if (disabled) knob.w *= 0.6f;
    dl->AddCircleFilled(c, r, theme::U32(knob), 24);
}

} // namespace

void SetRtl(bool rtl) { g_rtl = rtl; }
bool IsRtl() { return g_rtl; }

void SetFonts(ImFont* body, ImFont* strong, bool hasIcons)
{
    g_body = body;
    g_strong = strong ? strong : body;
    g_icons = hasIcons;
}

ImFont* StrongFont() { return g_strong ? g_strong : ImGui::GetFont(); }
bool HasIcons() { return g_icons; }

float Em(float v) { return v * ImGui::GetFontSize(); }

void Spacing(float lines) { ImGui::Dummy(ImVec2(0, Em(0.5f * lines))); }

void Tooltip(const char* text)
{
    if (!text || !text[0]) return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Em(0.6f), Em(0.45f)));
    if (ImGui::BeginTooltip()) {
        // The tooltip sizes itself to the text block, so no extra alignment is needed.
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize();
        const ImVec2 ts = TextSize(font, size, text, Em(22.f));
        DrawText(ImGui::GetWindowDrawList(), font, size, ImGui::GetCursorScreenPos(), theme::kText, text, Em(22.f));
        ImGui::Dummy(ts);
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar();
}

void Paragraph(const char* text, const ImVec4& color, float width, ImFont* font, float sizeScale)
{
    if (!text) return;
    if (!font) font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * sizeScale;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float w = (width > 0 && width < avail) ? width : avail;
    const ImVec2 ts = TextSize(font, size, text, w);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    // Right-to-left paragraphs hug the right edge of their column.
    const float x = g_rtl ? p.x + std::max(0.f, w - ts.x) : p.x;
    DrawText(ImGui::GetWindowDrawList(), font, size, ImVec2(x, p.y), color, text, w);
    ImGui::Dummy(ImVec2(std::max(ts.x, 1.f), ts.y));
}

// ── Containers ──────────────────────────────────────────────────────────────

void PageHeader(const char* title, const char* subtitle)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float avail = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float titleSize = em * 1.5f;
    const ImVec2 ts = TextSize(StrongFont(), titleSize, title);
    DrawText(dl, StrongFont(), titleSize, ImVec2(StartX(p.x, avail, ts.x), p.y), theme::kText, title);
    float h = ts.y;
    if (subtitle && subtitle[0]) {
        const ImVec2 ss = TextSize(ImGui::GetFont(), em, subtitle, avail);
        DrawText(dl, ImGui::GetFont(), em, ImVec2(StartX(p.x, avail, std::min(ss.x, avail)), p.y + h + em * 0.2f),
                 theme::kTextDim, subtitle, avail);
        h += em * 0.2f + ss.y;
    }
    ImGui::Dummy(ImVec2(avail, h + em * 0.9f));
}

void BeginCard(const char* id, const char* title, const char* subtitle)
{
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, em * 0.7f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.55f, em * 0.55f));
    ImGui::BeginChild(id, ImVec2(-FLT_MIN, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.5f, em * 0.15f));
    if (title && title[0]) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float avail = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float pad = em * 0.6f;
        const float size = em * 0.82f;
        const ImVec2 ts = TextSize(StrongFont(), size, title);
        DrawText(dl, StrongFont(), size, ImVec2(StartX(p.x + pad, avail - 2 * pad, ts.x), p.y + em * 0.25f),
                 theme::kTextDim, title);
        float h = em * 0.25f + ts.y;
        if (subtitle && subtitle[0]) {
            const ImVec2 ss = TextSize(ImGui::GetFont(), em * 0.86f, subtitle, avail - 2 * pad);
            DrawText(dl, ImGui::GetFont(), em * 0.86f,
                     ImVec2(StartX(p.x + pad, avail - 2 * pad, std::min(ss.x, avail - 2 * pad)), p.y + h + em * 0.1f),
                     theme::kTextFaint, subtitle, avail - 2 * pad);
            h += em * 0.1f + ss.y;
        }
        ImGui::Dummy(ImVec2(avail, h + em * 0.35f));
    }
}

void EndCard()
{
    ImGui::PopStyleVar();       // ItemSpacing
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.6f));
}

// ── Rows ────────────────────────────────────────────────────────────────────

bool ToggleSwitch(const char* id, bool* v, bool disabled)
{
    const float em = ImGui::GetFontSize();
    const float w = em * 2.3f, h = em * 1.25f;
    ImGui::PushID(id);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton("##switch", ImVec2(w, h)) && !disabled;
    if (pressed) *v = !*v;
    const float t = Anim(ImGui::GetID("##anim"), *v ? 1.f : 0.f);
    DrawSwitch(ImGui::GetWindowDrawList(), p, w, h, t, ImGui::IsItemHovered(), disabled);
    ImGui::PopID();
    return pressed;
}

bool ToggleRow(const char* label, bool* v, const char* hint, const char* badge, const ImVec4* badgeColor, bool disabled)
{
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    if (win->SkipItems) return false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImFont* font = ImGui::GetFont();
    const float em = ImGui::GetFontSize();
    const float padX = em * 0.6f, padY = em * 0.5f;
    const float swW = em * 2.3f, swH = em * 1.25f;
    const float width = ImGui::GetContentRegionAvail().x;
    const float textMax = width - padX * 2 - swW - em;
    const float hintSize = em * 0.86f;

    const ImVec2 ls = TextSize(font, em, label);
    float badgeW = 0;
    ImVec2 bs(0, 0);
    if (badge && badge[0]) {
        bs = TextSize(font, em * 0.78f, badge);
        badgeW = bs.x + em * 0.9f;
    }
    const ImVec2 hs = (hint && hint[0]) ? TextSize(font, hintSize, hint, textMax) : ImVec2(0, 0);
    const float textH = ls.y + (hs.y > 0 ? em * 0.12f + hs.y : 0);
    const float rowH = std::max(swH, textH) + padY * 2;

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool pressed = ImGui::InvisibleButton("##row", ImVec2(width, rowH)) && !disabled;
    const bool hovered = ImGui::IsItemHovered() && !disabled;
    if (pressed) *v = !*v;

    const float hoverT = Anim(ImGui::GetID("##hov"), hovered ? 1.f : 0.f, 20.f);
    if (hoverT > 0.01f)
        dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + rowH), theme::U32(theme::kCardHover, hoverT), em * 0.45f);

    const float swX = EndX(p0.x + padX, width - 2 * padX, swW);
    const float t = Anim(ImGui::GetID("##sw"), *v ? 1.f : 0.f);
    DrawSwitch(dl, ImVec2(swX, p0.y + (rowH - swH) * 0.5f), swW, swH, t, hovered, disabled);

    const float ty = p0.y + (rowH - textH) * 0.5f;
    const float lx = StartX(p0.x + padX, width - 2 * padX, ls.x);
    DrawText(dl, font, em, ImVec2(lx, ty), disabled ? theme::kTextFaint : theme::kText, label);
    if (badgeW > 0) {
        const ImVec4 bc = badgeColor ? *badgeColor : theme::kTextDim;
        const float bx = g_rtl ? lx - em * 0.5f - badgeW : lx + ls.x + em * 0.5f;
        const float by = ty + (ls.y - (bs.y + em * 0.2f)) * 0.5f;
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + badgeW, by + bs.y + em * 0.2f), theme::U32(bc, 0.16f), em);
        DrawText(dl, font, em * 0.78f, ImVec2(bx + em * 0.45f, by + em * 0.1f), bc, badge);
    }
    if (hs.y > 0) {
        const float hx = StartX(p0.x + padX, width - 2 * padX, std::min(hs.x, textMax));
        DrawText(dl, font, hintSize, ImVec2(hx, ty + ls.y + em * 0.12f), theme::kTextDim, hint, textMax);
    }
    ImGui::PopID();
    return pressed;
}

bool Segmented(const char* id, int* v, const char* const* items, int count, float width)
{
    if (count <= 0) return false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float h = em * 2.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float segW = width / count;
    const float inset = em * 0.18f;
    bool changed = false;

    ImGui::PushID(id);
    dl->AddRectFilled(p0, ImVec2(p0.x + width, p0.y + h), theme::U32(theme::kFrame), em * 0.5f);

    // Sliding highlight
    const float targetIdx = (float)(g_rtl ? count - 1 - *v : *v);
    const float animIdx = Anim(ImGui::GetID("##slide"), targetIdx, 18.f);
    const ImVec2 hl0(p0.x + animIdx * segW + inset, p0.y + inset);
    dl->AddRectFilled(hl0, ImVec2(hl0.x + segW - 2 * inset, p0.y + h - inset), theme::U32(theme::Accent()), em * 0.4f);

    for (int i = 0; i < count; ++i) {
        const int slot = g_rtl ? count - 1 - i : i;
        ImGui::SetCursorScreenPos(ImVec2(p0.x + slot * segW, p0.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h)) && *v != i) {
            *v = i;
            changed = true;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec2 ts = TextSize(ImGui::GetFont(), em, items[i]);
        const ImVec4 col = (*v == i) ? ImVec4(1, 1, 1, 1) : (hov ? theme::kText : theme::kTextDim);
        DrawText(dl, ImGui::GetFont(), em, ImVec2(p0.x + slot * segW + (segW - ts.x) * 0.5f, p0.y + (h - ts.y) * 0.5f),
                 col, items[i]);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p0);
    ImGui::Dummy(ImVec2(width, h));
    return changed;
}

static void RowLabel(const char* label, const char* hint, float width, float reservedEnd, float& outH)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float padX = em * 0.6f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float textMax = width - padX * 2 - reservedEnd;
    const ImVec2 ls = TextSize(ImGui::GetFont(), em, label);
    DrawText(dl, ImGui::GetFont(), em, ImVec2(StartX(p0.x + padX, width - 2 * padX, ls.x), p0.y), theme::kText, label);
    outH = ls.y;
    if (hint && hint[0]) {
        const ImVec2 hs = TextSize(ImGui::GetFont(), em * 0.86f, hint, textMax);
        DrawText(dl, ImGui::GetFont(), em * 0.86f,
                 ImVec2(StartX(p0.x + padX, width - 2 * padX, std::min(hs.x, textMax)), p0.y + ls.y + em * 0.12f),
                 theme::kTextDim, hint, textMax);
        outH += em * 0.12f + hs.y;
    }
}

bool SegmentedRow(const char* label, const char* hint, int* v, const char* const* items, int count)
{
    const float em = ImGui::GetFontSize();
    const float padX = em * 0.6f, padY = em * 0.5f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + padY));
    float labelH = 0;
    RowLabel(label, hint, width, 0, labelH);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + padX, p0.y + padY + labelH + em * 0.5f));
    const bool changed = Segmented(label, v, items, count, width - 2 * padX);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, ImGui::GetCursorScreenPos().y));
    ImGui::Dummy(ImVec2(width, padY));
    return changed;
}

bool SliderRow(const char* label, const char* hint, int* v, int min, int max, const char* valueFmt)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float padX = em * 0.6f, padY = em * 0.5f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();

    char value[48];
    snprintf(value, sizeof(value), valueFmt ? valueFmt : "%d", *v);
    const ImVec2 vs = TextSize(StrongFont(), em, value);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + padY));
    float labelH = 0;
    RowLabel(label, hint, width, vs.x + em, labelH);
    DrawText(dl, StrongFont(), em, ImVec2(EndX(p0.x + padX, width - 2 * padX, vs.x), p0.y + padY), theme::Accent(), value);

    const float trackY = p0.y + padY + labelH + em * 0.9f;
    const float x0 = p0.x + padX + em * 0.5f, x1 = p0.x + width - padX - em * 0.5f;
    const float hitH = em * 1.4f;
    ImGui::SetCursorScreenPos(ImVec2(p0.x + padX, trackY - hitH * 0.5f));
    ImGui::PushID(label);
    ImGui::InvisibleButton("##slider", ImVec2(width - 2 * padX, hitH));
    bool changed = false;
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();
    if (active && x1 > x0) {
        float f = (ImGui::GetIO().MousePos.x - x0) / (x1 - x0);
        f = std::clamp(f, 0.f, 1.f);
        if (g_rtl) f = 1.f - f;
        const int nv = min + (int)std::lround(f * (max - min));
        if (nv != *v) { *v = nv; changed = true; }
    }
    if (hovered || active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::PopID();

    float f = (max > min) ? (float)(*v - min) / (float)(max - min) : 0.f;
    if (g_rtl) f = 1.f - f;
    const float kx = x0 + (x1 - x0) * f;
    const float th = em * 0.28f;
    dl->AddRectFilled(ImVec2(x0, trackY - th * 0.5f), ImVec2(x1, trackY + th * 0.5f), theme::U32(theme::kFrameHover), th);
    if (g_rtl) dl->AddRectFilled(ImVec2(kx, trackY - th * 0.5f), ImVec2(x1, trackY + th * 0.5f), theme::U32(theme::Accent()), th);
    else       dl->AddRectFilled(ImVec2(x0, trackY - th * 0.5f), ImVec2(kx, trackY + th * 0.5f), theme::U32(theme::Accent()), th);
    const float kr = em * ((active || hovered) ? 0.55f : 0.48f);
    dl->AddCircleFilled(ImVec2(kx, trackY), kr, theme::U32(ImVec4(1, 1, 1, 1)), 24);
    dl->AddCircle(ImVec2(kx, trackY), kr, theme::U32(theme::Accent()), 24, em * 0.12f);

    ImGui::SetCursorScreenPos(ImVec2(p0.x, trackY + hitH * 0.5f));
    ImGui::Dummy(ImVec2(width, padY * 0.5f));
    return changed;
}

bool BeginComboRow(const char* label, const char* hint, const char* preview)
{
    const float em = ImGui::GetFontSize();
    const float padX = em * 0.6f, padY = em * 0.5f;
    const float width = ImGui::GetContentRegionAvail().x;
    const float comboW = std::min(width * 0.52f, em * 17.f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float frameH = ImGui::GetFrameHeight();

    // Measure the label block first to centre both sides on the same row.
    const ImVec2 ls = TextSize(ImGui::GetFont(), em, label);
    float labelH = ls.y;
    const float textMax = width - padX * 2 - comboW - em;
    if (hint && hint[0]) labelH += em * 0.12f + TextSize(ImGui::GetFont(), em * 0.86f, hint, textMax).y;
    const float rowH = std::max(frameH, labelH) + padY * 2;

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + (rowH - labelH) * 0.5f));
    float dummy = 0;
    RowLabel(label, hint, width, comboW + em, dummy);

    ImGui::SetCursorScreenPos(ImVec2(EndX(p0.x + padX, width - 2 * padX, comboW), p0.y + (rowH - frameH) * 0.5f));
    ImGui::SetNextItemWidth(comboW);
    // The ID comes from a hidden label, not PushID/PopID: an open combo makes its popup the
    // current window, so a PopID here would land on the popup's ID stack instead of ours.
    const std::string id = std::string("##combo_") + label;
    // Whole-pixel padding and spacing for the list. A card's fractional spacing makes the list's
    // content a fraction of a pixel taller than the window, which shows a needless scrollbar.
    // (The style stack is shared by all windows, so popping after EndCombo is fine.)
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::round(em * 0.4f), std::round(em * 0.4f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(std::round(em * 0.5f), 2.f));
    const bool open = ImGui::BeginCombo(id.c_str(), preview ? preview : "", ImGuiComboFlags_HeightLarge);
    g_comboRow.rowBottom = p0.y + rowH;
    g_comboRow.startX = p0.x;
    if (!open) {
        ImGui::PopStyleVar(2);
        ImGui::SetCursorScreenPos(ImVec2(p0.x, g_comboRow.rowBottom));
        ImGui::Dummy(ImVec2(width, 0));
    }
    return open;
}

void EndComboRow()
{
    ImGui::EndCombo();
    ImGui::PopStyleVar(2);
    ImGui::SetCursorScreenPos(ImVec2(g_comboRow.startX, g_comboRow.rowBottom));
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 0));
}

bool ComboItem(const char* label, bool selected, const char* detail)
{
    const float em = ImGui::GetFontSize();
    ImGui::PushID(label);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float iconW = em * 1.5f;
    const ImVec2 ls = TextSize(ImGui::GetFont(), em, label);
    const ImVec2 ds = detail && detail[0] ? TextSize(ImGui::GetFont(), em * 0.82f, detail) : ImVec2(0, 0);
    // Wide enough for the longest text: the list grows past the box instead of cutting words off.
    const float w = std::max({ ImGui::GetContentRegionAvail().x, em * 12.f, iconW + std::max(ls.x, ds.x) + em * 0.8f });
    const float h = std::round(em * (detail && detail[0] ? 2.4f : 1.8f));
    const bool pressed = ImGui::Selectable("##item", selected, 0, ImVec2(w, h));
    if (selected) ImGui::SetItemDefaultFocus();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected && g_icons)
        DrawText(dl, ImGui::GetFont(), em, ImVec2(g_rtl ? p.x + w - iconW : p.x + em * 0.2f, p.y + (h - em) * 0.5f - em * 0.1f),
                 theme::Accent(), theme::icon::Check);
    const float textX0 = g_rtl ? p.x : p.x + iconW;
    const float textW = w - iconW;
    float ty = p.y + (h - ls.y) * 0.5f;
    if (detail && detail[0]) ty = p.y + em * 0.2f;
    DrawText(dl, ImGui::GetFont(), em, ImVec2(StartX(textX0, textW, ls.x), ty), theme::kText, label);
    if (detail && detail[0]) {
        DrawText(dl, ImGui::GetFont(), em * 0.82f, ImVec2(StartX(textX0, textW, ds.x), ty + ls.y), theme::kTextDim, detail);
    }
    ImGui::PopID();
    return pressed;
}

bool ButtonRow(const char* label, const char* hint, const char* button, bool primary, bool disabled)
{
    const float em = ImGui::GetFontSize();
    const float padX = em * 0.6f, padY = em * 0.5f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 bs = TextSize(ImGui::GetFont(), em, button);
    const float btnW = bs.x + em * 2.f;
    const float frameH = em * 2.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();

    const ImVec2 ls = TextSize(ImGui::GetFont(), em, label);
    float labelH = ls.y;
    const float textMax = width - padX * 2 - btnW - em;
    if (hint && hint[0]) labelH += em * 0.12f + TextSize(ImGui::GetFont(), em * 0.86f, hint, textMax).y;
    const float rowH = std::max(frameH, labelH) + padY * 2;

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + (rowH - labelH) * 0.5f));
    float dummy = 0;
    RowLabel(label, hint, width, btnW + em, dummy);
    ImGui::SetCursorScreenPos(ImVec2(EndX(p0.x + padX, width - 2 * padX, btnW), p0.y + (rowH - frameH) * 0.5f));
    ImGui::PushID(label);
    const bool pressed = primary ? PrimaryButton(button, ImVec2(btnW, frameH), disabled)
                                 : SecondaryButton(button, ImVec2(btnW, frameH), disabled);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowH));
    ImGui::Dummy(ImVec2(width, 0));
    return pressed;
}

void InfoRow(const char* label, const char* value, const ImVec4* valueColor)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float padX = em * 0.6f, padY = em * 0.4f;
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 ls = TextSize(ImGui::GetFont(), em, label);
    const float valueMax = width - 2 * padX - ls.x - em;
    const ImVec2 vs = TextSize(ImGui::GetFont(), em, value ? value : "", valueMax);
    const float rowH = std::max(ls.y, vs.y) + padY * 2;
    DrawText(dl, ImGui::GetFont(), em, ImVec2(StartX(p0.x + padX, width - 2 * padX, ls.x), p0.y + padY), theme::kTextDim, label);
    DrawText(dl, ImGui::GetFont(), em, ImVec2(EndX(p0.x + padX, width - 2 * padX, std::min(vs.x, valueMax)), p0.y + padY),
             valueColor ? *valueColor : theme::kText, value ? value : "", valueMax);
    ImGui::Dummy(ImVec2(width, rowH));
}

// ── Buttons ─────────────────────────────────────────────────────────────────

static bool StyledButton(const char* label, const ImVec2& sizeArg, bool disabled, const ImVec4& bg, const ImVec4& bgHover,
                         const ImVec4& bgActive, const ImVec4& text)
{
    const float em = ImGui::GetFontSize();
    const ImVec2 ts = TextSize(ImGui::GetFont(), em, label);
    ImVec2 size = sizeArg;
    if (size.x <= 0) size.x = ts.x + em * 2.f;
    if (size.y <= 0) size.y = em * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    bool pressed = ImGui::InvisibleButton("##btn", size);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImGui::PopID();
    if (disabled) pressed = false;
    ImVec4 c = disabled ? theme::kFrame : (act ? bgActive : (hov ? bgHover : bg));
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), theme::U32(c), em * 0.45f);
    ImVec4 tc = disabled ? theme::kTextFaint : text;
    DrawText(ImGui::GetWindowDrawList(), ImGui::GetFont(), em,
             ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), tc, label);
    if (hov && !disabled) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

bool PrimaryButton(const char* label, const ImVec2& size, bool disabled)
{
    return StyledButton(label, size, disabled, theme::Accent(), theme::AccentHover(), theme::AccentActive(), ImVec4(1, 1, 1, 1));
}

bool SecondaryButton(const char* label, const ImVec2& size, bool disabled)
{
    return StyledButton(label, size, disabled, theme::kFrame, theme::kFrameHover, theme::kFrameActive, theme::kText);
}

bool SupportButton(const char* label, const ImVec2& sizeArg, bool soft)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const ImVec2 ts = TextSize(ImGui::GetFont(), em, label);
    const float iconW = g_icons ? em * 1.45f : 0.f;
    ImVec2 size = sizeArg;
    if (size.x <= 0) size.x = ts.x + iconW + em * 2.f;
    if (size.y <= 0) size.y = em * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##support", size);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    const float hovT = Anim(ImGui::GetID("##hov"), hov ? 1.f : 0.f, 20.f);
    ImGui::PopID();

    const ImVec4 white(1, 1, 1, 1);
    ImVec4 bg, text, heart;
    if (soft) {
        bg = theme::kKofi;
        bg.w = act ? 0.30f : 0.11f + 0.11f * hovT;
        text = theme::Mix(theme::kTextDim, theme::kText, hovT);
        heart = theme::kKofi;
    } else {
        bg = act ? theme::Mix(theme::kKofi, ImVec4(0, 0, 0, 1), 0.12f) : theme::Mix(theme::kKofi, white, 0.12f * hovT);
        text = heart = white;
    }
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), theme::U32(bg), em * 0.45f);

    // Heart then label, centred as one block; mirrored for right-to-left languages.
    const float x0 = p.x + std::max(em * 0.6f, (size.x - iconW - ts.x) * 0.5f);
    const float labelX = g_rtl ? x0 : x0 + iconW;
    if (iconW > 0) {
        const float hx = g_rtl ? x0 + ts.x + em * 0.45f : x0;
        DrawText(dl, ImGui::GetFont(), em, ImVec2(hx, p.y + (size.y - em) * 0.5f - em * 0.08f), heart, theme::icon::Heart);
    }
    DrawText(dl, ImGui::GetFont(), em, ImVec2(labelX, p.y + (size.y - ts.y) * 0.5f), text, label);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

bool SidebarItem(const char* icon, const char* label, bool selected)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = em * 2.4f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(label);
    const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    const float selT = Anim(ImGui::GetID("##sel"), selected ? 1.f : 0.f, 18.f);
    const float hovT = Anim(ImGui::GetID("##hov"), hov ? 1.f : 0.f, 20.f);
    ImGui::PopID();

    if (selT > 0.01f) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::U32(theme::AccentSoft(0.16f * selT)), em * 0.5f);
        const float barW = em * 0.22f, barH = h * 0.5f * selT;
        const float bx = g_rtl ? p.x + w - barW : p.x;
        dl->AddRectFilled(ImVec2(bx, p.y + (h - barH) * 0.5f), ImVec2(bx + barW, p.y + (h + barH) * 0.5f),
                          theme::U32(theme::Accent()), barW);
    } else if (hovT > 0.01f) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::U32(theme::kCardHover, hovT), em * 0.5f);
    }

    const ImVec4 tc = selected ? theme::kText : theme::Mix(theme::kTextDim, theme::kText, hovT);
    const float iconW = (g_icons && icon) ? em * 1.9f : 0.f;
    const ImVec2 ls = TextSize(ImGui::GetFont(), em, label);
    const float padX = em * 0.85f;
    if (iconW > 0) {
        const float ix = g_rtl ? p.x + w - padX - em * 1.1f : p.x + padX;
        DrawText(dl, ImGui::GetFont(), em * 1.05f, ImVec2(ix, p.y + (h - em) * 0.5f - em * 0.12f),
                 selected ? theme::Accent() : tc, icon);
    }
    const float lx = g_rtl ? p.x + w - padX - iconW - ls.x : p.x + padX + iconW;
    DrawText(dl, ImGui::GetFont(), em, ImVec2(lx, p.y + (h - ls.y) * 0.5f), tc, label);
    return pressed;
}

bool KeyCap(const char* id, const char* text, bool listening, float width)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float h = em * 2.0f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(id);
    const bool pressed = ImGui::InvisibleButton("##key", ImVec2(width, h));
    const bool hov = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec4 bg = listening ? theme::AccentSoft(0.18f) : (hov ? theme::kFrameHover : theme::kFrame);
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), theme::U32(bg), em * 0.45f);
    // Bottom edge like a physical key.
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), theme::U32(listening ? theme::Accent() : theme::kBorder), em * 0.45f, 0, em * 0.08f);
    float alpha = 1.f;
    if (listening) alpha = 0.55f + 0.45f * (0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.f));
    const ImVec4 tc = listening ? theme::Accent() : theme::kText;
    const ImVec2 ts = TextSize(StrongFont(), em, text);
    DrawText(dl, StrongFont(), em, ImVec2(p.x + (width - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
             ImVec4(tc.x, tc.y, tc.z, tc.w * alpha), text);
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return pressed;
}

void Badge(const char* text, const ImVec4& color)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float size = em * 0.8f;
    const ImVec2 ts = TextSize(ImGui::GetFont(), size, text);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 sz(ts.x + em * 1.0f, ts.y + em * 0.3f);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), theme::U32(color, 0.16f), sz.y * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + em * 0.42f, p.y + sz.y * 0.5f), em * 0.15f, theme::U32(color), 12);
    DrawText(dl, ImGui::GetFont(), size, ImVec2(p.x + em * 0.7f, p.y + em * 0.15f), color, text);
    ImGui::Dummy(ImVec2(sz.x + em * 0.2f, sz.y));
}

bool PositionPicker(const char* id, int* anchor, bool custom, float width)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float h = width * 9.f / 16.f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = em * 0.5f;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), theme::U32(theme::kFrame), em * 0.6f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), theme::U32(theme::kBorder), em * 0.6f, 0, em * 0.08f);
    // Stand under the miniature screen.
    dl->AddRectFilled(ImVec2(p.x + width * 0.42f, p.y + h), ImVec2(p.x + width * 0.58f, p.y + h + em * 0.35f),
                      theme::U32(theme::kBorder));

    const float bw = width * 0.18f, bh = h * 0.16f, inset = em * 0.6f;
    const ImVec2 slots[6] = {
        { p.x + inset, p.y + inset }, { p.x + (width - bw) * 0.5f, p.y + inset }, { p.x + width - inset - bw, p.y + inset },
        { p.x + inset, p.y + h - inset - bh }, { p.x + (width - bw) * 0.5f, p.y + h - inset - bh },
        { p.x + width - inset - bw, p.y + h - inset - bh },
    };
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < 6; ++i) {
        ImGui::SetCursorScreenPos(slots[i]);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##slot", ImVec2(bw, bh))) {
            *anchor = i;
            changed = true;
        }
        const bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        const bool sel = (*anchor == i) && !custom;
        ImVec4 c = sel ? theme::Accent() : (hov ? theme::kFrameActive : theme::kFrameHover);
        if (custom && *anchor == i) c = theme::AccentSoft(0.35f);
        dl->AddRectFilled(slots[i], ImVec2(slots[i].x + bw, slots[i].y + bh), theme::U32(c), r * 0.6f);
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, h + em * 0.5f));
    return changed;
}

bool ColorSwatches(const char* id, int* index, int count, const ImVec4* colors, const char* const* names)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float d = em * 1.6f, gap = em * 0.55f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float total = count * d + (count - 1) * gap;
    const float x0 = g_rtl ? p.x + avail - total : p.x;
    bool changed = false;
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i) {
        const ImVec2 c0(x0 + i * (d + gap), p.y);
        ImGui::SetCursorScreenPos(c0);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##sw", ImVec2(d, d))) { *index = i; changed = true; }
        const bool hov = ImGui::IsItemHovered();
        if (hov && names) Tooltip(names[i]);
        ImGui::PopID();
        const ImVec2 c(c0.x + d * 0.5f, c0.y + d * 0.5f);
        dl->AddCircleFilled(c, d * 0.5f * (hov ? 0.92f : 0.85f), theme::U32(colors[i]), 32);
        if (*index == i) dl->AddCircle(c, d * 0.5f + em * 0.12f, theme::U32(theme::kText), 32, em * 0.12f);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(avail, d + em * 0.2f));
    return changed;
}

bool Banner(const char* icon, const char* text, const ImVec4& color, const char* button)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float width = ImGui::GetContentRegionAvail().x;
    const float pad = em * 0.8f;
    const float iconW = (g_icons && icon) ? em * 1.6f : 0.f;
    float btnW = 0;
    if (button) btnW = TextSize(ImGui::GetFont(), em, button).x + em * 2.f;
    const float textMax = width - 2 * pad - iconW - (btnW > 0 ? btnW + em : 0);
    const ImVec2 ts = TextSize(ImGui::GetFont(), em, text, textMax);
    const float h = std::max(ts.y, button ? em * 2.f : 0.f) + pad * 1.4f;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), theme::U32(color, 0.12f), em * 0.6f);
    dl->AddRect(p, ImVec2(p.x + width, p.y + h), theme::U32(color, 0.35f), em * 0.6f, 0, em * 0.06f);
    if (iconW > 0)
        DrawText(dl, ImGui::GetFont(), em * 1.1f,
                 ImVec2(g_rtl ? p.x + width - pad - em * 1.1f : p.x + pad, p.y + (h - em) * 0.5f - em * 0.1f), color, icon);
    const float tx = g_rtl ? p.x + width - pad - iconW - std::min(ts.x, textMax) : p.x + pad + iconW;
    DrawText(dl, ImGui::GetFont(), em, ImVec2(tx, p.y + (h - ts.y) * 0.5f), theme::kText, text, textMax);
    bool pressed = false;
    if (button) {
        ImGui::SetCursorScreenPos(ImVec2(g_rtl ? p.x + pad : p.x + width - pad - btnW, p.y + (h - em * 2.f) * 0.5f));
        pressed = SecondaryButton(button, ImVec2(btnW, em * 2.f));
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(width, h + em * 0.6f));
    return pressed;
}

} // namespace ui
