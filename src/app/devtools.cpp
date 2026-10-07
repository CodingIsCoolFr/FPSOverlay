#include "app/devtools.h"
#include "app/config.h"
#include "app/log.h"
#include "app/version.h"
#include "capture/frame_capture.h"
#include "capture/frame_stats.h"
#include "locale/locale.h"
#include "platform/win_util.h"
#include "render/d3d.h"
#include "sensors/sensor_hub.h"
#include "ui/hud.h"
#include "ui/settings_window.h"
#include "ui/theme.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "opengl32.lib")

namespace devtools {
namespace {

void Out(const char* fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    DWORD w = 0;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) WriteFile(h, buf, (DWORD)strlen(buf), &w, nullptr);
    logx::Info("%s", buf);
}

bool SavePng(const std::wstring& path, const std::vector<uint8_t>& rgba, int w, int h)
{
    IWICImagingFactory* f = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(w, h));
    if (ok) {
        // The PNG encoder takes BGRA natively; swap channels instead of hoping RGBA is accepted.
        std::vector<uint8_t> bgra(rgba);
        for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
             SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data())) &&
             SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (f) f->Release();
    return ok;
}

// Realistic sample data for screenshots.
SensorSnapshot SampleSensors()
{
    SensorSnapshot s;
    s.seq = 1;
    s.cpuName = "13th Gen Intel(R) Core(TM) i9-13900K";
    GpuAdapter a;
    a.name = "NVIDIA GeForce RTX 4080";
    a.key = "NVIDIA GeForce RTX 4080|pci:01:00.0";
    a.vendorId = 0x10DE;
    a.dedicatedBytes = 16ull << 30;
    s.adapters.push_back(a);
    s.activeAdapter = 0;
    s.gpu.load = 97.f;
    s.gpu.temp = 66.f;
    s.gpu.hotspot = 78.f;
    s.gpu.power = 287.f;
    s.gpu.coreClock = 2745.f;
    s.gpu.memClock = 11201.f;
    s.gpu.fanRpm = 1450.f;
    s.gpu.fanPercent = 42.f;
    s.gpu.vramUsedMB = 9420.f;
    s.gpu.vramTotalMB = 16376.f;
    s.cpu.load = 34.f;
    s.cpu.temp = 61.f;
    s.cpu.power = 98.f;
    s.cpu.clock = 5480.f;
    s.cpu.fanRpm = 1180.f;
    s.ram.usedGB = 17.8f;
    s.ram.totalGB = 31.7f;
    s.status.nvml = SourceState::Ok;
    s.status.pdh = SourceState::Ok;
    s.status.lhm = SourceState::Ok;
    s.status.pawnioInstalled = true;
    s.status.pawnioVersion = "2.0.1";
    s.status.cpuTempAvailable = true;
    s.status.tickMs = 4.2f;
    s.cpuTempChoices = { { "/intelcpu/0/temperature/26", "CPU Package" }, { "/intelcpu/0/temperature/0", "Core Max" } };
    s.cpuTempSensorUsed = "/intelcpu/0/temperature/26";
    return s;
}

std::vector<float> SampleGraph()
{
    std::vector<float> g;
    unsigned seed = 12345;
    for (int i = 0; i < 180; ++i) {
        seed = seed * 1103515245u + 12345u;
        const float noise = ((seed >> 16) & 0x7FFF) / 32767.f;
        float ms = 6.8f + 0.9f * std::sin(i * 0.21f) + noise * 0.8f;
        if (i == 61 || i == 132) ms = 19.5f;
        if (i == 133) ms = 11.f;
        g.push_back(ms);
    }
    return g;
}

