#include "ui/settings_window.h"
#include "app/ini.h"
#include "app/log.h"
#include "app/version.h"
#include "locale/locale.h"
#include "platform/win_util.h"
#include "ui/theme.h"
#include "ui/widgets.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandlerEx(HWND, UINT, WPARAM, LPARAM, ImGuiIO&);

namespace {

constexpr wchar_t kClass[] = L"FPSOverlay.Settings";
constexpr float kBaseFontPx = 16.f;
constexpr int kDefaultW = 940, kDefaultH = 680, kMinW = 760, kMinH = 540;

enum PageId { kPageOverlay = 0, kPageAppearance, kPageSensors, kPageHotkeys, kPageGeneral, kPageAbout, kPageCount };

const char* T(const char* s) { return locale::T(s); }

struct Lang { const char* code; const char* name; };
const Lang kLangs[] = {
    { "en-US", "English" },         { "zh-CN", "\xE7\xAE\x80\xE4\xBD\x93\xE4\xB8\xAD\xE6\x96\x87" },
    { "ar", "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9" },
    { "fa", "\xD9\x81\xD8\xA7\xD8\xB1\xD8\xB3\xDB\x8C" },
    { "fr", "Fran\xC3\xA7" "ais" },     { "nl", "Nederlands" },     { "de", "Deutsch" },
    { "it", "Italiano" },              { "es", "Espa\xC3\xB1ol" },  { "pt-BR", "Portugu\xC3\xAAs (Brasil)" },
    { "pt-PT", "Portugu\xC3\xAAs (Portugal)" }, { "ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E" },
    { "ko", "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4" }, { "ru", "\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9" },
    { "pl", "Polski" },                { "tr", "T\xC3\xBCrk\xC3\xA7" "e" },
};

std::string Serialize(const cfg::Config& c)
{
    Ini ini;
    cfg::Store(c, ini);
    return ini.Serialize();
}

std::string Fmt(const char* fmt, float v)
{
    char buf[64];
    snprintf(buf, sizeof(buf), fmt, v);
    return buf;
}

float TempDisplay(const cfg::Config& c, float celsius) { return c.fahrenheit ? celsius * 9.f / 5.f + 32.f : celsius; }

} // namespace

// ── Window lifetime ─────────────────────────────────────────────────────────

bool SettingsWindow::Open(HINSTANCE inst, D3D& d3d, cfg::Config& config)
{
    cfg_ = &config;
    if (hwnd_) {
        if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
        SetForegroundWindow(hwnd_);
        return true;
    }
    d3d_ = &d3d;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq_ = (double)f.QuadPart;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &SettingsWindow::WndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.hbrBackground = CreateSolidBrush(RGB(15, 17, 21));
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    // Open on the monitor under the cursor, centred, sized for its DPI.
    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const UINT dpi = win::DpiForMonitor(mon);
    dpiScale_ = dpi / 96.f;
    RECT r = { 0, 0, (LONG)(kDefaultW * dpiScale_), (LONG)(kDefaultH * dpiScale_) };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    const int w = std::min<int>(r.right - r.left, mi.rcWork.right - mi.rcWork.left);
    const int h = std::min<int>(r.bottom - r.top, mi.rcWork.bottom - mi.rcWork.top);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
    const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;

    const std::wstring title = std::wstring(APP_NAME_W) + L" \x2014 " + win::ToWide(T("Settings"));
    hwnd_ = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, this);
    if (!hwnd_) return false;
    win::ApplyDarkTitleBar(hwnd_, RGB(21, 24, 30), RGB(232, 234, 240), RGB(42, 47, 58));
    dpiScale_ = GetDpiForWindow(hwnd_) / 96.f;

    RECT cr;
    GetClientRect(hwnd_, &cr);
    if (!target_.Create(d3d, hwnd_, cr.right - cr.left, cr.bottom - cr.top, SwapTarget::Mode::Flip)) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return false;
    }
    CreateContext(d3d, true);
    lastSerialized_ = Serialize(*cfg_);
    welcomeOpen_ = !cfg_->firstRunDone;

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    return true;
}

void SettingsWindow::Close()
{
    if (!hwnd_) return;
    capturing_ = -1;
    DestroyContext();
    target_.Destroy();
    HWND h = hwnd_;
    hwnd_ = nullptr;
    DestroyWindow(h);
}

void SettingsWindow::CreateContext(D3D& d3d, bool withWin32)
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (withWin32) ImGui_ImplWin32_Init(hwnd_);
    ImGui_ImplDX11_Init(d3d.Device(), d3d.Context());
    fontsDirty_ = true;
    styleDirty_ = true;
    ImGui::SetCurrentContext(prev);
}

void SettingsWindow::DestroyContext()
{
    if (!ctx_) return;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx_);
    ImGui_ImplDX11_Shutdown();
    if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(ctx_);
    ImGui::SetCurrentContext(prev == ctx_ ? nullptr : prev);
    ctx_ = nullptr;
}

void SettingsWindow::ApplyStyle()
{
    if (fontsDirty_) {
        const theme::Fonts f = theme::LoadFonts(cfg_->language.c_str(), kBaseFontPx);
        ui::SetFonts(f.body, f.strong, f.hasIcons);
        fontsDirty_ = false;
    }
    if (styleDirty_) {
        theme::SetAccent(cfg_->accent);
        theme::ApplySettingsStyle(dpiScale_);
        styleDirty_ = false;
    }
    ui::SetRtl(locale::IsRtl());
}

UiActions SettingsWindow::TakeActions()
{
    UiActions a = actions_;
    actions_ = UiActions();
    return a;
}

// ── Frame ───────────────────────────────────────────────────────────────────

bool SettingsWindow::Tick(const UiStatus& status, const SensorSnapshot& sensors)
{
    if (!hwnd_ || !ctx_) return false;
    if (minimized_ || target_.Occluded()) return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const bool active = GetForegroundWindow() == hwnd_;
    const double interval = active ? 1.0 / 60.0 : 1.0 / 15.0;
    const double since = (now.QuadPart - lastFrameQpc_) / qpcFreq_;
    if (lastFrameQpc_ && since < interval - 0.001) return false;
    lastFrameQpc_ = now.QuadPart;

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx_);
    ApplyStyle();

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    Draw(status, sensors);
    ImGui::Render();

    const float clear[4] = { theme::kBg.x, theme::kBg.y, theme::kBg.z, 1.f };
    target_.Bind(clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    target_.Present(false);
    ImGui::SetCurrentContext(prev);

    std::string current = Serialize(*cfg_);
    if (current != lastSerialized_) {
        lastSerialized_ = std::move(current);
        return true;
    }
    return false;
}

