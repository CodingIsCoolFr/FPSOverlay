#include "ui/hud.h"
#include "app/log.h"
#include "app/version.h"
#include "platform/win_util.h"
#include "ui/theme.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

constexpr wchar_t kClass[] = L"FPSOverlay.Hud";
constexpr float kBaseFontPx = 15.f;

// ── Layout model ────────────────────────────────────────────────────────────
// The HUD is laid out by hand (not with ImGui windows) so its exact size is known before it is
// drawn: the window is resized to fit with no one-frame lag, and baselines line up across fonts.

struct Span {
    ImFont* font = nullptr;
    float   size = 0;
    ImVec4  color;
    char    text[96] = {};
    float   slot = 0;           // minimum width (keeps numbers from jittering)
    bool    right = false;      // right-align inside the slot
    float   gap = 0;            // space before this span
    bool    separator = false;  // thin vertical rule instead of text
    float   graphW = 0;         // >0: inline frame-time graph of this width
};

struct Row {
    char   label[16] = {};
    ImVec4 labelColor;
    std::vector<Span> spans;
    float  graphH = 0;          // >0: full-width graph row
    bool   ruleAbove = false;
    bool   fullWidth = false;   // ignore the label column (footer line)
    float  gapAbove = 0;
};

struct Layout {
    std::vector<Row> rows;
    float labelCol = 0;
    float width = 0, height = 0;
    std::vector<float> rowAscent, rowHeight, rowWidth;
};

float Ascent(ImFont* f, float size) { return f->GetFontBaked(size)->Ascent; }

float TextW(ImFont* f, float size, const char* s)
{
    return f->CalcTextSizeA(size, FLT_MAX, 0.f, s).x;
}

ImVec4 Alpha(ImVec4 c, float a) { c.w *= a; return c; }

// Shortens UTF-8 text in place until it fits maxW, ending it with an ellipsis.
void Ellipsize(ImFont* f, float size, char* text, size_t cap, float maxW)
{
    if (TextW(f, size, text) <= maxW) return;
    static const char kDots[] = "\xE2\x80\xA6";
    size_t n = strlen(text);
    char buf[256];
    while (n > 0) {
        --n;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) --n;   // UTF-8 boundary
        while (n > 0 && text[n - 1] == ' ') --n;
        snprintf(buf, sizeof(buf), "%.*s%s", (int)n, text, kDots);
        if (TextW(f, size, buf) <= maxW) break;
    }
    snprintf(text, cap, "%s", buf);
}

struct Style {
    float em = 15.f;
    float textAlpha = 1.f;
    ImFont* body = nullptr;
    ImFont* strong = nullptr;
    ImVec4 accent;
    ImVec4 value = theme::kText;
    ImVec4 unit = theme::kTextDim;
    ImVec4 faint = theme::kTextFaint;
    bool colorize = true;
    bool fahrenheit = false;
};

ImVec4 Grade(const Style& st, float v, float warn, float bad, bool higherIsWorse = true)
{
    if (!st.colorize || !Has(v)) return st.value;
    if (higherIsWorse) return v >= bad ? theme::kBad : v >= warn ? theme::kWarn : st.value;
    return v <= bad ? theme::kBad : v <= warn ? theme::kWarn : theme::kGood;
}

void Value(Row& r, const Style& st, float gap, const char* text, const ImVec4& color, const char* slotTemplate,
           bool strong = true)
{
    Span s;
    s.font = strong ? st.strong : st.body;
    s.size = st.em;
    s.color = Alpha(color, st.textAlpha);
    snprintf(s.text, sizeof(s.text), "%s", text);
    s.gap = gap;
    if (slotTemplate) {
        s.slot = TextW(s.font, s.size, slotTemplate);
        s.right = true;
    }
    r.spans.push_back(s);
}

void Unit(Row& r, const Style& st, const char* unit)
{
    Span s;
    s.font = st.body;
    s.size = st.em * 0.78f;
    s.color = Alpha(st.unit, st.textAlpha);
    snprintf(s.text, sizeof(s.text), "%s", unit);
    s.gap = st.em * 0.12f;
    r.spans.push_back(s);
}

// value + unit, or a dash when the reading is missing.
void Metric(Row& r, const Style& st, bool first, float v, const char* fmt, const char* unit, const ImVec4& color,
            const char* slotTemplate)
{
    const float gap = first ? 0.f : st.em * 0.55f;
    if (!Has(v)) {
        Value(r, st, gap, "\xE2\x80\x93", st.faint, slotTemplate);   // en dash
        Unit(r, st, unit);
        return;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), fmt, v);
    Value(r, st, gap, buf, color, slotTemplate);
    Unit(r, st, unit);
}

void Separator(Row& r, const Style& st)
{
    Span s;
    s.separator = true;
    s.gap = st.em * 0.6f;
    s.slot = st.em * 0.08f;
    s.color = Alpha(st.faint, st.textAlpha * 0.8f);
    r.spans.push_back(s);
}

void Caption(Row& r, const Style& st, const char* text, const ImVec4& color, bool first)
{
    Span s;
    s.font = st.strong;
    s.size = st.em * 0.74f;
    s.color = Alpha(color, st.textAlpha);
    snprintf(s.text, sizeof(s.text), "%s", text);
    s.gap = first ? 0.f : st.em * 0.6f;
    r.spans.push_back(s);
}

