#include "app/app.h"
#include "app/devtools.h"
#include "app/log.h"
#include "app/updater.h"
#include "app/version.h"
#include "locale/locale.h"
#include "platform/shell.h"
#include "platform/win_util.h"
#include "ui/theme.h"

#include <commctrl.h>
#include <objbase.h>

#include <algorithm>
#include <thread>

namespace {

constexpr wchar_t kMainClass[] = L"FPSOverlay.Main";
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT kTrayId = 1;

enum MenuId : UINT {
    IdSettings = 100, IdToggleHud, IdResetStats, IdSnapCorner, IdQuit,
};

UINT ShowSettingsMessage()
{
    static const UINT msg = RegisterWindowMessageW(L"FPSOverlay.ShowSettings");
    return msg;
}

} // namespace

// ── Entry ───────────────────────────────────────────────────────────────────

int App::Run(HINSTANCE inst, int argc, wchar_t** argv)
{
    inst_ = inst;
    // The second half of an update: this exe was started from the update folder.
    if (const int rc = updater::FinishUpdateIfAsked(argc, argv); rc >= 0) return rc;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool startHidden = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--tray") == 0) startHidden = true;
        if (_wcsicmp(argv[i], L"--updated") == 0) justUpdated_ = true;
        if (_wcsicmp(argv[i], L"--update-failed") == 0) updateFailed_ = true;
    }

    configPath_ = win::DataDir() + L"config.ini";
    logx::Init(win::DataDir() + L"FPSOverlay.log");
    logx::Info("%s %s starting (elevated=%d)", APP_NAME, APP_VERSION, win::IsElevated());

    // Developer / diagnostic modes run without the tray app.
    if (const int dev = devtools::Run(argc, argv, configPath_); dev != devtools::kNotHandled) {
        logx::Shutdown();
        CoUninitialize();
        return dev;
    }

    // One instance: a second start just opens the settings of the first.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\FPSOverlay.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kMainClass, nullptr)) PostMessageW(other, ShowSettingsMessage(), 0, 0);
        logx::Info("Already running; asked the other instance to open settings");
        if (mutex) CloseHandle(mutex);
        logx::Shutdown();
        CoUninitialize();
        return 0;
    }

    cfg::LoadFile(configPath_, cfg_);
    locale::Load(cfg_.language.c_str());

    int code = 0;
    if (Init(startHidden)) Loop();
    else code = 1;
    Shutdown();

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    logx::Info("Exited");
    logx::Shutdown();
    CoUninitialize();
    return code;
}

bool App::Init(bool startHidden)
{
    elevated_ = win::IsElevated();
    win::EnableDarkMenus();

    std::string err;
    if (!d3d_.Init(err)) {
        MessageBoxW(nullptr, win::ToWide(err).c_str(), APP_NAME_W, MB_OK | MB_ICONERROR);
        return false;
    }

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = &App::MsgProc;
    wc.hInstance = inst_;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only: those miss the TaskbarCreated broadcast).
    msgWnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, APP_NAME_W, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst_, this);
    if (!msgWnd_) return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    // Let the second instance (same user, maybe started unelevated by the shell) reach us.
    ChangeWindowMessageFilterEx(msgWnd_, ShowSettingsMessage(), MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(msgWnd_, taskbarCreated_, MSGFLT_ALLOW, nullptr);

    if (!hud_.Create(inst_, d3d_)) {
        MessageBoxW(nullptr, L"The overlay window could not be created.", APP_NAME_W, MB_OK | MB_ICONERROR);
        return false;
    }
    hud_.onMenu = [this](POINT pt) { ShowMenu(pt, true); };
    hud_.onMoved = [this](int left, int top) {
        POINT pt = { left, top };
        HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfoW(mon, &mi)) return;
        cfg_.monitor = win::ToUtf8(mi.szDevice);
        cfg_.customPos = true;
        cfg_.posX = left - mi.rcMonitor.left;
        cfg_.posY = top - mi.rcMonitor.top;
        SaveSoon();
    };

    if (!capture_.Start(captureError_)) logx::Warn("Frame capture unavailable: %s", captureError_.c_str());
    sensors_.Start(BuildSensorRequest());

    frameTimer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!frameTimer_) frameTimer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    AddTrayIcon();
    hud_.SetVisible(true);
    PollHotkeys();      // swallow key presses that happened before we started
    if (!startHidden || !cfg_.firstRunDone) OpenSettings();
    RefreshAutostartAsync();
    startTick_ = lastUpdateCheck_ = GetTickCount64();
    updater::CheckAsync(cfg_.autoUpdate);
    if (justUpdated_) TrayMessage(locale::TF("Updated to version %s", APP_VERSION));
    return true;
}

