// The in-game HUD: a small topmost, click-through, per-pixel transparent window sized to its
// content. Hold Ctrl over it to drag it (with edge snapping) or right-click for the menu.
#pragma once

#include "app/config.h"
#include "capture/frame_stats.h"
#include "render/d3d.h"
#include "platform/win_util.h"
#include "sensors/snapshot.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct ImGuiContext;
struct ImFont;

struct HudFrameInfo {
    bool captureRunning = false;
    bool fresh = false;                 // frames arrived recently
    FrameStats stats;
    const std::vector<float>* graph = nullptr;
    std::string target;                 // game name
    std::string api;                    // "DX12"
};

class Hud {
public:
    bool Create(HINSTANCE inst, D3D& d3d);
    void Destroy();

    // Fades in or out over a fraction of a second; the window hides once the fade-out ends.
    void SetVisible(bool visible);
    bool Visible() const { return visible_; }
    bool Active() const { return visible_ || shown_; }     // wanted, or still fading out
    bool Fading() const { return visible_ ? fade_ < 1.f : shown_; }
    // The monitor the HUD goes on, known even while it is hidden.
    HMONITOR Monitor(const cfg::Config& cfg)
    {
        RECT rc;
        return ResolveMonitor(cfg, rc) ? monitor_ : nullptr;
    }
    // The middle of where the HUD goes, screen pixels, known even while it is hidden.
    POINT Spot(const cfg::Config& cfg);

    // Renders one frame when due (cfg.hudFps). `force` refreshes the numbers immediately.
    void Tick(const cfg::Config& cfg, const HudFrameInfo& frames, const SensorSnapshot& sensors, bool force = false);

    // Renders the HUD offscreen over a solid background (documentation screenshots, tests).
    bool RenderToImage(const cfg::Config& cfg, const HudFrameInfo& frames, const SensorSnapshot& sensors,
                       const float background[4], std::vector<uint8_t>& rgba, int& w, int& h);

    HWND Hwnd() const { return hwnd_; }
    bool Interactive() const { return interactive_; }

    // Fired from the window procedure.
    std::function<void(POINT screen)> onMenu;               // Ctrl + right click
    std::function<void(int left, int top)> onMoved;         // finished a Ctrl + drag (content top-left, screen px)
    std::function<void()> onModalTick;                      // during a drag Windows' move loop blocks the app's loop

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    void EnsureFonts(float pixelSize);
    void BuildFrame(const cfg::Config& cfg, const HudFrameInfo& frames, const SensorSnapshot& sensors,
                    float dpiScale, bool glow);
    bool ResolveMonitor(const cfg::Config& cfg, RECT& monitorRect);
    bool ContentOrigin(const cfg::Config& cfg, int contentW, int contentH, POINT& topLeft);    // content, not window
    void Place(const cfg::Config& cfg, int contentW, int contentH);
    void UpdateInteractive();
    void SetClickThrough(bool on);

    HWND hwnd_ = nullptr;
    D3D* d3d_ = nullptr;
    SwapTarget target_;
    ImGuiContext* ctx_ = nullptr;
    ImFont* fontBody_ = nullptr;
    ImFont* fontStrong_ = nullptr;

    bool visible_ = false;
    bool shown_ = false;
    float fade_ = 0.f;          // 0 hidden .. 1 fully shown
    bool interactive_ = false;
    bool dragging_ = false;
    bool clickThrough_ = true;
    int  affinity_ = -1;

    LONGLONG lastFrameQpc_ = 0;
    LONGLONG lastStatsQpc_ = 0;
    LONGLONG lastTopmostQpc_ = 0;
    double qpcFreq_ = 1.0;

    // Values held between stats refreshes, so numbers change at a readable pace.
    FrameStats shownStats_;
    bool shownFresh_ = false;
    SensorSnapshot shownSensors_;
    float graphScale_ = 0.f;

    int contentW_ = 0, contentH_ = 0;
    int pad_ = 8;
    float dpiScale_ = 1.f;
    HMONITOR monitor_ = nullptr;
    win::MonitorInfo monCache_[16];
    int monCount_ = 0;
    ULONGLONG monCacheTick_ = 0;
};