int Shots(const std::wstring& dir, const std::string& language, float scale)
{
    CreateDirectoryW(dir.c_str(), nullptr);
    D3D d3d;
    std::string err;
    if (!d3d.Init(err)) {
        Out("D3D init failed: %s\n", err.c_str());
        return 2;
    }
    cfg::Config c;
    c.language = language;
    locale::Load(c.language.c_str());
    const int shotW = (int)std::lround(940 * scale), shotH = (int)std::lround(680 * scale);
    const SensorSnapshot sensors = SampleSensors();
    const std::vector<float> graph = SampleGraph();
    UiStatus st;
    st.elevated = true;
    st.captureRunning = true;
    st.hudVisible = true;
    st.target = "Cyberpunk 2077";
    st.liveFps = 143.6f;
    st.configPath = "C:\\Games\\FPS Overlay\\config.ini";
    st.updateCheckEnabled = APP_UPDATE_REPO[0] != '\0';

    int failures = 0;
    const char* pageNames[] = { "settings-overlay", "settings-appearance", "settings-sensors", "settings-hotkeys",
                                "settings-general", "settings-about" };
    for (int page = -1; page < 6; ++page) {
        SettingsWindow sw;
        std::vector<uint8_t> px;
        cfg::Config pc = c;
        pc.firstRunDone = page >= 0;
        if (!sw.RenderPageToImage(d3d, pc, page, shotW, shotH, st, sensors, px, scale)) { ++failures; continue; }
        const std::wstring name = page < 0 ? L"settings-welcome" : win::ToWide(pageNames[page]);
        if (!SavePng(dir + L"\\" + name + L".png", px, shotW, shotH)) ++failures;
    }

    HudFrameInfo fi;
    fi.captureRunning = true;
    fi.fresh = true;
    fi.stats.valid = true;
    fi.stats.fps = 143.6f;
    fi.stats.frameTimeMs = 6.96f;
    fi.stats.low1 = 112.f;
    fi.stats.low01 = 94.f;
    fi.graph = &graph;
    fi.target = "Cyberpunk 2077";
    fi.api = "DX12";

    Hud hud;
    if (!hud.Create(GetModuleHandleW(nullptr), d3d)) {
        Out("HUD create failed\n");
        return 3;
    }
    const float bg[4] = { 0.20f, 0.24f, 0.30f, 1.f };
    struct Variant { const wchar_t* name; cfg::Layout layout; int accent; bool all; };
    const Variant variants[] = {
        { L"hud-vertical", cfg::Layout::Vertical, 0, false },
        { L"hud-vertical-full", cfg::Layout::Vertical, 1, true },
        { L"hud-horizontal", cfg::Layout::Horizontal, 0, false },
        { L"hud-bar", cfg::Layout::Bar, 0, false },
    };
    for (const auto& v : variants) {
        cfg::Config hc = c;
        hc.layout = v.layout;
        hc.accent = v.accent;
        hc.scale = 140;
        if (v.all)
            for (int i = 0; i < (int)cfg::Metric::Count; ++i) hc.show[i] = true;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!hud.RenderToImage(hc, fi, sensors, bg, px, w, h) || !SavePng(dir + L"\\" + v.name + L".png", px, w, h)) ++failures;
    }
    hud.Destroy();
    d3d.Shutdown();
    Out("shots written to %s, failures: %d\n", win::ToUtf8(dir).c_str(), failures);
    return failures;
}