void App::Shutdown()
{
    SaveNow();
    if (autostartThread_.joinable()) autostartThread_.join();
    sensors_.Stop();
    capture_.Stop();
    settings_.Close();
    hud_.Destroy();
    RemoveTrayIcon();
    if (frameTimer_) CloseHandle(frameTimer_);
    frameTimer_ = nullptr;
    if (msgWnd_) DestroyWindow(msgWnd_);
    msgWnd_ = nullptr;
    d3d_.Shutdown();
}

// ── Main loop ───────────────────────────────────────────────────────────────

void App::Loop()
{
    while (running_) {
        // Wake for input or for the next frame, whichever comes first.
        int waitMs = 1000 / std::max(1, cfg_.hudFps);
        if (settings_.IsOpen() && GetForegroundWindow() == settings_.Hwnd()) waitMs = std::min(waitMs, 16);
        if (!hud_.Visible() && !settings_.IsOpen()) waitMs = 50;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)waitMs * 10000;
        SetWaitableTimer(frameTimer_, &due, 0, nullptr, nullptr, FALSE);
        MsgWaitForMultipleObjectsEx(1, &frameTimer_, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running_ = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running_) break;
        Frame();
    }
}

void App::Frame()
{
    const ULONGLONG now = GetTickCount64();
    if (!settings_.CapturingHotkey()) PollHotkeys();

    if (now - lastTargetUpdate_ >= 250) {
        lastTargetUpdate_ = now;
        tracker_.Update(capture_);
    }

    const uint64_t seq = sensors_.Seq();
    if (seq != snapshotSeq_) {
        snapshotSeq_ = seq;
        snapshot_ = sensors_.Snapshot();
    }

    const double avgWindow = std::max(1.0, cfg_.statsIntervalMs / 1000.0);
    const FrameCapture::Result fr = capture_.Query(tracker_.Pid(), avgWindow, (double)cfg_.lowsWindowSec, &graph_, 180);
    tracker_.Refine(fr.source);
    const bool fresh = fr.stats.valid && fr.secondsSinceLastFrame < 1.5;

    HudFrameInfo info;
    info.captureRunning = capture_.Running();
    info.fresh = fresh;
    info.stats = fr.stats;
    info.graph = &graph_;
    info.target = tracker_.DisplayName();
    info.api = tracker_.ApiLabel();

    hud_.SetVisible(hudWanted_ && !(cfg_.hideWhenIdle && !fresh));
    theme::SetAccent(cfg_.accent);
    hud_.Tick(cfg_, info, snapshot_);

    TickUpdates(now, fresh);
    if (!running_) return;

    if (settings_.IsOpen()) {
        UiStatus st;
        st.elevated = elevated_;
        st.captureRunning = capture_.Running();
        st.captureError = captureError_;
        st.hudVisible = hudWanted_;
        st.target = tracker_.DisplayName();
        st.liveFps = fresh ? fr.stats.fps : 0.f;
        st.autostart = autostart_.load() == 1;
        st.update = updater::GetStatus();
        st.configPath = win::ToUtf8(configPath_);
        if (settings_.Tick(st, snapshot_)) {
            SaveSoon();
            PollHotkeys();      // new bindings start clean
        }
        HandleUiActions(settings_.TakeActions());
    }

    sensors_.SetRequest(BuildSensorRequest());

    if (saveDue_ && now >= saveDue_) SaveNow();
}

SensorRequest App::BuildSensorRequest() const
{
    SensorRequest r;
    auto on = [this](cfg::Metric m) { return cfg_.show[(int)m]; };
    const bool all = settings_.IsOpen();    // settings show live values for every row
    r.gpuLoad = all || on(cfg::Metric::GpuLoad);
    r.gpuTemp = all || on(cfg::Metric::GpuTemp);
    r.gpuHotspot = all || on(cfg::Metric::GpuHotspot);
    r.gpuPower = all || on(cfg::Metric::GpuPower);
    r.gpuClock = all || on(cfg::Metric::GpuClock);
    r.gpuMemClock = all || on(cfg::Metric::GpuMemClock);
    r.gpuFan = all || on(cfg::Metric::GpuFan);
    r.vram = all || on(cfg::Metric::Vram);
    r.cpuLoad = all || on(cfg::Metric::CpuLoad);
    r.cpuTemp = all || on(cfg::Metric::CpuTemp);
    r.cpuPower = all || on(cfg::Metric::CpuPower);
    r.cpuClock = all || on(cfg::Metric::CpuClock);
    r.cpuFan = all || on(cfg::Metric::CpuFan);
    r.ram = all || on(cfg::Metric::Ram);
    r.wantChoices = all;
    r.gpuKey = cfg_.gpu;
    r.cpuTempPref = cfg_.cpuTempSensor;
    r.cpuFanPref = cfg_.cpuFanSensor;
    r.intervalMs = cfg_.sensorIntervalMs;
    return r;
}