void SettingsWindow::Draw(const UiStatus& status, const SensorSnapshot& sensors)
{
    const ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoScrollbar);

    const float headerH = em * 3.4f;
    const float footerH = em * 3.6f;
    const float sideW = std::round(em * 13.5f);
    const float bodyH = io.DisplaySize.y - headerH - footerH;
    const bool rtl = ui::IsRtl();

    DrawHeader(status, headerH);

    ImGui::SetCursorPos(ImVec2(rtl ? io.DisplaySize.x - sideW : 0.f, headerH));
    DrawSidebar(sideW, bodyH + footerH);

    ImGui::SetCursorPos(ImVec2(rtl ? 0.f : sideW, headerH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.2f));
    ImGui::BeginChild("##page", ImVec2(io.DisplaySize.x - sideW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    DrawPage(status, sensors);
    ImGui::Dummy(ImVec2(0, em * 0.5f));
    ImGui::EndChild();

    ImGui::SetCursorPos(ImVec2(rtl ? 0.f : sideW, headerH + bodyH));
    DrawFooter(status, footerH);

    if (welcomeOpen_) DrawWelcome(sensors);
    ImGui::End();
}

void SettingsWindow::DrawHeader(const UiStatus& status, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();
    const bool rtl = ui::IsRtl();
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(io.DisplaySize.x, height), theme::U32(theme::kSurface));
    dl->AddLine(ImVec2(0, height - 1), ImVec2(io.DisplaySize.x, height - 1), theme::U32(theme::kBorder));

    // Logo mark: accent tile with a tiny frame-time trace.
    const float tile = em * 1.9f;
    const float lx = rtl ? io.DisplaySize.x - em * 1.1f - tile : em * 1.1f;
    const ImVec2 t0(lx, (height - tile) * 0.5f);
    dl->AddRectFilled(t0, ImVec2(t0.x + tile, t0.y + tile), theme::U32(theme::Accent()), em * 0.45f);
    const ImVec2 trace[] = {
        { t0.x + tile * 0.18f, t0.y + tile * 0.62f }, { t0.x + tile * 0.38f, t0.y + tile * 0.62f },
        { t0.x + tile * 0.50f, t0.y + tile * 0.28f }, { t0.x + tile * 0.62f, t0.y + tile * 0.74f },
        { t0.x + tile * 0.82f, t0.y + tile * 0.50f },
    };
    dl->AddPolyline(trace, 5, IM_COL32(255, 255, 255, 240), 0, em * 0.13f);

    ImFont* strong = ui::StrongFont();
    const float titleSize = em * 1.12f;
    const ImVec2 ts = strong->CalcTextSizeA(titleSize, FLT_MAX, 0, APP_NAME);
    const char* ver = "v" APP_VERSION;
    const ImVec2 vs = ImGui::GetFont()->CalcTextSizeA(em * 0.8f, FLT_MAX, 0, ver);
    const float ty = (height - ts.y) * 0.5f;
    if (!rtl) {
        dl->AddText(strong, titleSize, ImVec2(lx + tile + em * 0.7f, ty), theme::U32(theme::kText), APP_NAME);
        dl->AddText(ImGui::GetFont(), em * 0.8f, ImVec2(lx + tile + em * 0.7f + ts.x + em * 0.5f, ty + ts.y - vs.y - em * 0.1f),
                    theme::U32(theme::kTextFaint), ver);
    } else {
        const float x = lx - em * 0.7f - ts.x;
        dl->AddText(strong, titleSize, ImVec2(x, ty), theme::U32(theme::kText), APP_NAME);
        dl->AddText(ImGui::GetFont(), em * 0.8f, ImVec2(x - em * 0.5f - vs.x, ty + ts.y - vs.y - em * 0.1f),
                    theme::U32(theme::kTextFaint), ver);
    }

    // Status chips on the far side.
    const char* capText = status.captureRunning ? T("FPS capture on") : T("FPS capture off");
    const ImVec4 capCol = status.captureRunning ? theme::kGood : theme::kWarn;
    const char* hudText = status.hudVisible ? T("Overlay shown") : T("Overlay hidden");
    const ImVec4 hudCol = status.hudVisible ? theme::Accent() : theme::kTextDim;
    auto chipW = [&](const char* s) { return ImGui::GetFont()->CalcTextSizeA(em * 0.8f, FLT_MAX, 0, s).x + em * 1.2f; };
    const float w1 = chipW(capText), w2 = chipW(hudText);
    const float chipY = (height - em * 1.25f) * 0.5f;
    float cx = rtl ? em * 1.1f : io.DisplaySize.x - em * 1.1f - w1 - w2 - em * 0.4f;
    ImGui::SetCursorPos(ImVec2(cx, chipY));
    ui::Badge(hudText, hudCol);
    if (ImGui::IsItemHovered()) ui::Tooltip(T("Click the button at the bottom or press your hotkey to show or hide it."));
    ImGui::SetCursorPos(ImVec2(cx + w2 + em * 0.4f, chipY));
    ui::Badge(capText, capCol);
    if (ImGui::IsItemHovered() && !status.captureError.empty()) ui::Tooltip(status.captureError.c_str());
}

void SettingsWindow::DrawSidebar(float width, float height)
{
    const float em = ImGui::GetFontSize();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::kSurface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 0.7f, em * 0.9f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.f);
    ImGui::BeginChild("##nav", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    struct Item { const char* icon; const char* label; };
    const Item items[kPageCount] = {
        { theme::icon::Display, T("Overlay") },
        { theme::icon::Palette, T("Appearance") },
        { theme::icon::Pulse, T("Sensors") },
        { theme::icon::Keyboard, T("Hotkeys") },
        { theme::icon::Settings, T("General") },
        { theme::icon::Info, T("About") },
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, em * 0.2f));
    for (int i = 0; i < kPageCount; ++i) {
        if (ui::SidebarItem(items[i].icon, items[i].label, cfg_->settingsPage == i)) {
            cfg_->settingsPage = i;
            capturing_ = -1;
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    // Divider between sidebar and page.
    const ImVec2 p = ImGui::GetItemRectMin(), q = ImGui::GetItemRectMax();
    const float x = ui::IsRtl() ? p.x : q.x - 1;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y), ImVec2(x, q.y), theme::U32(theme::kBorder));
}

