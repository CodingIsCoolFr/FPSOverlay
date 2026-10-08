// The settings window. Every change applies live to the running HUD; the app saves the
// config shortly after the last change.
#pragma once

#include "app/config.h"
#include "app/updater.h"
#include "render/d3d.h"
#include "sensors/snapshot.h"

#include <windows.h>

#include <functional>
#include <string>

struct ImGuiContext;
struct ImFont;

// Read-only state the window displays.
struct UiStatus {
    bool elevated = false;
    bool captureRunning = false;
    std::string captureError;
    bool hudVisible = true;
    std::string target;
    float liveFps = 0.f;            // 0 = none
    bool autostart = false;
    updater::Status update;         // update check, download and install state
    std::string configPath;
};

// Requests from the window, consumed (and cleared) by the app each tick.
struct UiActions {
    bool toggleHud = false;
    bool installPawnIo = false;
    bool resetPosition = false;
    bool quit = false;
    bool openConfigFolder = false;
    bool checkUpdates = false;
    bool openUpdatePage = false;
    bool installUpdate = false;     // download if needed, then install now
    int  setAutostart = -1;         // -1 none, 0 off, 1 on
    bool languageChanged = false;
};

class SettingsWindow {
public:
    bool Open(HINSTANCE inst, D3D& d3d, cfg::Config& config);
    void Close();
    bool IsOpen() const { return hwnd_ != nullptr; }
    HWND Hwnd() const { return hwnd_; }

    // Renders a frame when due. Returns true if the config was edited this frame.
    // vblank: the frame waitable fired (the display can take a frame now).
    bool Tick(const UiStatus& status, const SensorSnapshot& sensors, bool vblank = false);

    // While the user scrolls, moves the mouse or types (and for a second after), and while a
    // scroll is still gliding, the window renders once per display refresh, paced by
    // FrameWaitable(). Otherwise it renders 30 times a second in front, 15 behind other windows.
    bool WantsFastFrames() const;
    HANDLE FrameWaitable() const { return target_.FrameWaitable(); }

    // While the window is dragged or resized, Windows' own move loop blocks the app's loop;
    // this runs the app's frame from a timer meanwhile.
    std::function<void()> onModalTick;

    UiActions TakeActions();
    bool CapturingHotkey() const { return capturing_ >= 0; }
    void ReloadFonts() { fontsDirty_ = true; }

    // Offscreen render of one page for screenshots. Window need not be open.
    bool RenderPageToImage(D3D& d3d, cfg::Config& config, int page, int width, int height, const UiStatus& status,
                           const SensorSnapshot& sensors, std::vector<uint8_t>& rgba, float dpiScale = 1.f);

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT, WPARAM, LPARAM);

    void CreateContext(D3D& d3d, bool withWin32);
    void DestroyContext();
    void ApplyStyle();
    void Draw(const UiStatus& status, const SensorSnapshot& sensors);
    void DrawHeader(const UiStatus& status, float height);
    void DrawSidebar(float width, float height);
    void DrawPage(const UiStatus& status, const SensorSnapshot& sensors);
    void DrawFooter(const UiStatus& status, float height);
    void PageMetrics(const SensorSnapshot& sensors);
    void PageAppearance(const SensorSnapshot& sensors);
    void PageSensors(const UiStatus& status, const SensorSnapshot& sensors);
    void PageHotkeys();
    void PageGeneral(const UiStatus& status);
    void PageAbout(const UiStatus& status, const SensorSnapshot& sensors);
    void DrawWelcome(const SensorSnapshot& sensors);
    void SmoothScroll(float em);

    HWND hwnd_ = nullptr;
    D3D* d3d_ = nullptr;
    SwapTarget target_;
    ImGuiContext* ctx_ = nullptr;
    cfg::Config* cfg_ = nullptr;
    std::string lastSerialized_;
    UiActions actions_;
    int capturing_ = -1;            // hotkey slot waiting for a key press
    float dpiScale_ = 1.f;
    bool fontsDirty_ = true;
    bool styleDirty_ = true;
    LONGLONG lastFrameQpc_ = 0;
    LONGLONG lastInputQpc_ = 0;
    UINT lastInputMsg_ = 0;         // diagnostics (test builds)
    LPARAM lastMouseLp_ = -1;       // last WM_MOUSEMOVE position
    double qpcFreq_ = 1.0;
    bool inputSinceCheck_ = true;   // config can only change through input: compare it then
    int framesSinceCheck_ = 0;
    float scrollTarget_ = 0.f;      // smooth wheel scrolling of the page
    float scrollPos_ = 0.f;         // gliding position (float; ImGui keeps whole pixels)
    bool scrollAnimating_ = false;
    bool minimized_ = false;
    bool welcomeOpen_ = false;
};