void App::HandleUiActions(const UiActions& a)
{
    if (a.toggleHud) SetHudVisible(!hudWanted_);
    if (a.installPawnIo) sensors_.InstallPawnIo();
    if (a.resetPosition) {
        cfg_.customPos = false;
        SaveSoon();
    }
    if (a.quit) running_ = false;
    if (a.openConfigFolder) shell::RevealInExplorer(configPath_);
    if (a.checkUpdates) updater::CheckAsync(cfg_.autoUpdate);
    if (a.openUpdatePage) shell::OpenUnelevated(updater::ReleasePageUrl());
    if (a.installUpdate) {
        installRequested_ = true;
        if (updater::GetStatus().state != updater::State::Ready) updater::CheckAsync(true);
    }
    if (a.languageChanged) ApplyLanguage();
    if (a.setAutostart >= 0 && !autostartBusy_.exchange(true)) {
        const bool enable = a.setAutostart == 1;
        autostart_ = enable ? 1 : 0;    // optimistic; corrected below
        if (autostartThread_.joinable()) autostartThread_.join();
        autostartThread_ = std::thread([this, enable] {
            std::string err;
            const bool ok = enable ? autostart::Enable(win::ExePath(), err) : autostart::Disable(err);
            if (!ok) logx::Warn("Autostart: %s", err.c_str());
            autostart_ = autostart::IsEnabled() ? 1 : 0;
            autostartBusy_ = false;
        });
    }
}

void App::RefreshAutostartAsync()
{
    if (autostartBusy_.exchange(true)) return;
    if (autostartThread_.joinable()) autostartThread_.join();
    autostartThread_ = std::thread([this] {
        autostart_ = autostart::IsEnabled() ? 1 : 0;
        autostartBusy_ = false;
    });
}

void App::OpenSettings()
{
    settings_.Open(inst_, d3d_, cfg_);
}

void App::SetHudVisible(bool visible)
{
    hudWanted_ = visible;
    hud_.SetVisible(visible);
}

void App::ResetStats()
{
    if (tracker_.Pid()) capture_.ResetHistory(tracker_.Pid());
}

void App::ApplyLanguage()
{
    locale::Load(cfg_.language.c_str());
    if (trayAdded_) {
        wcsncpy_s(nid_.szTip, APP_NAME_W, _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid_);
    }
    if (settings_.IsOpen()) {
        const std::wstring title = std::wstring(APP_NAME_W) + L" \x2014 " + win::ToWide(locale::T("Settings"));
        SetWindowTextW(settings_.Hwnd(), title.c_str());
    }
    SaveSoon();
}

void App::SaveSoon()
{
    saveDue_ = GetTickCount64() + 600;
}

void App::SaveNow()
{
    saveDue_ = 0;
    if (!cfg::SaveFile(configPath_, cfg_)) logx::Warn("Could not save %s", win::ToUtf8(configPath_).c_str());
}

// ── Hotkeys ─────────────────────────────────────────────────────────────────
// Polled instead of RegisterHotKey on purpose: RegisterHotKey swallows the key system-wide, so a
// plain "Insert" binding would stop working in every other app. Polling leaves the key alone.

void App::PollHotkeys()
{
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const UINT mods = (down(VK_CONTROL) ? MOD_CONTROL : 0) | (down(VK_MENU) ? MOD_ALT : 0) |
                      (down(VK_SHIFT) ? MOD_SHIFT : 0) | ((down(VK_LWIN) || down(VK_RWIN)) ? MOD_WIN : 0);
    for (int i = 0; i < (int)cfg::HotkeyAction::Count; ++i) {
        const cfg::Hotkey& hk = cfg_.hotkeys[i];
        if (!hk.vk) {
            hotkeyDown_[i] = false;
            continue;
        }
        const SHORT state = GetAsyncKeyState((int)hk.vk);
        const bool isDown = (state & 0x8000) != 0;
        // Edge, or a tap that started and ended between two polls.
        const bool pressed = (isDown && !hotkeyDown_[i]) || (!isDown && (state & 1) && !hotkeyDown_[i]);
        hotkeyDown_[i] = isDown;
        if (!pressed || mods != hk.mods) continue;

        switch ((cfg::HotkeyAction)i) {
            case cfg::HotkeyAction::ToggleHud:    SetHudVisible(!hudWanted_); break;
            case cfg::HotkeyAction::ResetStats:   ResetStats(); break;
            case cfg::HotkeyAction::OpenSettings: OpenSettings(); break;
            case cfg::HotkeyAction::Exit:         running_ = false; break;
            default: break;
        }
    }
}