// HUD frames over a transparent background, 15 per simulated second, for animated documentation.
// A synthetic 144 FPS frame-time trace runs through the real frame statistics: the numbers refresh
// twice a second and the sensors once, as in the app, while the graph scrolls every frame.
int DemoFrames(const std::wstring& dir, int count, cfg::Layout layout, int scale, int accent, bool all)
{
    CreateDirectoryW(dir.c_str(), nullptr);
    D3D d3d;
    std::string err;
    if (!d3d.Init(err)) {
        Out("D3D init failed: %s\n", err.c_str());
        return 2;
    }
    locale::Load("en-US");
    cfg::Config c;
    c.layout = layout;
    c.scale = scale;
    c.accent = accent;
    if (all)
        for (int i = 0; i < (int)cfg::Metric::Count; ++i) c.show[i] = true;

    Hud hud;
    if (!hud.Create(GetModuleHandleW(nullptr), d3d)) {
        Out("HUD create failed\n");
        return 3;
    }

    unsigned seed = 2026;
    auto rnd = [&seed] {
        seed = seed * 1103515245u + 12345u;
        return ((seed >> 16) & 0x7FFF) / 32767.f;
    };
    FrameRing ring;
    double t = 0;
    int frameNo = 0;
    auto pushUntil = [&](double until) {
        while (t < until) {
            float ms = 6.75f + 0.25f * std::sin(frameNo * 0.031f) + 0.2f * std::sin(frameNo * 0.17f) + rnd() * 0.35f;
            if (frameNo % 307 == 150) ms = 15.5f + rnd() * 2.f;    // stutter
            else if (frameNo % 89 == 30) ms = 8.6f + rnd();         // small hitch
            t += ms / 1000.0;
            ring.Push(t, ms);
            ++frameNo;
        }
    };
    pushUntil(10.5);    // fill the lows window first

    SensorSnapshot s = SampleSensors();
    const SensorSnapshot base = s;
    std::vector<float> graph;
    HudFrameInfo fi;
    fi.captureRunning = true;
    fi.fresh = true;
    fi.graph = &graph;
    fi.target = "Cyberpunk 2077";
    fi.api = "DX12";

    const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
    const double start = t;
    int failures = 0;
    for (int f = 0; f < count; ++f) {
        pushUntil(start + f / 15.0);
        CopyRecentFrameTimes(ring, 180, graph);
        if (f % 8 == 0) fi.stats = ComputeFrameStats(ring, t, 1.0, 10.0);
        if (f % 15 == 0) {
            auto drift = [&](float& v, float center, float range) {
                v = std::clamp(v + (rnd() - 0.5f) * range * 0.6f, center - range, center + range);
            };
            drift(s.gpu.load, 97.f, 2.f);
            drift(s.gpu.temp, base.gpu.temp, 1.f);
            s.gpu.hotspot = s.gpu.temp + 12.f;
            drift(s.gpu.power, base.gpu.power, 9.f);
            drift(s.gpu.coreClock, base.gpu.coreClock, 15.f);
            drift(s.gpu.fanRpm, base.gpu.fanRpm, 15.f);
            drift(s.gpu.vramUsedMB, base.gpu.vramUsedMB, 60.f);
            drift(s.cpu.load, base.cpu.load, 5.f);
            drift(s.cpu.temp, base.cpu.temp, 2.f);
            drift(s.cpu.power, base.cpu.power, 8.f);
            drift(s.cpu.clock, base.cpu.clock, 40.f);
            drift(s.ram.usedGB, base.ram.usedGB, 0.08f);
            s.gpu.load = std::round(s.gpu.load);
            s.gpu.temp = std::round(s.gpu.temp);
            s.gpu.hotspot = std::round(s.gpu.hotspot);
            s.gpu.coreClock = std::round(s.gpu.coreClock / 15.f) * 15.f;
            s.cpu.temp = std::round(s.cpu.temp);
        }
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!hud.RenderToImage(c, fi, s, clear, px, w, h)) {
            ++failures;
            continue;
        }
        // The HUD renders premultiplied alpha (what DWM wants); PNG wants straight alpha.
        for (size_t i = 0; i + 3 < px.size(); i += 4) {
            const unsigned a = px[i + 3];
            if (a == 0 || a == 255) continue;
            for (int k = 0; k < 3; ++k) px[i + k] = (uint8_t)std::min(255u, (px[i + k] * 255u + a / 2) / a);
        }
        wchar_t name[32];
        swprintf_s(name, L"\\frame%03d.png", f);
        if (!SavePng(dir + name, px, w, h)) ++failures;
    }
    hud.Destroy();
    d3d.Shutdown();
    Out("%d demo frames written to %s, failures: %d\n", count, win::ToUtf8(dir).c_str(), failures);
    return failures;
}

int Probe()
{
    SensorHub hub;
    SensorRequest r;
    r.gpuHotspot = r.gpuPower = r.gpuClock = r.gpuMemClock = r.gpuFan = true;
    r.cpuPower = r.cpuClock = r.cpuFan = true;
    r.wantChoices = true;
    r.intervalMs = 500;
    hub.Start(r);
    for (int i = 0; i < 6; ++i) {
        Sleep(1000);
        const SensorSnapshot s = hub.Snapshot();
        Out("[%d] tick %.1f ms | GPU load %.0f temp %.0f hot %.1f power %.0f clk %.0f vram %.0f/%.0f | CPU load %.0f temp %.0f power %.1f clk %.0f | RAM %.1f/%.1f | lhm=%d nvml=%d pdh=%d\n",
            i, s.status.tickMs, s.gpu.load, s.gpu.temp, s.gpu.hotspot, s.gpu.power, s.gpu.coreClock, s.gpu.vramUsedMB,
            s.gpu.vramTotalMB, s.cpu.load, s.cpu.temp, s.cpu.power, s.cpu.clock, s.ram.usedGB, s.ram.totalGB,
            (int)s.status.lhm, (int)s.status.nvml, (int)s.status.pdh);
    }
    hub.Stop();
    return 0;
}

// ── Frame-rate test windows ─────────────────────────────────────────────────

LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

HWND MakeTestWindow(const wchar_t* title)
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TestWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FPSOverlay.RenderTest";
    wc.style = CS_OWNDC;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 80, 80, 640, 360,
                                nullptr, nullptr, wc.hInstance, nullptr);
    return hwnd;
}