void SettingsWindow::DrawFooter(const UiStatus& status, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();
    const bool rtl = ui::IsRtl();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float sideW = std::round(em * 13.5f);
    const float w = io.DisplaySize.x - sideW;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), theme::U32(theme::kBg));
    dl->AddLine(p, ImVec2(p.x + w, p.y), theme::U32(theme::kBorder));

    // Left: what is being measured right now.
    char info[256];
    if (!status.target.empty() && status.liveFps > 0)
        snprintf(info, sizeof(info), "%s  \xC2\xB7  %.0f FPS", status.target.c_str(), status.liveFps);
    else if (!status.target.empty())
        snprintf(info, sizeof(info), "%s  \xC2\xB7  %s", status.target.c_str(), T("not drawing frames"));
    else
        snprintf(info, sizeof(info), "%s", T("Waiting for a game. Start one and click into it."));
    const ImVec2 is = ImGui::GetFont()->CalcTextSizeA(em * 0.9f, FLT_MAX, 0, info);
    const float pad = em * 1.6f;

    const char* hideLbl = status.hudVisible ? T("Hide overlay") : T("Show overlay");
    const char* doneLbl = T("Close to tray");
    const float bw1 = ImGui::CalcTextSize(hideLbl).x + em * 2.f;
    const float bw2 = ImGui::CalcTextSize(doneLbl).x + em * 2.f;
    const float bh = em * 2.1f;
    const float by = p.y + (height - bh) * 0.5f;
    const float maxInfo = w - 2 * pad - bw1 - bw2 - em * 1.5f;
    const float ix = rtl ? p.x + w - pad - std::min(is.x, maxInfo) : p.x + pad;
    dl->PushClipRect(ImVec2(rtl ? p.x + w - pad - maxInfo : p.x + pad, p.y), ImVec2(rtl ? p.x + w - pad : p.x + pad + maxInfo, p.y + height), true);
    dl->AddText(ImGui::GetFont(), em * 0.9f, ImVec2(ix, p.y + (height - is.y) * 0.5f), theme::U32(theme::kTextDim), info);
    dl->PopClipRect();

    float bx = rtl ? p.x + pad : p.x + w - pad - bw2 - em * 0.5f - bw1;
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    if (ui::SecondaryButton(hideLbl, ImVec2(bw1, bh))) actions_.toggleHud = true;
    ImGui::SetCursorScreenPos(ImVec2(bx + bw1 + em * 0.5f, by));
    if (ui::PrimaryButton(doneLbl, ImVec2(bw2, bh))) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

void SettingsWindow::DrawPage(const UiStatus& status, const SensorSnapshot& sensors)
{
    switch (cfg_->settingsPage) {
        case kPageOverlay:    PageMetrics(sensors); break;
        case kPageAppearance: PageAppearance(sensors); break;
        case kPageSensors:    PageSensors(status, sensors); break;
        case kPageHotkeys:    PageHotkeys(); break;
        case kPageGeneral:    PageGeneral(status); break;
        case kPageAbout:      PageAbout(status, sensors); break;
        default:             cfg_->settingsPage = kPageOverlay; break;
    }
}

// ── Pages ───────────────────────────────────────────────────────────────────

void SettingsWindow::PageMetrics(const SensorSnapshot& s)
{
    ui::PageHeader(T("Overlay"), T("Pick what the overlay shows. Changes apply right away."));

    const bool lhmStarting = s.status.lhm == SourceState::Starting || s.status.lhm == SourceState::Off;
    auto live = [&](cfg::Metric m, std::string& text, ImVec4& color) -> bool {
        color = theme::Accent();
        const GpuReadings& g = s.gpu;
        const CpuReadings& c = s.cpu;
        float v = kNoValue;
        const char* fmt = nullptr;
        bool tempUnit = false;
        switch (m) {
            case cfg::Metric::GpuLoad:     v = g.load; fmt = "%.0f%%"; break;
            case cfg::Metric::GpuTemp:     v = g.temp; tempUnit = true; break;
            case cfg::Metric::GpuHotspot:  v = g.hotspot; tempUnit = true; break;
            case cfg::Metric::GpuPower:    v = g.power; fmt = "%.0f W"; break;
            case cfg::Metric::GpuClock:    v = g.coreClock; fmt = "%.0f MHz"; break;
            case cfg::Metric::GpuMemClock: v = g.memClock; fmt = "%.0f MHz"; break;
            case cfg::Metric::GpuFan:
                if (Has(g.fanRpm) && g.fanRpm > 0) { v = g.fanRpm; fmt = "%.0f RPM"; }
                else { v = g.fanPercent; fmt = "%.0f%%"; }
                break;
            case cfg::Metric::Vram:
                if (Has(g.vramUsedMB) && Has(g.vramTotalMB)) {
                    char b[48];
                    snprintf(b, sizeof(b), "%.1f / %.0f GB", g.vramUsedMB / 1024.f, g.vramTotalMB / 1024.f);
                    text = b;
                    return true;
                }
                break;
            case cfg::Metric::CpuLoad:     v = c.load; fmt = "%.0f%%"; break;
            case cfg::Metric::CpuTemp:     v = c.temp; tempUnit = true; break;
            case cfg::Metric::CpuPower:    v = c.power; fmt = "%.0f W"; break;
            case cfg::Metric::CpuClock:    v = Has(c.clock) ? c.clock / 1000.f : kNoValue; fmt = "%.2f GHz"; break;
            case cfg::Metric::CpuFan:      v = c.fanRpm; fmt = "%.0f RPM"; break;
            case cfg::Metric::Ram:
                if (Has(s.ram.usedGB)) {
                    char b[48];
                    snprintf(b, sizeof(b), "%.1f / %.0f GB", s.ram.usedGB, s.ram.totalGB);
                    text = b;
                    return true;
                }
                break;
            default: return false;
        }
        if (Has(v)) {
            if (tempUnit) text = Fmt(cfg_->fahrenheit ? "%.0f \xC2\xB0" "F" : "%.0f \xC2\xB0" "C", TempDisplay(*cfg_, v));
            else text = Fmt(fmt, v);
            return true;
        }
        // No reading: explain why.
        const bool needsDriver = (m == cfg::Metric::CpuTemp || m == cfg::Metric::CpuPower || m == cfg::Metric::CpuFan);
        if (lhmStarting && (needsDriver || m == cfg::Metric::GpuHotspot)) { text = T("Loading"); color = theme::kTextDim; }
        else if (needsDriver && !s.status.pawnioInstalled) { text = T("Needs PawnIO"); color = theme::kWarn; }
        else { text = T("Not available"); color = theme::kTextFaint; }
        return true;
    };

    for (int gi = 0; gi < (int)cfg::Group::Count; ++gi) {
        const cfg::Group grp = (cfg::Group)gi;
        char id[32];
        snprintf(id, sizeof(id), "##grp%d", gi);
        ui::BeginCard(id, T(cfg::GroupLabel(grp)));
        for (int mi = 0; mi < (int)cfg::Metric::Count; ++mi) {
            const cfg::MetricInfo& info = cfg::Info((cfg::Metric)mi);
            if (info.group != grp) continue;
            std::string badge;
            ImVec4 color;
            const bool hasBadge = live(info.id, badge, color);
            // "Needs the PawnIO driver" only while it is missing (the fan note stays while no fan is found).
            const char* hint = info.hint ? T(info.hint) : nullptr;
            const bool driverMetric = info.id == cfg::Metric::CpuTemp || info.id == cfg::Metric::CpuPower || info.id == cfg::Metric::CpuFan;
            if (driverMetric && s.status.pawnioInstalled && !(info.id == cfg::Metric::CpuFan && !Has(s.cpu.fanRpm))) hint = nullptr;
            ui::ToggleRow(T(info.label), &cfg_->show[mi], hint, hasBadge ? badge.c_str() : nullptr, hasBadge ? &color : nullptr);
        }
        ui::EndCard();
    }
}