float TempValue(const Style& st, float c) { return (Has(c) && st.fahrenheit) ? c * 9.f / 5.f + 32.f : c; }
const char* TempUnit(const Style& st) { return st.fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C"; }

void FormatTime(const cfg::Config& c, char* out, size_t cap)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (c.time24h) {
        if (c.timeSeconds) snprintf(out, cap, "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
        else snprintf(out, cap, "%02u:%02u", t.wHour, t.wMinute);
    } else {
        int h = t.wHour % 12;
        if (h == 0) h = 12;
        const char* ap = t.wHour < 12 ? "AM" : "PM";
        if (c.timeSeconds) snprintf(out, cap, "%d:%02u:%02u %s", h, t.wMinute, t.wSecond, ap);
        else snprintf(out, cap, "%d:%02u %s", h, t.wMinute, ap);
    }
}

void Measure(Layout& L, const Style& st, float graphMinW)
{
    L.rowAscent.assign(L.rows.size(), 0.f);
    L.rowHeight.assign(L.rows.size(), 0.f);
    L.rowWidth.assign(L.rows.size(), 0.f);
    L.labelCol = 0;
    for (const Row& r : L.rows)
        if (r.label[0]) L.labelCol = std::max(L.labelCol, TextW(st.strong, st.em * 0.74f, r.label));
    const float labelGap = L.labelCol > 0 ? st.em * 0.7f : 0.f;

    float w = 0, h = 0;
    for (size_t i = 0; i < L.rows.size(); ++i) {
        const Row& r = L.rows[i];
        float rw = (L.labelCol > 0 && !r.graphH && !r.fullWidth) ? L.labelCol + labelGap : 0.f;
        float asc = 0, desc = 0;
        for (const Span& s : r.spans) {
            float sw = 0;
            if (s.separator) {
                sw = s.slot;
                asc = std::max(asc, st.em * 0.8f);
                desc = std::max(desc, st.em * 0.25f);
            } else if (s.graphW > 0) {
                sw = s.graphW;
                asc = std::max(asc, st.em * 0.85f);
                desc = std::max(desc, st.em * 0.2f);
            } else {
                sw = std::max(TextW(s.font, s.size, s.text), s.slot);
                const float a = Ascent(s.font, s.size);
                asc = std::max(asc, a);
                desc = std::max(desc, s.size * 1.28f - a);
            }
            rw += s.gap + sw;
        }
        if (r.graphH > 0) {
            asc = r.graphH;
            desc = 0;
        }
        L.rowAscent[i] = asc;
        L.rowHeight[i] = asc + desc;
        L.rowWidth[i] = rw;
        w = std::max(w, rw);
        h += r.gapAbove + L.rowHeight[i];
    }
    L.width = std::max(w, graphMinW > 0 ? graphMinW : 0.f);
    L.height = h;
}

void DrawGraph(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const std::vector<float>* graph, float& scale, float avgMs,
               const Style& st, float bgAlpha)
{
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    dl->AddRectFilled(p0, p1, theme::U32(ImVec4(1, 1, 1, 0.035f * std::max(bgAlpha, 0.3f))), st.em * 0.25f);
    if (!graph || graph->size() < 2) return;
    const std::vector<float>& g = *graph;

    // Scale: comfortably above the typical frame, but let real spikes hit the ceiling.
    std::vector<float> sorted(g);
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() * 95 / 100, sorted.end());
    const float p95 = sorted[sorted.size() * 95 / 100];
    const float target = std::max({ p95 * 1.6f, avgMs * 2.f, 4.f });
    scale = scale <= 0 ? target : scale + (target - scale) * 0.08f;

    const float stepX = w / (float)(g.size() - 1);
    const ImU32 line = theme::U32(st.accent, st.textAlpha);
    const ImU32 fillTop = theme::U32(st.accent, 0.32f * st.textAlpha);
    const ImU32 fillBottom = theme::U32(st.accent, 0.02f * st.textAlpha);
    const ImU32 spike = theme::U32(theme::kBad, st.textAlpha);

    std::vector<ImVec2> pts(g.size());
    for (size_t i = 0; i < g.size(); ++i) {
        const float v = std::min(g[i] / scale, 1.f);
        pts[i] = ImVec2(p0.x + stepX * i, p1.y - v * (h - 1.f));
    }
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        dl->AddRectFilledMultiColor(ImVec2(pts[i].x, std::min(pts[i].y, pts[i + 1].y)), ImVec2(pts[i + 1].x, p1.y),
                                    fillTop, fillTop, fillBottom, fillBottom);
    }
    dl->AddPolyline(pts.data(), (int)pts.size(), line, 0, std::max(1.f, st.em * 0.09f));
    // Mark stutters: frames over twice the average.
    if (avgMs > 0) {
        for (size_t i = 0; i < g.size(); ++i)
            if (g[i] > avgMs * 2.f && g[i] > 8.f)
                dl->AddCircleFilled(pts[i], std::max(1.5f, st.em * 0.13f), spike, 8);
    }
}

} // namespace

// ── Window ──────────────────────────────────────────────────────────────────

