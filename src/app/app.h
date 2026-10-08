// Owns every part of the program and runs the main loop.
#pragma once

#include "app/config.h"
#include "app/game_visibility.h"
#include "capture/capture_watchdog.h"
#include "capture/frame_capture.h"
#include "capture/target_tracker.h"
#include "render/d3d.h"
#include "sensors/sensor_hub.h"
#include "ui/hud.h"
#include "ui/settings_window.h"

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <thread>
#include <string>
#include <vector>

class App {
public:
    int Run(HINSTANCE inst, int argc, wchar_t** argv);

private:
    bool Init(bool startHidden);
    void Shutdown();
    void Loop();
    void Frame(bool settingsVblank = false);

    void OpenSettings();
    void SetHudVisible(bool visible);
    void ResetStats();
    void ApplyLanguage();
    void SaveSoon();
    void SaveNow();
    SensorRequest BuildSensorRequest() const;
    bool GameShown(ULONGLONG now, const FrameCapture::Result& frames);
    void CheckCapture(ULONGLONG now);
    void HandleUiActions(const UiActions& a);
    void RefreshAutostartAsync();

    // Message-only window: tray icon, single-instance pings, hotkey events.
    static LRESULT CALLBACK MsgProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMsg(UINT, WPARAM, LPARAM);
    void AddTrayIcon();
    void RemoveTrayIcon();
    void TrayMessage(const std::string& text);
    void TickUpdates(ULONGLONG now, bool gameRunning);
    void ShowMenu(POINT at, bool fromHud);
    void PollHotkeys();

    HINSTANCE inst_ = nullptr;
    HWND msgWnd_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    bool trayAdded_ = false;
    UINT taskbarCreated_ = 0;
    HANDLE frameTimer_ = nullptr;

    cfg::Config cfg_;
    std::wstring configPath_;
    D3D d3d_;
    Hud hud_;
    SettingsWindow settings_;
    FrameCapture capture_;
    TargetTracker tracker_;
    SensorHub sensors_;

    std::string captureError_;
    bool running_ = true;
    bool hudWanted_ = true;         // user toggle
    bool elevated_ = false;
    ULONGLONG saveDue_ = 0;
    ULONGLONG lastTargetUpdate_ = 0;
    CaptureWatchdog captureWatchdog_;
    ULONGLONG lastCaptureCheck_ = 0;
    GameVisibility gameVis_;        // "Hide when no game is running"
    int gameLogState_ = -1;         // last logged inputs of that decision
    DWORD gameLogPid_ = 0;
    bool hotkeyDown_[(int)cfg::HotkeyAction::Count] = {};
    std::vector<float> graph_;
    FrameCapture::Result frames_;
    ULONGLONG lastFramesQuery_ = 0;
    SensorSnapshot snapshot_;
    uint64_t snapshotSeq_ = ~0ull;

    bool justUpdated_ = false;             // started by the installer of a new version
    bool updateFailed_ = false;            // started by an installer that had to roll back
    bool installRequested_ = false;        // "Install now": install as soon as the download is ready
    ULONGLONG startTick_ = 0;
    ULONGLONG lastGameFrame_ = 0;          // last time a game was drawing frames
    ULONGLONG lastUpdateCheck_ = 0;

    std::atomic<int> autostart_{ -1 };     // -1 unknown, 0 off, 1 on
    std::atomic<bool> autostartBusy_{ false };
    std::thread autostartThread_;          // schtasks runs here; joined before shutdown
};