void SettingsWindow::PageAppearance(const SensorSnapshot&)
{
    const float em = ImGui::GetFontSize();
    ui::PageHeader(T("Appearance"), T("Style, size and position of the overlay."));

    ui::BeginCard("##layout", T("Layout"));
    {
        const char* items[] = { T("Vertical"), T("Horizontal"), T("Bar") };
        int v = (int)cfg_->layout;
        if (ui::SegmentedRow(T("Style"), T("Vertical is a compact panel. Horizontal and Bar fit everything on one line."), &v, items, 3))
            cfg_->layout = (cfg::Layout)v;
    }
    ui::EndCard();

    ui::BeginCard("##pos", T("Position"));
    {
        const float pickW = std::min(em * 15.f, ImGui::GetContentRegionAvail().x * 0.45f);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        const bool rtl = ui::IsRtl();
        ImGui::SetCursorScreenPos(ImVec2(rtl ? start.x + avail - pickW - em * 0.6f : start.x + em * 0.6f, start.y + em * 0.4f));
        int anchor = (int)cfg_->anchor;
        if (ui::PositionPicker("##picker", &anchor, cfg_->customPos, pickW)) {
            cfg_->anchor = (cfg::Anchor)anchor;
            cfg_->customPos = false;
        }
        const float pickBottom = ImGui::GetCursorScreenPos().y;
        // Explanation next to the picker.
        const float textX = rtl ? start.x + em * 0.6f : start.x + pickW + em * 1.8f;
        const float textW = avail - pickW - em * 2.4f;
        ImGui::SetCursorScreenPos(ImVec2(textX, start.y + em * 0.6f));
        ImGui::BeginGroup();    // keeps every line of the text column at textX
        ui::Paragraph(cfg_->customPos ? T("Custom position") : T("Pinned to a corner"), theme::kText, textW);
        ImGui::Dummy(ImVec2(0, em * 0.15f));
        ui::Paragraph(T("Click a spot on the screen, or hold Ctrl over the overlay and drag it anywhere. It snaps to the screen edges."),
                      theme::kTextDim, textW);
        if (cfg_->customPos) {
            ImGui::Dummy(ImVec2(0, em * 0.3f));
            const char* snap = T("Snap back to corner");
            if (rtl) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.f, textW - ImGui::CalcTextSize(snap).x - em * 2.f));
            if (ui::SecondaryButton(snap)) actions_.resetPosition = true;
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(start.x, std::max(pickBottom, ImGui::GetCursorScreenPos().y) + em * 0.3f));
        ImGui::Dummy(ImVec2(avail, 0));

        // Monitor choice
        win::MonitorInfo mons[16];
        const int n = win::EnumerateMonitors(mons, 16);
        auto monLabel = [&](int i) {
            char b[192];
            snprintf(b, sizeof(b), "%s  (%ld \xC3\x97 %ld)%s%s", locale::TF("Display %d", i + 1),
                     mons[i].rect.right - mons[i].rect.left, mons[i].rect.bottom - mons[i].rect.top,
                     mons[i].primary ? "  \xC2\xB7  " : "", mons[i].primary ? T("main") : "");
            return std::string(b);
        };
        std::string preview = T("Main display");
        for (int i = 0; i < n; ++i)
            if (!cfg_->monitor.empty() && mons[i].name == cfg_->monitor) preview = monLabel(i);
        if (ui::BeginComboRow(T("Screen"), n > 1 ? T("Which monitor the overlay lives on.") : nullptr, preview.c_str())) {
            if (ui::ComboItem(T("Main display"), cfg_->monitor.empty())) { cfg_->monitor.clear(); cfg_->customPos = false; }
            for (int i = 0; i < n; ++i) {
                if (ui::ComboItem(monLabel(i).c_str(), cfg_->monitor == mons[i].name)) {
                    cfg_->monitor = mons[i].name;
                    cfg_->customPos = false;
                }
            }
            ui::EndComboRow();
        }
    }
    ui::EndCard();

    ui::BeginCard("##look", T("Look"));
    ui::SliderRow(T("Size"), nullptr, &cfg_->scale, 50, 250, "%d%%");
    ui::SliderRow(T("Background"), T("0% shows only the text."), &cfg_->bgOpacity, 0, 100, "%d%%");
    ui::SliderRow(T("Text opacity"), nullptr, &cfg_->textOpacity, 20, 100, "%d%%");
    {
        // Accent color row
        const float pad = em * 0.6f;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + em * 0.5f));
        const char* lbl = T("Accent color");
        const ImVec2 ls = ImGui::CalcTextSize(lbl);
        ImGui::SetCursorScreenPos(ImVec2(ui::IsRtl() ? p0.x + avail - pad - ls.x : p0.x + pad, p0.y + em * 0.75f));
        ImGui::TextColored(theme::kText, "%s", lbl);
        ImVec4 colors[theme::kAccentCount];
        const char* names[theme::kAccentCount];
        for (int i = 0; i < theme::kAccentCount; ++i) {
            colors[i] = theme::AccentAt(i).color;
            names[i] = T(theme::AccentAt(i).name);
        }
        const float swW = theme::kAccentCount * em * 1.6f + (theme::kAccentCount - 1) * em * 0.55f;
        ImGui::SetCursorScreenPos(ImVec2(ui::IsRtl() ? p0.x + pad : p0.x + avail - pad - swW, p0.y + em * 0.45f));
        ImGui::PushID("##accent");
        ImGui::BeginGroup();
        int acc = cfg_->accent;
        // Swatches are right-aligned by drawing them in a child-width region.
        ImGui::SetNextItemWidth(swW);
        if (ui::ColorSwatches("##sw", &acc, theme::kAccentCount, colors, names)) {
            cfg_->accent = acc;
            styleDirty_ = true;
        }
        ImGui::EndGroup();
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(p0.x, std::max(ImGui::GetCursorScreenPos().y, p0.y + em * 2.4f)));
        ImGui::Dummy(ImVec2(avail, em * 0.2f));
    }
    ui::ToggleRow(T("Color-coded values"), &cfg_->colorValues, T("Green, yellow and red for frame rate, temperatures and memory."));
    ui::ToggleRow(T("Show labels"), &cfg_->showLabels, T("FPS, GPU, CPU and RAM captions."));
    ui::EndCard();

    ui::BeginCard("##behaviour", T("Behavior"));
    ui::ToggleRow(T("Hide when no game is running"), &cfg_->hideWhenIdle,
                  T("The overlay appears only while the app in front is drawing frames."));
    ui::ToggleRow(T("Hide from screenshots and recordings"), &cfg_->hideFromCapture,
                  T("OBS, Discord streams and screenshots will not show the overlay. You still see it."));
    ui::EndCard();
}