bool Hud::Create(HINSTANCE inst, D3D& d3d)
{
    d3d_ = &d3d;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq_ = (double)f.QuadPart;

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = &Hud::WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                            kClass, APP_NAME_W, WS_POPUP, 0, 0, 64, 32, nullptr, nullptr, inst, this);
    if (!hwnd_) {
        logx::Error("HUD window creation failed: %lu", GetLastError());
        return false;
    }
    SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    const MARGINS m = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd_, &m);
    // No rounded corners or shadow from DWM around a transparent window.
    const int noRound = 1;   // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &noRound, sizeof(noRound));

    if (!target_.Create(d3d, hwnd_, 64, 32, SwapTarget::Mode::Transparent)) return false;

    IMGUI_CHECKVERSION();
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ctx_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui_ImplDX11_Init(d3d.Device(), d3d.Context());
    EnsureFonts(kBaseFontPx);
    ImGui::SetCurrentContext(prev);
    return true;
}

void Hud::Destroy()
{
    if (ctx_) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(ctx_);
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext(ctx_);
        ImGui::SetCurrentContext(prev == ctx_ ? nullptr : prev);
        ctx_ = nullptr;
    }
    target_.Destroy();
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void Hud::EnsureFonts(float pixelSize)
{
    if (fontBody_) return;
    theme::Fonts f = theme::LoadFonts("en-US", pixelSize);
    fontBody_ = f.body;
    fontStrong_ = f.strong;
}

void Hud::SetVisible(bool visible)
{
    visible_ = visible;
    if (!visible && shown_) {
        ShowWindow(hwnd_, SW_HIDE);
        shown_ = false;
    }
}

void Hud::SetClickThrough(bool on)
{
    if (on == clickThrough_) return;
    clickThrough_ = on;
    LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
    ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
    SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
}

void Hud::UpdateInteractive()
{
    if (dragging_) return;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    POINT pt;
    GetCursorPos(&pt);
    RECT r;
    GetWindowRect(hwnd_, &r);
    const bool over = PtInRect(&r, pt) != FALSE;
    const bool want = shown_ && ctrl && over;
    if (want != interactive_) {
        interactive_ = want;
        SetClickThrough(!want);
    }
}

bool Hud::ResolveMonitor(const cfg::Config& c, RECT& rc)
{
    // Monitor layout rarely changes; refresh once a second or on WM_DISPLAYCHANGE.
    const ULONGLONG now = GetTickCount64();
    if (!monCount_ || now - monCacheTick_ > 1000) {
        monCount_ = win::EnumerateMonitors(monCache_, 16);
        monCacheTick_ = now;
    }
    const win::MonitorInfo* mons = monCache_;
    const int n = monCount_;
    const win::MonitorInfo* mon = nullptr;
    for (int i = 0; i < n; ++i)
        if (!c.monitor.empty() && mons[i].name == c.monitor) mon = &mons[i];
    if (!mon)
        for (int i = 0; i < n; ++i)
            if (mons[i].primary) mon = &mons[i];
    if (!mon && n > 0) mon = &mons[0];
    if (!mon) return false;
    monitor_ = mon->handle;
    dpiScale_ = mon->dpi / 96.f;
    rc = mon->rect;
    return true;
}

void Hud::Place(const cfg::Config& c, int contentW, int contentH)
{
    RECT rc;
    if (!ResolveMonitor(c, rc)) return;
    const int monW = rc.right - rc.left;
    int x, y;
    if (c.customPos) {
        x = rc.left + c.posX;
        y = rc.top + c.posY;
    } else {
        const int m = (int)std::lround(c.margin * dpiScale_);
        const int col = (int)c.anchor % 3;          // 0 left, 1 center, 2 right
        const bool bottom = (int)c.anchor >= 3;
        x = col == 0 ? rc.left + m : col == 1 ? rc.left + (monW - contentW) / 2 : rc.right - m - contentW;
        y = bottom ? rc.bottom - m - contentH : rc.top + m;
    }
    // Never let the HUD leave its monitor.
    x = std::clamp(x, (int)rc.left, std::max((int)rc.left, (int)rc.right - contentW));
    y = std::clamp(y, (int)rc.top, std::max((int)rc.top, (int)rc.bottom - contentH));

    const int wx = x - pad_, wy = y - pad_;
    const int ww = contentW + 2 * pad_, wh = contentH + 2 * pad_;
    RECT cur;
    GetWindowRect(hwnd_, &cur);
    if (cur.left != wx || cur.top != wy || cur.right - cur.left != ww || cur.bottom - cur.top != wh)
        SetWindowPos(hwnd_, HWND_TOPMOST, wx, wy, ww, wh, SWP_NOACTIVATE | SWP_NOREDRAW);
}