// Paces frames with a high-resolution timer plus a short spin for accuracy.
struct Pacer {
    LARGE_INTEGER freq{}, next{};
    HANDLE timer = nullptr;
    double period = 0;
    explicit Pacer(double fps)
    {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&next);
        period = 1.0 / fps;
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    ~Pacer() { if (timer) CloseHandle(timer); }
    void Wait()
    {
        next.QuadPart += (LONGLONG)(period * freq.QuadPart);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double remain = (double)(next.QuadPart - now.QuadPart) / freq.QuadPart;
        if (remain > 0.002 && timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((remain - 0.0015) * 1e7);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart);
        if (now.QuadPart - next.QuadPart > (LONGLONG)(period * freq.QuadPart)) next = now;    // fell behind: resync
    }
};

bool Pump()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

int RenderTestD3D11(double fps, double seconds)
{
    HWND hwnd = MakeTestWindow(L"Render test (Direct3D 11)");
    D3D d3d;
    std::string err;
    if (!hwnd || !d3d.Init(err)) return 2;
    SwapTarget st;
    if (!st.Create(d3d, hwnd, 640, 360, SwapTarget::Mode::Flip)) return 3;
    Pacer pacer(fps);
    LARGE_INTEGER f, start, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&start);
    int frame = 0;
    for (;;) {
        if (!Pump()) break;
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - start.QuadPart) / (double)f.QuadPart > seconds) break;
        const float t = (frame++ % 120) / 120.f;
        const float c[4] = { 0.1f, 0.2f + 0.5f * t, 0.4f, 1.f };
        st.Bind(c);
        st.Present(false);
        pacer.Wait();
    }
    st.Destroy();
    d3d.Shutdown();
    DestroyWindow(hwnd);
    return 0;
}

int RenderTestOpenGL(double fps, double seconds)
{
    HWND hwnd = MakeTestWindow(L"Render test (OpenGL)");
    if (!hwnd) return 2;
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32 };
    pfd.cDepthBits = 24;
    const int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return 3;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) return 4;
    // Vsync off so the pacer alone decides the frame rate.
    using SwapIntervalFn = BOOL(WINAPI*)(int);
    if (auto si = reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT"))) si(0);
    Pacer pacer(fps);
    LARGE_INTEGER f, start, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&start);
    int frame = 0;
    for (;;) {
        if (!Pump()) break;
        QueryPerformanceCounter(&now);
        if ((now.QuadPart - start.QuadPart) / (double)f.QuadPart > seconds) break;
        const float t = (frame++ % 120) / 120.f;
        glClearColor(0.4f, 0.2f + 0.5f * t, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(dc);
        pacer.Wait();
    }
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
    ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    return 0;
}

// ── App icon ────────────────────────────────────────────────────────────────
// The icon is drawn by the same renderer as the UI (accent tile + frame-time trace), so the
// .ico in the repo can be regenerated any time: FPSOverlay.exe --make-icon icon.ico