void SettingsWindow::PageSensors(const UiStatus& status, const SensorSnapshot& s)
{
    ui::PageHeader(T("Sensors"), T("Where the numbers come from, and how often they update."));

    ui::BeginCard("##gpu", T("Graphics card"));
    {
        std::string preview = T("Automatic");
        if (!cfg_->gpu.empty() && s.activeAdapter >= 0 && s.activeAdapter < (int)s.adapters.size())
            preview = s.adapters[s.activeAdapter].name;
        else if (s.activeAdapter >= 0 && s.activeAdapter < (int)s.adapters.size())
            preview = std::string(T("Automatic")) + "  (" + s.adapters[s.activeAdapter].name + ")";
        if (ui::BeginComboRow(T("Graphics card to monitor"), s.adapters.size() > 1 ? T("Automatic picks the card with the most video memory.") : nullptr,
                              preview.c_str())) {
            if (ui::ComboItem(T("Automatic"), cfg_->gpu.empty())) cfg_->gpu.clear();
            for (const auto& a : s.adapters) {
                char detail[64];
                snprintf(detail, sizeof(detail), "%.0f GB", a.dedicatedBytes / (1024.0 * 1024.0 * 1024.0));
                if (ui::ComboItem(a.name.c_str(), cfg_->gpu == a.key, detail)) cfg_->gpu = a.key;
            }
            ui::EndComboRow();
        }
    }
    ui::EndCard();

    ui::BeginCard("##cpu", T("Processor"));
    {
        auto labelFor = [](const std::vector<LhmSensorChoice>& list, const std::string& id) -> std::string {
            for (const auto& c : list)
                if (c.id == id) return c.label;
            return {};
        };
        // Plain names for the CPU readings people choose between; the rest keep LHM's names
        // ("P-Core #3", "CCD1 (Tdie)").
        auto tempName = [](const std::string& lhmName, const char** detail) -> std::string {
            const char* d = nullptr;
            std::string name = lhmName;
            if (lhmName == "CPU Package" || lhmName == "Package" || lhmName == "Core (Tctl/Tdie)" || lhmName == "Core (Tdie)") {
                name = T("Package");
                d = T("The CPU's own reading, which HWiNFO, Corsair iCUE and NZXT CAM show by default.");
            } else if (lhmName == "Core Max") {
                name = T("Hottest core");
            } else if (lhmName == "Core Average") {
                name = T("Average of all cores");
                d = T("Lower than Package. NZXT CAM's Average setting shows this one.");
            } else if (lhmName == "Core (Tctl)") {
                name = T("Control temperature");
                d = T("Includes a fan-control offset, so it reads higher than the real temperature.");
            }
            if (detail) *detail = d;
            return name;
        };
        std::string used = labelFor(s.cpuTempChoices, s.cpuTempSensorUsed);
        if (!used.empty()) used = tempName(used, nullptr);
        std::string preview = cfg_->cpuTempSensor.empty()
                                  ? std::string(T("Automatic")) + (used.empty() ? "" : "  (" + used + ")")
                                  : tempName(labelFor(s.cpuTempChoices, cfg_->cpuTempSensor), nullptr);
        if (preview.empty()) preview = cfg_->cpuTempSensor;
        if (ui::BeginComboRow(T("Temperature sensor"), T("Package is the CPU's own reading. Average of all cores is lower and steadier."),
                              preview.c_str())) {
            if (ui::ComboItem(T("Automatic"), cfg_->cpuTempSensor.empty())) cfg_->cpuTempSensor.clear();
            for (const auto& c : s.cpuTempChoices) {
                const char* detail = nullptr;
                const std::string name = tempName(c.label, &detail);
                ImGui::PushID(c.id.c_str());    // two LHM names can map to the same plain name
                if (ui::ComboItem(name.c_str(), cfg_->cpuTempSensor == c.id, detail)) cfg_->cpuTempSensor = c.id;
                ImGui::PopID();
            }
            ui::EndComboRow();
        }
        used = labelFor(s.fanChoices, s.cpuFanSensorUsed);
        preview = cfg_->cpuFanSensor.empty() ? std::string(T("Automatic")) + (used.empty() ? "" : "  (" + used + ")")
                                             : labelFor(s.fanChoices, cfg_->cpuFanSensor);
        if (preview.empty()) preview = T("None found");
        if (ui::BeginComboRow(T("Fan sensor"), s.fanChoices.empty() ? T("No motherboard fans were found. They need the PawnIO driver.") : nullptr,
                              preview.c_str())) {
            if (ui::ComboItem(T("Automatic"), cfg_->cpuFanSensor.empty())) cfg_->cpuFanSensor.clear();
            for (const auto& c : s.fanChoices)
                if (ui::ComboItem(c.label.c_str(), cfg_->cpuFanSensor == c.id)) cfg_->cpuFanSensor = c.id;
            ui::EndComboRow();
        }
    }
    ui::EndCard();

    ui::BeginCard("##pawnio", T("PawnIO driver"), T("CPU temperature, CPU power and motherboard fans are read through this small open-source driver."));
    {
        const auto& st = s.status;
        if (st.pawnioBusy) {
            ui::InfoRow(T("Status"), T("Installing..."), &theme::kWarn);
        } else if (!st.pawnioInstalled) {
            if (ui::ButtonRow(T("Not installed"), T("Install it to see CPU temperature and power. No restart needed in most cases."),
                              T("Install"), true))
                actions_.installPawnIo = true;
        } else if (st.pawnioOutdated) {
            std::string label = std::string(T("Installed")) + " " + st.pawnioVersion;
            if (ui::ButtonRow(label.c_str(), T("A newer version comes with this app."), T("Update"), true))
                actions_.installPawnIo = true;
        } else {
            std::string v = std::string(T("Installed")) + (st.pawnioVersion.empty() ? "" : "  " + st.pawnioVersion);
            ui::InfoRow(T("Status"), v.c_str(), &theme::kGood);
        }
        if (st.pawnioResult != 0 && !st.pawnioBusy) {
            const bool ok = st.pawnioResult == 1;
            ui::InfoRow(T("Last install"), ok ? T("Installed. CPU sensors are loading.") : st.pawnioMessage.c_str(),
                        ok ? &theme::kGood : &theme::kBad);
            if (ok && st.pawnioInstalled && !st.cpuTempAvailable && st.lhm == SourceState::Ok)
                ui::InfoRow("", T("If the CPU temperature stays empty, restart Windows once."), &theme::kTextDim);
        }
    }
    ui::EndCard();

    ui::BeginCard("##rates", T("Update rates"));
    {
        const char* hz[] = { "15", "30", "60", "120" };
        const int hzVals[] = { 15, 30, 60, 120 };
        int i = 1;
        for (int k = 0; k < 4; ++k) if (cfg_->hudFps == hzVals[k]) i = k;
        if (ui::SegmentedRow(T("Overlay redraws per second"), T("30 is smooth for numbers. Raise it for a smoother frame time graph."), &i, hz, 4))
            cfg_->hudFps = hzVals[i];

        const char* ms[] = { "0.25 s", "0.5 s", "1 s" };
        const int msVals[] = { 250, 500, 1000 };
        i = 1;
        for (int k = 0; k < 3; ++k) if (cfg_->statsIntervalMs == msVals[k]) i = k;
        if (ui::SegmentedRow(T("Numbers change every"), nullptr, &i, ms, 3)) cfg_->statsIntervalMs = msVals[i];

        const char* poll[] = { "0.5 s", "1 s", "2 s" };
        const int pollVals[] = { 500, 1000, 2000 };
        i = 1;
        for (int k = 0; k < 3; ++k) if (cfg_->sensorIntervalMs == pollVals[k]) i = k;
        if (ui::SegmentedRow(T("Read sensors every"), nullptr, &i, poll, 3)) cfg_->sensorIntervalMs = pollVals[i];

        const char* win[] = { "5 s", "10 s", "30 s", "60 s" };
        const int winVals[] = { 5, 10, 30, 60 };
        i = 1;
        for (int k = 0; k < 4; ++k) if (cfg_->lowsWindowSec == winVals[k]) i = k;
        if (ui::SegmentedRow(T("1% low covers the last"), T("Longer windows are steadier; shorter ones react faster."), &i, win, 4))
            cfg_->lowsWindowSec = winVals[i];
    }
    ui::EndCard();

    ui::BeginCard("##sources", T("Data sources"));
    {
        auto state = [](SourceState st, const char*& text, ImVec4& col) {
            switch (st) {
                case SourceState::Ok:       text = T("Working"); col = theme::kGood; break;
                case SourceState::Starting: text = T("Starting"); col = theme::kWarn; break;
                case SourceState::Failed:   text = T("Not available"); col = theme::kTextFaint; break;
                default:                    text = T("Not used"); col = theme::kTextFaint; break;
            }
        };
        const char* t;
        ImVec4 c;
        ui::InfoRow(T("Frame capture (Event Tracing for Windows)"),
                    status.captureRunning ? T("Working") : (status.captureError.empty() ? T("Off") : status.captureError.c_str()),
                    status.captureRunning ? &theme::kGood : &theme::kBad);
        state(s.status.nvml, t, c);
        ui::InfoRow(T("NVIDIA driver (NVML)"), t, &c);
        state(s.status.pdh, t, c);
        ui::InfoRow(T("Windows performance counters"), t, &c);
        state(s.status.lhm, t, c);
        std::string lhm = t;
        if (s.status.lhm == SourceState::Failed && !s.status.lhmError.empty()) lhm = s.status.lhmError;
        ui::InfoRow(T("LibreHardwareMonitor"), lhm.c_str(), &c);
        char cost[64];
        snprintf(cost, sizeof(cost), "%.1f ms", s.status.tickMs);
        ui::InfoRow(T("Sensor read time"), cost);
    }
    ui::EndCard();
}