void Hud::Tick(const cfg::Config& c, const HudFrameInfo& frames, const SensorSnapshot& sensors, bool force)
{
    if (!hwnd_ || !ctx_) return;
    UpdateInteractive();
    if (!visible_) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double sinceFrame = (now.QuadPart - lastFrameQpc_) / qpcFreq_;
    const int fps = interactive_ ? std::max(c.hudFps, 60) : c.hudFps;
    if (!force && lastFrameQpc_ && sinceFrame < 1.0 / fps - 0.001) return;
    const float dt = lastFrameQpc_ ? (float)std::clamp(sinceFrame, 0.0005, 0.25) : 1.f / 30.f;
    lastFrameQpc_ = now.QuadPart;

    // Re-assert topmost now and then: borderless games sometimes push themselves above us.
    if ((now.QuadPart - lastTopmostQpc_) / qpcFreq_ > 2.0) {
        lastTopmostQpc_ = now.QuadPart;
        if (shown_) SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    const int wantAffinity = c.hideFromCapture ? 1 : 0;
    if (wantAffinity != affinity_) {
        affinity_ = wantAffinity;
        SetWindowDisplayAffinity(hwnd_, wantAffinity ? 0x11 /* WDA_EXCLUDEFROMCAPTURE */ : WDA_NONE);
    }

    if (force || !lastStatsQpc_ || (now.QuadPart - lastStatsQpc_) / qpcFreq_ * 1000.0 >= c.statsIntervalMs) {
        lastStatsQpc_ = now.QuadPart;
        shownStats_ = frames.stats;
        shownFresh_ = frames.fresh;
        shownSensors_ = sensors;
    }

    if (target_.Occluded()) return;
    RECT monRect;
    ResolveMonitor(c, monRect);      // DPI for this frame's layout

    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = dt;

    // Two passes at most: if the content size changed, resize and lay out again so the
    // frame we present is never clipped.
    for (int pass = 0; pass < 2; ++pass) {
        io.DisplaySize = ImVec2((float)target_.Width(), (float)target_.Height());
        ImGui_ImplDX11_NewFrame();
        ImGui::NewFrame();
        BuildFrame(c, frames, shownSensors_, dpiScale_, interactive_);
        const int ww = contentW_ + 2 * pad_, wh = contentH_ + 2 * pad_;
        if (pass == 0 && (ww != (int)target_.Width() || wh != (int)target_.Height())) {
            ImGui::EndFrame();
            target_.Resize(ww, wh);
            continue;
        }
        ImGui::Render();
        break;
    }

    if (!dragging_) Place(c, contentW_, contentH_);

    const float clear[4] = { 0, 0, 0, 0 };
    target_.Bind(clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    target_.Present(false);
    ImGui::SetCurrentContext(prev);

    if (!shown_) {
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        shown_ = true;
    }
}

void Hud::BuildFrame(const cfg::Config& c, const HudFrameInfo& frames, const SensorSnapshot& sensors, float dpiScale,
                     bool glow)
{
    Style st;
    st.em = std::round(kBaseFontPx * (c.scale / 100.f) * dpiScale);
    st.textAlpha = c.textOpacity / 100.f;
    st.body = fontBody_;
    st.strong = fontStrong_;
    st.accent = theme::AccentAt(c.accent).color;
    st.colorize = c.colorValues;
    st.fahrenheit = c.fahrenheit;
    pad_ = (int)std::ceil(st.em * 0.5f);

    auto on = [&](cfg::Metric m) { return c.show[(int)m]; };
    const bool fresh = shownFresh_ && shownStats_.valid;
    const FrameStats& fs = shownStats_;
    const GpuReadings& g = sensors.gpu;
    const CpuReadings& cpu = sensors.cpu;
    const bool bar = c.layout == cfg::Layout::Bar;
    const bool vertical = c.layout == cfg::Layout::Vertical;
    const ImVec4 capGpu = bar ? ImVec4(0.52f, 0.90f, 0.70f, 1.f) : st.accent;
    const ImVec4 capCpu = bar ? ImVec4(0.94f, 0.86f, 0.55f, 1.f) : st.accent;
    const ImVec4 capFps = bar ? ImVec4(0.95f, 0.55f, 0.62f, 1.f) : st.accent;
    const ImVec4 capMem = bar ? ImVec4(0.62f, 0.75f, 1.00f, 1.f) : st.accent;

    Layout L;
    const float graphW = st.em * (vertical ? 13.f : 6.f);
    const float fpsVal = fresh ? fs.fps : kNoValue;

    // ── Frames ──
    auto addFrames = [&](Row& r, bool first) {
        bool any = false;
        if (on(cfg::Metric::Fps)) {
            if (!vertical && c.showLabels) Caption(r, st, "FPS", capFps, first && !any);
            Span s;
            s.font = st.strong;
            s.size = vertical ? st.em * 1.65f : st.em;
            const float v = fpsVal;
            s.color = Alpha(Has(v) ? Grade(st, v, 59.5f, 29.5f, false) : st.faint, st.textAlpha);
            if (!st.colorize && Has(v)) s.color = Alpha(st.value, st.textAlpha);
            if (Has(v)) snprintf(s.text, sizeof(s.text), "%.0f", v);
            else snprintf(s.text, sizeof(s.text), "\xE2\x80\x93");
            s.slot = TextW(s.font, s.size, "888");
            s.right = true;
            s.gap = (r.spans.empty()) ? 0.f : st.em * 0.3f;
            r.spans.push_back(s);
            any = true;
        }
        if (on(cfg::Metric::FrameTime)) {
            Metric(r, st, r.spans.empty(), fresh ? fs.frameTimeMs : kNoValue, "%.1f", "ms", st.value, "88.8");
            any = true;
        }
        return any;
    };
    auto addLows = [&](Row& r) {
        if (on(cfg::Metric::Low1)) {
            Caption(r, st, "1%", st.unit, r.spans.empty());
            Metric(r, st, true, fresh ? fs.low1 : kNoValue, "%.0f", "", Grade(st, fs.low1, 59.5f, 29.5f, false), "888");
            r.spans.back().gap = 0;     // empty unit
            r.spans[r.spans.size() - 2].gap = st.em * 0.25f;
        }
        if (on(cfg::Metric::Low01)) {
            Caption(r, st, "0.1%", st.unit, r.spans.empty());
            Metric(r, st, true, fresh ? fs.low01 : kNoValue, "%.0f", "", Grade(st, fs.low01, 59.5f, 29.5f, false), "888");
            r.spans.back().gap = 0;
            r.spans[r.spans.size() - 2].gap = st.em * 0.25f;
        }
    };

    // part: 0 = everything, 1 = load / temperature / power, 2 = clocks and fan
    auto gpuValues = [&](Row& r, int part) {
        bool first = true;
        auto put = [&](bool show, const char* tag, float v, const char* fmt, const char* unit, ImVec4 col, const char* slot) {
            if (!show) return;
            if (tag) {
                Caption(r, st, tag, st.unit, first);
                Metric(r, st, true, v, fmt, unit, col, slot);
                r.spans[r.spans.size() - 2].gap = st.em * 0.2f;
            } else {
                Metric(r, st, first, v, fmt, unit, col, slot);
            }
            first = false;
        };
        if (part != 2) {
            put(on(cfg::Metric::GpuLoad), nullptr, g.load, "%.0f", "%", st.value, "100");
            put(on(cfg::Metric::GpuTemp), nullptr, TempValue(st, g.temp), "%.0f", TempUnit(st), Grade(st, g.temp, 83.f, 90.f), "88");
            put(on(cfg::Metric::GpuHotspot), "hot", TempValue(st, g.hotspot), "%.0f", TempUnit(st), Grade(st, g.hotspot, 95.f, 105.f), "88");
            put(on(cfg::Metric::GpuPower), nullptr, g.power, "%.0f", "W", st.value, "888");
        }
        if (part != 1) {
            put(on(cfg::Metric::GpuClock), nullptr, g.coreClock, "%.0f", "MHz", st.value, "8888");
            put(on(cfg::Metric::GpuMemClock), "mem", g.memClock, "%.0f", "MHz", st.value, "88888");
            if (on(cfg::Metric::GpuFan)) {
                if (Has(g.fanRpm) && g.fanRpm > 0) put(true, "fan", g.fanRpm, "%.0f", "RPM", st.value, "8888");
                else put(true, "fan", g.fanPercent, "%.0f", "%", st.value, "100");
            }
        }
        return !first;
    };
    auto vramValues = [&](Row& r, bool first) {
        if (!on(cfg::Metric::Vram)) return false;
        char buf[48];
        if (Has(g.vramUsedMB) && Has(g.vramTotalMB) && g.vramTotalMB > 0) {
            const float pct = 100.f * g.vramUsedMB / g.vramTotalMB;
            snprintf(buf, sizeof(buf), "%.1f", g.vramUsedMB / 1024.f);
            Value(r, st, first ? 0.f : st.em * 0.55f, buf, Grade(st, pct, 90.f, 97.f), "88.8");
            snprintf(buf, sizeof(buf), "/ %.0f GB", g.vramTotalMB / 1024.f);
            Unit(r, st, buf);
        } else {
            Value(r, st, first ? 0.f : st.em * 0.55f, "\xE2\x80\x93", st.faint, "88.8");
            Unit(r, st, "GB");
        }
        return true;
    };
    auto cpuValues = [&](Row& r, int part) {
        bool first = true;
        auto put = [&](bool show, const char* tag, float v, const char* fmt, const char* unit, ImVec4 col, const char* slot) {
            if (!show) return;
            if (tag) {
                Caption(r, st, tag, st.unit, first);
                Metric(r, st, true, v, fmt, unit, col, slot);
                r.spans[r.spans.size() - 2].gap = st.em * 0.2f;
            } else {
                Metric(r, st, first, v, fmt, unit, col, slot);
            }
            first = false;
        };
        if (part != 2) {
            put(on(cfg::Metric::CpuLoad), nullptr, cpu.load, "%.0f", "%", Grade(st, cpu.load, 90.f, 98.f), "100");
            put(on(cfg::Metric::CpuTemp), nullptr, TempValue(st, cpu.temp), "%.0f", TempUnit(st), Grade(st, cpu.temp, 85.f, 95.f), "88");
            put(on(cfg::Metric::CpuPower), nullptr, cpu.power, "%.0f", "W", st.value, "888");
        }
        if (part != 1) {
            put(on(cfg::Metric::CpuClock), nullptr, Has(cpu.clock) ? cpu.clock / 1000.f : kNoValue, "%.2f", "GHz", st.value, "8.88");
            put(on(cfg::Metric::CpuFan), "fan", cpu.fanRpm, "%.0f", "RPM", st.value, "8888");
        }
        return !first;
    };
    auto ramValues = [&](Row& r, bool first) {
        if (!on(cfg::Metric::Ram)) return false;
        char buf[48];
        if (Has(sensors.ram.usedGB) && Has(sensors.ram.totalGB) && sensors.ram.totalGB > 0) {
            const float pct = 100.f * sensors.ram.usedGB / sensors.ram.totalGB;
            snprintf(buf, sizeof(buf), "%.1f", sensors.ram.usedGB);
            Value(r, st, first ? 0.f : st.em * 0.55f, buf, Grade(st, pct, 90.f, 97.f), "88.8");
            snprintf(buf, sizeof(buf), "/ %.0f GB", sensors.ram.totalGB);
            Unit(r, st, buf);
        } else {
            Value(r, st, first ? 0.f : st.em * 0.55f, "\xE2\x80\x93", st.faint, "88.8");
            Unit(r, st, "GB");
        }
        return true;
    };
    auto infoText = [&](char* out, size_t cap) {
        out[0] = '\0';
        std::string s;
        if (on(cfg::Metric::Process)) {
            if (!frames.target.empty()) s = frames.target;
            else if (!frames.captureRunning) s = "FPS capture unavailable";
        }
        if (on(cfg::Metric::Api) && !frames.api.empty() && on(cfg::Metric::Process) && !frames.target.empty())
            s += "  \xC2\xB7  " + frames.api;
        if (on(cfg::Metric::Time)) {
            char t[32];
            FormatTime(c, t, sizeof(t));
            if (!s.empty()) s += "  \xC2\xB7  ";
            s += t;
        }
        snprintf(out, cap, "%s", s.c_str());
    };

    if (vertical) {
        // FPS block: big number, frame time, then lows on a smaller line.
        Row fr;
        if (c.showLabels) {
            snprintf(fr.label, sizeof(fr.label), "FPS");
            fr.labelColor = capFps;
        }
        const bool hasFrames = addFrames(fr, true);
        if (on(cfg::Metric::Api) && !frames.api.empty() && !on(cfg::Metric::Process)) {
            Caption(fr, st, frames.api.c_str(), st.unit, fr.spans.empty());
        }
        if (hasFrames) L.rows.push_back(fr);

        Row lows;
        addLows(lows);
        if (!lows.spans.empty()) {
            lows.gapAbove = st.em * 0.1f;
            L.rows.push_back(lows);
        }
        if (on(cfg::Metric::FrameGraph)) {
            Row gr;
            gr.graphH = st.em * 2.2f;
            gr.gapAbove = st.em * 0.35f;
            L.rows.push_back(gr);
        }

        // Group rows: main numbers first, clocks and fans on a second line so nothing is ambiguous.
        auto addGroup = [&](const char* label, const ImVec4& color, auto&& values) {
            Row main;
            if (c.showLabels) { snprintf(main.label, sizeof(main.label), "%s", label); main.labelColor = color; }
            Row extra;
            const bool hasMain = values(main, 1);
            const bool hasExtra = values(extra, 2);
            if (!hasMain && !hasExtra) return;
            Row& first = hasMain ? main : extra;
            if (!hasMain && c.showLabels) { snprintf(extra.label, sizeof(extra.label), "%s", label); extra.labelColor = color; }
            first.gapAbove = st.em * 0.35f;
            first.ruleAbove = !L.rows.empty();
            L.rows.push_back(first);
            if (hasMain && hasExtra) {
                extra.gapAbove = st.em * 0.12f;
                L.rows.push_back(extra);
            }
        };

        addGroup("GPU", capGpu, gpuValues);

        Row vramRow;
        if (c.showLabels) { snprintf(vramRow.label, sizeof(vramRow.label), "VRAM"); vramRow.labelColor = capGpu; }
        if (vramValues(vramRow, true)) { vramRow.gapAbove = st.em * 0.15f; L.rows.push_back(vramRow); }

        addGroup("CPU", capCpu, cpuValues);

        Row ramRow;
        if (c.showLabels) { snprintf(ramRow.label, sizeof(ramRow.label), "RAM"); ramRow.labelColor = capMem; }
        if (ramValues(ramRow, true)) { ramRow.gapAbove = st.em * 0.15f; L.rows.push_back(ramRow); }

        char info[256];
        infoText(info, sizeof(info));
        if (info[0]) {
            // The footer never widens the panel beyond its other rows (long window titles).
            Measure(L, st, on(cfg::Metric::FrameGraph) ? graphW : 0.f);
            Row ir;
            Span s;
            s.font = st.body;
            s.size = st.em * 0.8f;
            s.color = Alpha(st.unit, st.textAlpha);
            snprintf(s.text, sizeof(s.text), "%s", info);
            Ellipsize(s.font, s.size, s.text, sizeof(s.text), std::max(L.width, st.em * 12.f));
            ir.spans.push_back(s);
            ir.gapAbove = st.em * 0.4f;
            ir.fullWidth = true;
            L.rows.push_back(ir);
        }
    } else {
        // One line. Groups separated by thin rules.
        Row r;
        bool any = false;
        auto sep = [&]() { if (any) Separator(r, st); };
        {
            Row tmp;
            if (addFrames(tmp, true)) {
                sep();
                for (auto& s : tmp.spans) r.spans.push_back(s);
                any = true;
            }
            Row lows;
            addLows(lows);
            if (!lows.spans.empty()) {
                if (any) lows.spans.front().gap = st.em * 0.55f;
                for (auto& s : lows.spans) r.spans.push_back(s);
                any = true;
            }
            if (on(cfg::Metric::FrameGraph)) {
                Span gs;
                gs.graphW = graphW;
                gs.gap = any ? st.em * 0.6f : 0.f;
                r.spans.push_back(gs);
                any = true;
            }
        }
        {
            Row tmp;
            const bool hasGpu = gpuValues(tmp, 0);
            Row vr;
            const bool hasVram = vramValues(vr, !hasGpu);
            if (hasGpu || hasVram) {
                sep();
                if (c.showLabels) Caption(r, st, "GPU", capGpu, r.spans.empty());
                for (size_t i = 0; i < tmp.spans.size(); ++i) {
                    Span s = tmp.spans[i];
                    if (i == 0 && c.showLabels) s.gap = st.em * 0.35f;
                    r.spans.push_back(s);
                }
                for (size_t i = 0; i < vr.spans.size(); ++i) {
                    Span s = vr.spans[i];
                    if (i == 0 && !hasGpu && c.showLabels) s.gap = st.em * 0.35f;
                    r.spans.push_back(s);
                }
                any = true;
            }
        }
        {
            Row tmp;
            if (cpuValues(tmp, 0)) {
                sep();
                if (c.showLabels) Caption(r, st, "CPU", capCpu, r.spans.empty());
                for (size_t i = 0; i < tmp.spans.size(); ++i) {
                    Span s = tmp.spans[i];
                    if (i == 0 && c.showLabels) s.gap = st.em * 0.35f;
                    r.spans.push_back(s);
                }
                any = true;
            }
        }
        {
            Row tmp;
            if (ramValues(tmp, true)) {
                sep();
                if (c.showLabels) Caption(r, st, "RAM", capMem, r.spans.empty());
                for (size_t i = 0; i < tmp.spans.size(); ++i) {
                    Span s = tmp.spans[i];
                    if (i == 0 && c.showLabels) s.gap = st.em * 0.35f;
                    r.spans.push_back(s);
                }
                any = true;
            }
        }
        char info[256];
        infoText(info, sizeof(info));
        if (info[0]) {
            sep();
            Span s;
            s.font = st.body;
            s.size = st.em * 0.82f;
            s.color = Alpha(st.unit, st.textAlpha);
            snprintf(s.text, sizeof(s.text), "%s", info);
            Ellipsize(s.font, s.size, s.text, sizeof(s.text), st.em * 16.f);
            s.gap = r.spans.empty() ? 0.f : st.em * 0.6f;
            r.spans.push_back(s);
            any = true;
        }
        if (!any) {
            Span s;
            s.font = st.body;
            s.size = st.em * 0.85f;
            s.color = Alpha(st.unit, st.textAlpha);
            snprintf(s.text, sizeof(s.text), "%s", APP_NAME);
            r.spans.push_back(s);
        }
        L.rows.push_back(r);
    }

    if (L.rows.empty()) {
        Row r;
        Span s;
        s.font = st.body;
        s.size = st.em * 0.85f;
        s.color = Alpha(st.unit, st.textAlpha);
        snprintf(s.text, sizeof(s.text), "%s", APP_NAME);
        r.spans.push_back(s);
        L.rows.push_back(r);
    }

    Measure(L, st, vertical && on(cfg::Metric::FrameGraph) ? graphW : 0.f);

    // ── Draw ──
    const float padX = bar ? st.em * 0.75f : st.em * 0.7f;
    const float padY = bar ? st.em * 0.4f : st.em * 0.55f;
    contentW_ = (int)std::ceil(L.width + 2 * padX);
    contentH_ = (int)std::ceil(L.height + 2 * padY);

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 o((float)pad_, (float)pad_);
    const ImVec2 e(o.x + contentW_, o.y + contentH_);
    const float rounding = bar ? 0.f : st.em * 0.55f;
    const float bgA = c.bgOpacity / 100.f;
    const ImVec4 bg = bar ? ImVec4(0, 0, 0, 1) : theme::kBg;
    if (bgA > 0.f) {
        dl->AddRectFilled(o, e, theme::U32(bg, bgA), rounding);
        if (!bar) dl->AddRect(o, e, theme::U32(theme::kBorder, std::min(1.f, bgA * 1.2f)), rounding, 0, 1.f);
    }
    if (glow) {
        const float pulse = 0.65f + 0.35f * std::sin((float)ImGui::GetTime() * 4.f);
        for (int i = 3; i >= 1; --i) {
            const float grow = (float)i * 1.5f;
            dl->AddRect(ImVec2(o.x - grow, o.y - grow), ImVec2(e.x + grow, e.y + grow),
                        theme::U32(st.accent, 0.18f * pulse * (4 - i)), rounding + grow, 0, 1.5f);
        }
        dl->AddRect(o, e, theme::U32(st.accent, pulse), rounding, 0, 1.5f);
    }

    float y = o.y + padY;
    const float labelGap = L.labelCol > 0 ? st.em * 0.7f : 0.f;
    for (size_t i = 0; i < L.rows.size(); ++i) {
        const Row& r = L.rows[i];
        y += r.gapAbove;
        if (r.ruleAbove && !bar) {
            const float ry = y - r.gapAbove * 0.5f;
            dl->AddLine(ImVec2(o.x + padX, ry), ImVec2(e.x - padX, ry), theme::U32(theme::kBorder, st.textAlpha * 0.9f), 1.f);
        }
        if (r.graphH > 0) {
            DrawGraph(dl, ImVec2(o.x + padX, y), ImVec2(o.x + padX + L.width, y + r.graphH), frames.graph, graphScale_,
                      fresh ? fs.frameTimeMs : 0.f, st, bgA);
            y += L.rowHeight[i];
            continue;
        }
        const float base = y + L.rowAscent[i];
        float x = o.x + padX;
        if (L.labelCol > 0 && !r.fullWidth) {
            if (r.label[0]) {
                const float ls = st.em * 0.74f;
                dl->AddText(st.strong, ls, ImVec2(x, std::floor(base - Ascent(st.strong, ls))),
                            theme::U32(Alpha(r.labelColor, st.textAlpha)), r.label);
            }
            x += L.labelCol + labelGap;
        }
        for (const Span& s : r.spans) {
            x += s.gap;
            if (s.separator) {
                dl->AddLine(ImVec2(x, base - st.em * 0.75f), ImVec2(x, base + st.em * 0.2f), theme::U32(s.color), s.slot);
                x += s.slot;
                continue;
            }
            if (s.graphW > 0) {
                DrawGraph(dl, ImVec2(x, base - st.em * 0.85f), ImVec2(x + s.graphW, base + st.em * 0.15f), frames.graph,
                          graphScale_, fresh ? fs.frameTimeMs : 0.f, st, bgA);
                x += s.graphW;
                continue;
            }
            const float tw = TextW(s.font, s.size, s.text);
            const float w = std::max(tw, s.slot);
            const float tx = s.right ? x + (w - tw) : x;
            dl->AddText(s.font, s.size, ImVec2(std::floor(tx), std::floor(base - Ascent(s.font, s.size))),
                        theme::U32(s.color), s.text);
            x += w;
        }
        y += L.rowHeight[i];
    }
}

bool Hud::RenderToImage(const cfg::Config& c, const HudFrameInfo& frames, const SensorSnapshot& sensors,
                        const float background[4], std::vector<uint8_t>& rgba, int& w, int& h)
{
    if (!ctx_ || !d3d_) return false;
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(ctx_);
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.f / 30.f;
    shownStats_ = frames.stats;
    shownFresh_ = frames.fresh;
    graphScale_ = 0.f;

    // Lay out once to learn the size, then render for real.
    io.DisplaySize = ImVec2(4096, 4096);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    BuildFrame(c, frames, sensors, 1.f, false);
    ImGui::EndFrame();
    w = contentW_ + 2 * pad_;
    h = contentH_ + 2 * pad_;

    io.DisplaySize = ImVec2((float)w, (float)h);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    BuildFrame(c, frames, sensors, 1.f, false);
    ImGui::Render();

    ID3D11Device* dev = d3d_->Device();
    ID3D11DeviceContext* ctx = d3d_->Context();
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* tex = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11Texture2D* staging = nullptr;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &tex)) &&
              SUCCEEDED(dev->CreateRenderTargetView(tex, nullptr, &rtv));
    if (ok) {
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->ClearRenderTargetView(rtv, background);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ok = SUCCEEDED(dev->CreateTexture2D(&td, nullptr, &staging));
    }
    if (ok) {
        ctx->CopyResource(staging, tex);
        D3D11_MAPPED_SUBRESOURCE map = {};
        ok = SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map));
        if (ok) {
            rgba.resize((size_t)w * h * 4);
            for (int row = 0; row < h; ++row)
                memcpy(&rgba[(size_t)row * w * 4], static_cast<const uint8_t*>(map.pData) + (size_t)row * map.RowPitch, (size_t)w * 4);
            ctx->Unmap(staging, 0);
        }
    }
    if (staging) staging->Release();
    if (rtv) rtv->Release();
    if (tex) tex->Release();
    ImGui::SetCurrentContext(prev);
    return ok;
}