// ── Tray and menus ──────────────────────────────────────────────────────────

void App::AddTrayIcon()
{
    nid_ = {};
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = msgWnd_;
    nid_.uID = kTrayId;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAY;
    if (FAILED(LoadIconMetric(inst_, MAKEINTRESOURCEW(1), LIM_SMALL, &nid_.hIcon)))
        nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(nid_.szTip, APP_NAME_W, _TRUNCATE);
    trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid_) != FALSE;
}

void App::TrayMessage(const std::string& text)
{
    if (!trayAdded_) return;
    NOTIFYICONDATAW n = nid_;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, APP_NAME_W, _TRUNCATE);
    wcsncpy_s(n.szInfo, win::ToWide(text).c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_USER;
    n.hBalloonIcon = nid_.hIcon;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// Checks for a new release every 6 hours, downloads it in the background and installs it when
// no game has drawn a frame for 10 seconds and the settings window is closed. "Install now"
// installs as soon as the download is ready.
void App::TickUpdates(ULONGLONG now, bool gameRunning)
{
    if (!updater::Enabled()) return;
    if (gameRunning) lastGameFrame_ = now;
    if (now - lastUpdateCheck_ >= 6ull * 3600 * 1000) {
        lastUpdateCheck_ = now;
        updater::CheckAsync(cfg_.autoUpdate);
    }
    const updater::Status st = updater::GetStatus();
    if (st.state == updater::State::Available && cfg_.autoUpdate) updater::CheckAsync(true);
    if (st.state == updater::State::Failed) installRequested_ = false;
    if (st.state != updater::State::Ready) return;

    // After a failed install (a file was in use) only "Install now" retries in this session, so a
    // stuck file cannot cause a restart loop.
    const bool idle = cfg_.autoUpdate && !updateFailed_ && !settings_.IsOpen() && now - startTick_ >= 15000 &&
                      now - lastGameFrame_ >= 10000;
    if (!installRequested_ && !idle) return;
    std::string err;
    if (updater::LaunchInstaller(!settings_.IsOpen(), err)) {
        SaveNow();
        running_ = false;
    } else {
        logx::Warn("Update: %s", err.c_str());
        installRequested_ = false;
    }
}

void App::RemoveTrayIcon()
{
    if (trayAdded_) Shell_NotifyIconW(NIM_DELETE, &nid_);
    trayAdded_ = false;
    if (nid_.hIcon) DestroyIcon(nid_.hIcon);
    nid_.hIcon = nullptr;
}

void App::ShowMenu(POINT at, bool fromHud)
{
    HMENU m = CreatePopupMenu();
    auto add = [&](UINT id, const char* text, bool enabled = true) {
        AppendMenuW(m, MF_STRING | (enabled ? 0 : MF_GRAYED), id, win::ToWide(locale::T(text)).c_str());
    };
    add(IdSettings, "Settings");
    add(IdToggleHud, hudWanted_ ? "Hide overlay" : "Show overlay");
    add(IdResetStats, "Reset 1% lows", tracker_.Pid() != 0);
    if (fromHud) add(IdSnapCorner, "Snap back to corner", cfg_.customPos);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    add(IdQuit, "Quit");
    SetMenuDefaultItem(m, IdSettings, FALSE);

    SetForegroundWindow(msgWnd_);
    const UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, 0, msgWnd_, nullptr);
    PostMessageW(msgWnd_, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
        case IdSettings:   OpenSettings(); break;
        case IdToggleHud:  SetHudVisible(!hudWanted_); break;
        case IdResetStats: ResetStats(); break;
        case IdSnapCorner: cfg_.customPos = false; SaveSoon(); break;
        case IdQuit:       running_ = false; break;
        default: break;
    }
}

LRESULT CALLBACK App::MsgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && self->msgWnd_ == hwnd) return self->HandleMsg(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT App::HandleMsg(UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TRAY) {
        switch (LOWORD(lp)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                OpenSettings();
                break;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU: {
                POINT pt;
                GetCursorPos(&pt);
                ShowMenu(pt, false);
                break;
            }
            default: break;
        }
        return 0;
    }
    if (msg == ShowSettingsMessage()) {
        OpenSettings();
        return 0;
    }
    if (taskbarCreated_ && msg == taskbarCreated_) {
        // Explorer restarted: the tray icon is gone, add it again.
        trayAdded_ = false;
        AddTrayIcon();
        return 0;
    }
    switch (msg) {
    case WM_QUERYENDSESSION:
        SaveNow();
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            SaveNow();
            running_ = false;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(msgWnd_, msg, wp, lp);
}