void SettingsWindow::PageHotkeys()
{
    const float em = ImGui::GetFontSize();
    ui::PageHeader(T("Hotkeys"), T("They work everywhere, even while a game has focus."));

    ui::BeginCard("##keys", T("Keyboard shortcuts"), T("Click a key, then press the new combination. Esc cancels, Backspace removes it."));
    const char* labels[] = { T("Show or hide the overlay"), T("Reset 1% lows"), T("Open settings"), T("Quit") };
    for (int i = 0; i < (int)cfg::HotkeyAction::Count; ++i) {
        const cfg::Hotkey& hk = cfg_->hotkeys[i];
        std::string text = capturing_ == i ? T("Press keys...") : (hk.vk ? win::HotkeyName(hk.vk, hk.mods) : T("Not set"));
        const float keyW = std::max(em * 9.f, ImGui::CalcTextSize(text.c_str()).x + em * 2.f);
        const float padX = em * 0.6f, padY = em * 0.45f;
        const float avail = ImGui::GetContentRegionAvail().x;
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const float rowH = em * 2.0f + padY * 2;
        const ImVec2 ls = ImGui::CalcTextSize(labels[i]);
        const bool rtl = ui::IsRtl();
        ImGui::GetWindowDrawList()->AddText(ImVec2(rtl ? p0.x + avail - padX - ls.x : p0.x + padX, p0.y + (rowH - ls.y) * 0.5f),
                                            theme::U32(theme::kText), labels[i]);
        ImGui::SetCursorScreenPos(ImVec2(rtl ? p0.x + padX : p0.x + avail - padX - keyW, p0.y + padY));
        char id[16];
        snprintf(id, sizeof(id), "##hk%d", i);
        if (ui::KeyCap(id, text.c_str(), capturing_ == i, keyW)) capturing_ = (capturing_ == i) ? -1 : i;
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowH));
        ImGui::Dummy(ImVec2(avail, 0));
    }
    ui::EndCard();

    ui::BeginCard("##mouse", T("Mouse"));
    ui::InfoRow(T("Move the overlay"), T("Hold Ctrl and drag it"));
    ui::InfoRow(T("Overlay menu"), T("Hold Ctrl and right-click it"));
    ui::EndCard();
}