bool EncodePng(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& png)
{
    IWICImagingFactory* f = nullptr;
    IStream* mem = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    std::vector<uint8_t> bgra(rgba);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(mem, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
              SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
              IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
              SUCCEEDED(frame->WritePixels(h, w * 4, (UINT)bgra.size(), bgra.data())) && SUCCEEDED(frame->Commit()) &&
              SUCCEEDED(enc->Commit());
    if (ok) {
        STATSTG st = {};
        LARGE_INTEGER zero = {};
        ok = SUCCEEDED(mem->Stat(&st, STATFLAG_NONAME)) && SUCCEEDED(mem->Seek(zero, STREAM_SEEK_SET, nullptr));
        if (ok) {
            png.resize((size_t)st.cbSize.QuadPart);
            ULONG read = 0;
            ok = SUCCEEDED(mem->Read(png.data(), (ULONG)png.size(), &read)) && read == png.size();
        }
    }
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (mem) mem->Release();
    if (f) f->Release();
    return ok;
}

bool RenderIcon(D3D& d3d, int size, std::vector<uint8_t>& rgba)
{
    ImGuiContext* prev = ImGui::GetCurrentContext();
    ImGuiContext* ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2((float)size, (float)size);
    io.DeltaTime = 1.f / 60.f;
    ImGui_ImplDX11_Init(d3d.Device(), d3d.Context());
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float s = (float)size;
    const float inset = size <= 16 ? 0.f : s * 0.04f;
    dl->AddRectFilled(ImVec2(inset, inset), ImVec2(s - inset, s - inset), theme::U32(theme::AccentAt(0).color), s * 0.22f);
    const ImVec2 pts[] = {
        { s * 0.17f, s * 0.60f }, { s * 0.37f, s * 0.60f }, { s * 0.50f, s * 0.26f },
        { s * 0.63f, s * 0.74f }, { s * 0.83f, s * 0.46f },
    };
    const float thick = std::max(1.6f, s * (size <= 24 ? 0.11f : 0.085f));
    // Separate segments with round joins and caps (a thick polyline would draw sharp miter spikes).
    for (int i = 0; i + 1 < 5; ++i) dl->AddLine(pts[i], pts[i + 1], IM_COL32(255, 255, 255, 255), thick);
    for (const ImVec2& p : pts) dl->AddCircleFilled(p, thick * 0.5f, IM_COL32(255, 255, 255, 255), 16);
    ImGui::Render();

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = size;
    td.Height = size;
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
        const float clear[4] = { 0, 0, 0, 0 };
        d3d.Context()->OMSetRenderTargets(1, &rtv, nullptr);
        d3d.Context()->ClearRenderTargetView(rtv, clear);
        D3D11_VIEWPORT vp = { 0, 0, s, s, 0, 1 };
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
            rgba.resize((size_t)size * size * 4);
            for (int r = 0; r < size; ++r)
                memcpy(&rgba[(size_t)r * size * 4], static_cast<const uint8_t*>(map.pData) + (size_t)r * map.RowPitch, (size_t)size * 4);
            d3d.Context()->Unmap(staging, 0);
            // Blending onto a transparent target leaves premultiplied color; PNG wants straight alpha.
            for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
                const int a = rgba[i + 3];
                if (a > 0 && a < 255)
                    for (int k = 0; k < 3; ++k) rgba[i + k] = (uint8_t)std::min(255, rgba[i + k] * 255 / a);
            }
        }
    }
    if (staging) staging->Release();
    if (rtv) rtv->Release();
    if (tex) tex->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext(ctx);
    ImGui::SetCurrentContext(prev);
    return ok;
}

