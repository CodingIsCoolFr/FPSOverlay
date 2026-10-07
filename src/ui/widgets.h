// Custom ImGui widgets for the settings window. All sizes derive from the current font size,
// so they scale with DPI, and every row mirrors itself for right-to-left languages.
#pragma once

#include "imgui.h"

namespace ui {

void SetRtl(bool rtl);
bool IsRtl();

void SetFonts(ImFont* body, ImFont* strong, bool hasIcons);
ImFont* StrongFont();
bool HasIcons();

// ── Containers ──
// A rounded card. Content inside uses the full card width.
void BeginCard(const char* id, const char* title = nullptr, const char* subtitle = nullptr);
void EndCard();
void PageHeader(const char* title, const char* subtitle);

// ── Rows (label on the start side, control on the end side) ──
// Clicking anywhere on the row flips the switch. `badge` is a small status pill after the label.
bool ToggleRow(const char* label, bool* v, const char* hint = nullptr,
               const char* badge = nullptr, const ImVec4* badgeColor = nullptr, bool disabled = false);

// Segmented control on its own full-width line under a label.
bool SegmentedRow(const char* label, const char* hint, int* v, const char* const* items, int count);

// Integer slider with the value shown at the end of the label line.
bool SliderRow(const char* label, const char* hint, int* v, int min, int max, const char* valueFmt);

// Label + dropdown. Returns true while the dropdown is open; call EndComboRow() only then.
bool BeginComboRow(const char* label, const char* hint, const char* preview);
void EndComboRow();
bool ComboItem(const char* label, bool selected, const char* detail = nullptr);

// Label + a button on the end side. Returns true when the button is clicked.
bool ButtonRow(const char* label, const char* hint, const char* button, bool primary = false, bool disabled = false);

// Label + a value (read-only), e.g. "Graphics card    NVIDIA GeForce RTX 4080".
void InfoRow(const char* label, const char* value, const ImVec4* valueColor = nullptr);

// ── Standalone controls ──
bool Segmented(const char* id, int* v, const char* const* items, int count, float width);
bool ToggleSwitch(const char* id, bool* v, bool disabled = false);
bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0), bool disabled = false);
bool SecondaryButton(const char* label, const ImVec2& size = ImVec2(0, 0), bool disabled = false);
bool SidebarItem(const char* icon, const char* label, bool selected);
bool KeyCap(const char* id, const char* text, bool listening, float width);
void Badge(const char* text, const ImVec4& color);
// Six anchor dots on a miniature screen. `custom` dims them (HUD was dragged by hand).
bool PositionPicker(const char* id, int* anchor, bool custom, float width);
// Accent swatches; returns true when one is picked.
bool ColorSwatches(const char* id, int* index, int count, const ImVec4* colors, const char* const* names);
// Banner across the content width with an optional action button. Returns true on click.
bool Banner(const char* icon, const char* text, const ImVec4& color, const char* button = nullptr);

void Tooltip(const char* text);
// Wrapped text at the cursor (right-aligned and wrapped correctly for right-to-left languages).
// width <= 0 uses the available width.
void Paragraph(const char* text, const ImVec4& color, float width = 0.f, ImFont* font = nullptr, float sizeScale = 1.f);
void Spacing(float lines = 1.f);
float Em(float v);      // v * current font size

} // namespace ui