void SettingsWindow::PageGeneral(const UiStatus& status)
{
    ui::PageHeader(T("General"), nullptr);

    ui::BeginCard("##startup", T("Startup"));
    bool autostart = status.autostart;
    if (ui::ToggleRow(T("Start with Windows"), &autostart,
                      T("Starts quietly in the tray when you sign in. No admin prompt.")))
        actions_.setAutostart = autostart ? 1 : 0;
    ui::EndCard();

    if (updater::Enabled()) {
        ui::BeginCard("##updates", T("Updates"));
        ui::ToggleRow(T("Install updates automatically"), &cfg_->autoUpdate,
                      T("Downloads new versions from GitHub, checks them and installs them when no game is running."));
        ui::EndCard();
    }

    ui::BeginCard("##units", T("Units"));
    {
        const char* temp[] = { "\xC2\xB0" "C", "\xC2\xB0" "F" };
        int t = cfg_->fahrenheit ? 1 : 0;
        if (ui::SegmentedRow(T("Temperature"), nullptr, &t, temp, 2)) cfg_->fahrenheit = t == 1;
        const char* clock[] = { T("24-hour"), T("12-hour") };
        int ck = cfg_->time24h ? 0 : 1;
        if (ui::SegmentedRow(T("Clock"), nullptr, &ck, clock, 2)) cfg_->time24h = ck == 0;
        ui::ToggleRow(T("Show seconds"), &cfg_->timeSeconds);
    }
    ui::EndCard();

    ui::BeginCard("##lang", T("Language"));
    {
        const char* current = cfg_->language.c_str();
        for (const auto& l : kLangs)
            if (_stricmp(l.code, cfg_->language.c_str()) == 0) current = l.name;
        if (ui::BeginComboRow(T("Language"), nullptr, current)) {
            for (const auto& l : kLangs) {
                if (ui::ComboItem(l.name, _stricmp(l.code, cfg_->language.c_str()) == 0)) {
                    if (_stricmp(l.code, cfg_->language.c_str()) != 0) {
                        cfg_->language = l.code;
                        actions_.languageChanged = true;
                        fontsDirty_ = true;
                    }
                }
            }
            ui::EndComboRow();
        }
    }
    ui::EndCard();
}

void SettingsWindow::PageAbout(const UiStatus& status, const SensorSnapshot& s)
{
    const float em = ImGui::GetFontSize();
    ui::PageHeader(APP_NAME, T("A lightweight frame rate and hardware overlay for Windows games."));

    ui::BeginCard("##ver", nullptr);
    ui::InfoRow(T("Version"), APP_VERSION);
    ui::InfoRow(T("Administrator"), status.elevated ? T("Yes") : T("No (FPS capture needs it)"),
                status.elevated ? &theme::kGood : &theme::kWarn);
    using updater::State;
    const updater::Status& up = status.update;
    switch (up.state) {
    case State::Off:
        break;
    case State::Checking:
        ui::ButtonRow(T("Updates"), nullptr, T("Checking..."), false, true);
        break;
    case State::Downloading: {
        char pct[16];
        snprintf(pct, sizeof(pct), "%.0f%%", up.progress * 100.f);
        ui::InfoRow(locale::TF("Downloading version %s", up.version.c_str()), pct, &theme::kTextDim);
        break;
    }
    case State::Ready:
        if (ui::ButtonRow(locale::TF("Version %s is ready", up.version.c_str()),
                          T("It installs by itself when no game is running."), T("Install now"), true))
            actions_.installUpdate = true;
        break;
    case State::Available:
        if (ui::ButtonRow(locale::TF("Version %s is available", up.version.c_str()), nullptr, T("Update"), true))
            actions_.installUpdate = true;
        break;
    case State::Failed:
        if (up.version.empty()) {
            if (ui::ButtonRow(T("Update check failed"), up.error.c_str(), T("Try again"), false)) actions_.checkUpdates = true;
        } else if (ui::ButtonRow(locale::TF("Version %s could not be installed", up.version.c_str()), up.error.c_str(),
                                 T("Download"), false)) {
            actions_.openUpdatePage = true;
        }
        break;
    default:
        if (ui::ButtonRow(T("Updates"), up.state == State::UpToDate ? T("You have the newest version.") : nullptr,
                          T("Check now"), false))
            actions_.checkUpdates = true;
        break;
    }
    ui::EndCard();

    ui::BeginCard("##pc", T("This PC"));
    ui::InfoRow(T("Processor"), s.cpuName.c_str());
    for (size_t i = 0; i < s.adapters.size(); ++i) {
        char v[192];
        snprintf(v, sizeof(v), "%s  (%.0f GB)", s.adapters[i].name.c_str(), s.adapters[i].dedicatedBytes / (1024.0 * 1024.0 * 1024.0));
        ui::InfoRow(i == 0 ? T("Graphics") : "", v);
    }
    if (Has(s.ram.totalGB)) {
        char v[64];
        snprintf(v, sizeof(v), "%.0f GB", s.ram.totalGB);
        ui::InfoRow(T("Memory"), v);
    }
    ui::EndCard();

    ui::BeginCard("##files", T("Files"));
    if (ui::ButtonRow(T("Settings file"), status.configPath.c_str(), T("Open folder"))) actions_.openConfigFolder = true;
    ui::EndCard();

    ui::BeginCard("##license", T("License"));
    {
        const float pad = em * 0.6f;
        ImGui::Indent(pad);
        const float w = ImGui::GetContentRegionAvail().x - pad;
        ui::Paragraph(T("Free software under the GNU General Public License v3. See LICENSE.txt and NOTICE.md next to the app."),
                      theme::kTextDim, w);
        ImGui::Dummy(ImVec2(0, em * 0.3f));
        ui::Paragraph(T("Built with Dear ImGui (MIT), LibreHardwareMonitor (MPL 2.0), RTLScript and the PawnIO driver."),
                      theme::kTextFaint, w);
        ImGui::Unindent(pad);
        ImGui::Dummy(ImVec2(0, em * 0.3f));
    }
    ui::EndCard();
}