int MakeIcon(const std::wstring& icoPath)
{
    D3D d3d;
    std::string err;
    if (!d3d.Init(err)) return 2;
    const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 96, 128, 256 };
    std::vector<std::vector<uint8_t>> pngs;
    for (int sz : sizes) {
        std::vector<uint8_t> rgba, png;
        if (!RenderIcon(d3d, sz, rgba) || !EncodePng(rgba, sz, sz, png)) return 3;
        pngs.push_back(std::move(png));
        if (sz == 256) {
            const std::wstring pngPath = icoPath.substr(0, icoPath.find_last_of(L'.')) + L".png";
            SavePng(pngPath, rgba, sz, sz);
        }
    }
    d3d.Shutdown();

    // ICO with PNG-compressed entries (supported since Windows Vista).
    std::vector<uint8_t> ico;
    auto u16 = [&](uint16_t v) { ico.push_back((uint8_t)v); ico.push_back((uint8_t)(v >> 8)); };
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) ico.push_back((uint8_t)(v >> (8 * i))); };
    const int n = (int)pngs.size();
    u16(0); u16(1); u16((uint16_t)n);
    uint32_t offset = 6 + 16 * n;
    for (int i = 0; i < n; ++i) {
        ico.push_back((uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.push_back((uint8_t)(sizes[i] >= 256 ? 0 : sizes[i]));
        ico.push_back(0);
        ico.push_back(0);
        u16(1);
        u16(32);
        u32((uint32_t)pngs[i].size());
        u32(offset);
        offset += (uint32_t)pngs[i].size();
    }
    for (const auto& p : pngs) ico.insert(ico.end(), p.begin(), p.end());
    HANDLE h = CreateFileW(icoPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 4;
    DWORD w = 0;
    WriteFile(h, ico.data(), (DWORD)ico.size(), &w, nullptr);
    CloseHandle(h);
    Out("icon written: %s (%d sizes)\n", win::ToUtf8(icoPath).c_str(), n);
    return 0;
}

int SelfTest()
{
    // ETW needs administrator rights or membership in "Performance Log Users".
    FrameCapture cap;
    std::string err;
    if (!cap.Start(err)) {
        Out("selftest: capture failed: %s\n", err.c_str());
        return 2;
    }
    const double started = cap.Now();
    struct Case { const wchar_t* api; double fps; };
    const Case cases[] = { { L"d3d11", 100 }, { L"d3d11", 237 }, { L"opengl", 60 }, { L"opengl", 144 } };
    int failures = 0;
    const std::wstring exe = win::ExePath();
    for (const Case& c : cases) {
        wchar_t args[512];
        swprintf_s(args, L"\"%s\" --render-test %s %.0f 7", exe.c_str(), c.api, c.fps);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, args, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            Out("  %ls %.0f: could not start\n", c.api, c.fps);
            ++failures;
            continue;
        }
        Sleep(5000);
        const FrameCapture::Result r = cap.Query(pi.dwProcessId, 2.0, 4.0);
        const double deviation = r.stats.valid ? std::fabs(r.stats.fps - c.fps) / c.fps : 1.0;
        const bool pass = r.stats.valid && deviation < 0.04;
        Out("  %-6ls target %5.0f -> measured %7.2f fps, frame time %6.2f ms, 1%% low %7.2f, source %s  %s\n", c.api, c.fps,
            r.stats.fps, r.stats.frameTimeMs, r.stats.low1, PresentSourceName(r.source), pass ? "PASS" : "FAIL");
        if (!pass) ++failures;
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    Out("selftest: %d failure(s), %llu ETW events (%.0f per second)\n", failures, (unsigned long long)cap.EventsSeen(),
        cap.EventsSeen() / std::max(1.0, cap.Now() - started));
    cap.Stop();
    return failures;
}

} // namespace

int Run(int argc, wchar_t** argv, const std::wstring&)
{
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--shots") {
            // --shots <dir> [--lang <code>] [--scale <factor>]
            std::wstring dir = win::ExeDir() + L"shots";
            std::string lang = "en-US";
            float scale = 1.f;
            for (int k = i + 1; k < argc; ++k) {
                if (_wcsicmp(argv[k], L"--lang") == 0 && k + 1 < argc) lang = win::ToUtf8(argv[++k]);
                else if (_wcsicmp(argv[k], L"--scale") == 0 && k + 1 < argc) scale = (float)_wtof(argv[++k]);
                else if (argv[k][0] != L'-') dir = argv[k];
            }
            if (scale < 0.5f || scale > 4.f) scale = 1.f;
            return Shots(dir, lang, scale);
        }
        if (a == L"--demo-frames") {
            // --demo-frames <dir> [--count N] [--layout vertical|horizontal|bar] [--scale pct] [--accent n] [--all]
            std::wstring dir = win::ExeDir() + L"demo";
            int count = 60, scale = 100, accent = 0;
            bool all = false;
            cfg::Layout layout = cfg::Layout::Vertical;
            for (int k = i + 1; k < argc; ++k) {
                if (_wcsicmp(argv[k], L"--count") == 0 && k + 1 < argc) count = _wtoi(argv[++k]);
                else if (_wcsicmp(argv[k], L"--scale") == 0 && k + 1 < argc) scale = _wtoi(argv[++k]);
                else if (_wcsicmp(argv[k], L"--accent") == 0 && k + 1 < argc) accent = _wtoi(argv[++k]);
                else if (_wcsicmp(argv[k], L"--all") == 0) all = true;
                else if (_wcsicmp(argv[k], L"--layout") == 0 && k + 1 < argc) {
                    ++k;
                    if (_wcsicmp(argv[k], L"horizontal") == 0) layout = cfg::Layout::Horizontal;
                    else if (_wcsicmp(argv[k], L"bar") == 0) layout = cfg::Layout::Bar;
                } else if (argv[k][0] != L'-') dir = argv[k];
            }
            return DemoFrames(dir, std::clamp(count, 1, 900), layout, std::clamp(scale, 50, 250), std::clamp(accent, 0, 7), all);
        }
        if (a == L"--probe") return Probe();
        if (a == L"--make-icon" && i + 1 < argc) return MakeIcon(argv[i + 1]);
        if (a == L"--selftest") return SelfTest();
        if (a == L"--render-test" && i + 3 < argc) {
            const double fps = _wtof(argv[i + 2]);
            const double secs = _wtof(argv[i + 3]);
            if (fps <= 0 || secs <= 0) return 2;
            return _wcsicmp(argv[i + 1], L"opengl") == 0 ? RenderTestOpenGL(fps, secs) : RenderTestD3D11(fps, secs);
        }
    }
    return kNotHandled;
}

} // namespace devtools