// ── Window procedure ────────────────────────────────────────────────────────

LRESULT CALLBACK Hud::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Hud*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->hwnd_ == hwnd) return self->Handle(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Hud::Handle(UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_NCHITTEST:
        // Interactive (Ctrl held): the whole HUD is a drag handle.
        return interactive_ ? HTCAPTION : HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ENTERSIZEMOVE:
        dragging_ = true;
        return 0;
    case WM_MOVING: {
        // Snap to the monitor edges (12 px) while dragging.
        RECT* r = reinterpret_cast<RECT*>(lp);
        HMONITOR mon = MonitorFromRect(r, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(mon, &mi)) {
            const int snap = (int)(12 * win::DpiForMonitor(mon) / 96);
            const int w = r->right - r->left, h = r->bottom - r->top;
            const RECT& m = mi.rcMonitor;
            const int cl = r->left + pad_, ct = r->top + pad_, cr = r->right - pad_, cb = r->bottom - pad_;
            if (std::abs(cl - m.left) < snap) r->left = m.left - pad_;
            else if (std::abs(cr - m.right) < snap) r->left = m.right + pad_ - w;
            if (std::abs(ct - m.top) < snap) r->top = m.top - pad_;
            else if (std::abs(cb - m.bottom) < snap) r->top = m.bottom + pad_ - h;
            r->right = r->left + w;
            r->bottom = r->top + h;
        }
        return TRUE;
    }
    case WM_EXITSIZEMOVE: {
        dragging_ = false;
        RECT r;
        GetWindowRect(hwnd_, &r);
        if (onMoved) onMoved(r.left + pad_, r.top + pad_);
        return 0;
    }
    case WM_NCRBUTTONUP:
    case WM_RBUTTONUP: {
        POINT pt;
        GetCursorPos(&pt);
        if (onMenu) onMenu(pt);
        return 0;
    }
    case WM_DPICHANGED:
        // The HUD sizes itself; ignore the suggested rectangle.
        monCount_ = 0;
        return 0;
    case WM_DISPLAYCHANGE:
        monCount_ = 0;
        break;
    case WM_ERASEBKGND:
        return 1;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}