void SettingsWindow::DrawWelcome(const SensorSnapshot& s)
{
    const float em = ImGui::GetFontSize();
    if (!ImGui::IsPopupOpen("##welcome")) ImGui::OpenPopup("##welcome");
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float w = std::min(em * 30.f, ds.x - em * 4.f);
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, theme::kCard);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 1.4f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, em * 0.8f);
    if (ImGui::BeginPopupModal("##welcome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize)) {
        const bool rtl = ui::IsRtl();
        ImGui::PushFont(ui::StrongFont(), ImGui::GetStyle().FontSizeBase * 1.35f);
        const char* title = T("Welcome");
        if (rtl) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title).x);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Spacing();
        auto bullet = [&](const char* text) {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float indent = em * 1.1f;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(rtl ? p.x + avail - indent * 0.45f : p.x + indent * 0.45f, p.y + em * 0.62f),
                                                        em * 0.16f, theme::U32(theme::Accent()), 12);
            ImGui::SetCursorScreenPos(ImVec2(rtl ? p.x : p.x + indent, p.y));
            ImGui::BeginGroup();
            ui::Paragraph(text, theme::kText, avail - indent);
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, em * 0.25f));
        };
        bullet(T("The overlay is already running. Start a game and click into it to see its frame rate."));
        bullet(T("Press Insert to show or hide it. You can change this under Hotkeys."));
        bullet(T("Hold Ctrl over the overlay to drag it, or Ctrl + right-click for its menu."));
        bullet(T("Closing this window keeps the overlay running in the tray."));
        if (!s.status.pawnioInstalled)
            bullet(T("For CPU temperature, install the PawnIO driver under Sensors."));
        ImGui::Dummy(ImVec2(0, em * 0.4f));
        if (ui::PrimaryButton(T("Got it"), ImVec2(ImGui::GetContentRegionAvail().x, em * 2.2f))) {
            cfg_->firstRunDone = true;
            welcomeOpen_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ── Offscreen render (screenshots) ──────────────────────────────────────────

bool SettingsWindow::RenderPageToImage(D3D& d3d, cfg::Config& config, int page, int width, int height,
                                       const UiStatus& status, const SensorSnapshot& sensors, std::vector<uint8_t>& rgba,
                                       float dpiScale)
{
    cfg_ = &config;
    d3d_ = &d3d;
    dpiScale_ = dpiScale;
    welcomeOpen_ = page < 0;
    config.settingsPage = page < 0 ? 0 : page;
    CreateContext(d3d, false);
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);
    io.DeltaTime = 1.f / 60.f;
    io.MousePos = ImVec2(-1000, -1000);

    // Several frames: auto-sized cards measure themselves on the previous frame,
    // and animations need a moment to settle.
    for (int i = 0; i < 40; ++i) {
        ApplyStyle();
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        Draw(status, sensors);
        if (i < 39) ImGui::EndFrame();
        else ImGui::Render();
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    ID3D11Texture2D* staging = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    bool ok = SUCCEEDED(d3d.Device()->CreateTexture2D(&td, nullptr, &tex)) &&
              SUCCEEDED(d3d.Device()->CreateRenderTargetView(tex, nullptr, &rtv));
    if (ok) {
        const float clear[4] = { theme::kBg.x, theme::kBg.y, theme::kBg.z, 1.f };
        d3d.Context()->OMSetRenderTargets(1, &rtv, nullptr);
        d3d.Context()->ClearRenderTargetView(rtv, clear);
        D3D11_VIEWPORT vp = { 0, 0, (float)width, (float)height, 0, 1 };
        d3d.Context()->RSSetViewports(1, &vp);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = SUCCEEDED(d3d.Device()->CreateTexture2D(&td, nullptr, &staging));
    }
    if (ok) {
        d3d.Context()->CopyResource(staging, tex);
        D3D11_MAPPED_SUBRESOURCE map = {};
        ok = SUCCEEDED(d3d.Context()->Map(staging, 0, D3D11_MAP_READ, 0, &map));
        if (ok) {
            rgba.resize((size_t)width * height * 4);
            for (int r = 0; r < height; ++r)
                memcpy(&rgba[(size_t)r * width * 4], static_cast<const uint8_t*>(map.pData) + (size_t)r * map.RowPitch, (size_t)width * 4);
            d3d.Context()->Unmap(staging, 0);
        }
    }
    if (staging) staging->Release();
    if (rtv) rtv->Release();
    if (tex) tex->Release();
    ImGui::SetCurrentContext(prev);
    DestroyContext();
    return ok;
}

// ── Window procedure ────────────────────────────────────────────────────────

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT SettingsWindow::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    // Hotkey capture eats key presses before ImGui sees them.
    if (capturing_ >= 0 && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
        const UINT vk = (UINT)wp;
        if (vk == VK_ESCAPE) { capturing_ = -1; return 0; }
        if (vk == VK_BACK || vk == VK_DELETE) {
            cfg_->hotkeys[capturing_] = cfg::Hotkey();
            capturing_ = -1;
            return 0;
        }
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN ||
            vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU)
            return 0;       // wait for the real key
        UINT mods = 0;
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
        if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
        if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
        const cfg::Hotkey hk{ vk, mods };
        // One combination, one action: take it away from any other slot.
        for (int i = 0; i < (int)cfg::HotkeyAction::Count; ++i)
            if (i != capturing_ && cfg_->hotkeys[i] == hk) cfg_->hotkeys[i] = cfg::Hotkey();
        cfg_->hotkeys[capturing_] = hk;
        capturing_ = -1;
        return 0;
    }

    if (ctx_) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(ctx_);
        const LRESULT r = ImGui_ImplWin32_WndProcHandlerEx(hwnd_, msg, wp, lp, ImGui::GetIO());
        ImGui::SetCurrentContext(prev);
        if (r) return r;
    }

    switch (msg) {
    case WM_SIZE:
        minimized_ = (wp == SIZE_MINIMIZED);
        if (!minimized_ && target_.Valid()) target_.Resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = (LONG)(kMinW * dpiScale_);
        mmi->ptMinTrackSize.y = (LONG)(kMinH * dpiScale_);
        return 0;
    }
    case WM_DPICHANGED: {
        dpiScale_ = HIWORD(wp) / 96.f;
        styleDirty_ = true;
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        Close();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
