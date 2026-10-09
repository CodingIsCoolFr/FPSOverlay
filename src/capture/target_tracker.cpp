#include "capture/target_tracker.h"
#include "capture/frame_capture.h"
#include "platform/win_util.h"

#include <vector>

namespace {

bool IsDesktopWindow(HWND hwnd)
{
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0;
}

bool IsShellWindow(HWND hwnd)
{
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return IsDesktopWindow(hwnd) || wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

// A window the user sees and could be using. Not: click-through (overlays), tool windows
// (overlays, floating toolbars, the taskbar), fully transparent, or cloaked.
bool IsSolidAppWindow(HWND hwnd)
{
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (ex & WS_EX_TRANSPARENT) return false;
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) && !(ex & WS_EX_APPWINDOW)) return false;
    if (ex & WS_EX_LAYERED) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) && alpha == 0)
            return false;
    }
    return !win::IsCloaked(hwnd);
}

// The process behind a window. Store apps: the frame window belongs to ApplicationFrameHost,
// the content to the app.
DWORD WindowPid(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        if (HWND core = FindWindowExW(hwnd, nullptr, L"Windows.UI.Core.CoreWindow", nullptr)) {
            DWORD inner = 0;
            GetWindowThreadProcessId(core, &inner);
            if (inner) pid = inner;
        }
    }
    return pid;
}

// The process's biggest visible, not minimized top-level window.
HWND BiggestWindow(DWORD pid)
{
    struct Search { DWORD pid; HWND best = nullptr; long long area = 0; } s{ pid };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<Search*>(lp);
        DWORD owner = 0;
        GetWindowThreadProcessId(h, &owner);
        if (owner != s->pid || !IsWindowVisible(h) || IsIconic(h)) return TRUE;
        RECT r;
        GetWindowRect(h, &r);
        const long long area = (long long)(r.right - r.left) * (r.bottom - r.top);
        if (area > s->area) {
            s->area = area;
            s->best = h;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.best;
}

} // namespace

DWORD TargetTracker::ForegroundPid(HWND* window)
{
    HWND fg = GetForegroundWindow();
    if (window) *window = fg;
    fgPopup_ = false;
    if (!fg) return 0;

    DWORD owner = 0;
    GetWindowThreadProcessId(fg, &owner);
    if (owner == GetCurrentProcessId()) return 0;

    const DWORD pid = WindowPid(fg);
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (!IsWindowVisible(fg) || win::IsCloaked(fg) || games::IsWindowsPopup(Lookup(pid).exe, cls)) {
        fgPopup_ = true;
        return 0;
    }
    return IsShellWindow(fg) ? 0 : pid;
}

void TargetTracker::SetTarget(DWORD pid)
{
    if (pid == pid_) return;
    pid_ = pid;
    name_.clear();
    exe_.clear();
    path_.clear();
    api_.clear();
    apiForSource_ = -2;
    isGame_ = false;
    fullscreen_ = false;
    window_ = nullptr;
    if (!pid) return;

    win::ProcessInfo info;
    if (win::QueryProcessInfo(pid, info)) {
        exe_ = info.exe;
        path_ = info.path;
        name_ = info.description.empty() ? info.exe : info.description;
    }
}

games::Verdict TargetTracker::Verdict(Candidate& c, const games::Choices& choices)
{
    const std::vector<std::wstring>& known = games::WindowsGameList();
    const games::Verdict plain = games::Classify(c.path, c.exe, choices, known);
    const games::Verdict asPlayer = games::Classify(c.path, c.exe, choices, known, true);
    if (plain == asPlayer) return plain;    // a video engine would not change the answer
    // Players may load their engine only when playback starts: look again every 5 s.
    const ULONGLONG now = GetTickCount64();
    if (!c.videoAt || now - c.videoAt >= 5000) {
        c.videoAt = now;
        c.video = win::ProcessHasModuleWhere(c.pid, games::IsVideoEngine);
    }
    return c.video ? asPlayer : plain;
}

bool TargetTracker::Judge(Candidate& c, const games::Choices& choices)
{
    switch (Verdict(c, choices)) {
        case games::Verdict::Game:    return true;
        case games::Verdict::NotGame: return false;
        default:                      return c.fullscreen;
    }
}

TargetTracker::Candidate& TargetTracker::Lookup(DWORD pid)
{
    auto it = known_.find(pid);
    if (it != known_.end()) return it->second;
    if (known_.size() > 256) known_.clear();    // pids of long-gone processes
    Candidate c;
    c.pid = pid;
    win::ProcessInfo info;
    if (win::QueryProcessInfo(pid, info)) {
        c.path = info.path;
        c.exe = info.exe;
    }
    return known_.emplace(pid, std::move(c)).first->second;
}

void TargetTracker::Update(const FrameCapture& capture, const games::Choices& choices)
{
    if (pid_ && !win::ProcessAlive(pid_)) SetTarget(0);

    HWND fgWnd = nullptr;
    const DWORD fg = ForegroundPid(&fgWnd);
    if (fg && fg != fg_.pid) fg_ = Lookup(fg);
    if (fg) fg_.fullscreen = games::FillsMonitor(fgWnd);

    if (fg && fg != pid_) {
        const bool fgDrawing = capture.IsPresenting(fg, 1.5);
        const bool curDrawing = pid_ && capture.IsPresenting(pid_, 2.0);
        // A game in front always takes over. Any other app only while no game is still drawing,
        // so a tool or a browser in front never steals the HUD from a running game.
        if (!curDrawing || (fgDrawing && (!isGame_ || Judge(fg_, choices)))) SetTarget(fg);
    }
    if (fg && fg == pid_) {
        fullscreen_ = fg_.fullscreen;
        window_ = fgWnd;
    }
    isGame_ = false;
    if (pid_) {
        Candidate& t = Lookup(pid_);
        t.fullscreen = fullscreen_;
        isGame_ = Judge(t, choices);
    }

    // No game drawing in front. One may still be drawing somewhere: a VR game, a game started
    // before this app, a game behind a chat window that was never clicked. Pick it up.
    if (!isGame_ || !capture.IsPresenting(pid_, 2.0)) {
        for (DWORD p : capture.PresentingPids(1.0)) {
            if (p == pid_) continue;
            Candidate& c = Lookup(p);
            // Only a game by its name or folder needs the video check (a player Windows lists as
            // a game); the rest are not picked up either way.
            if (games::Classify(c.path, c.exe, choices, games::WindowsGameList()) != games::Verdict::Game) continue;
            if (Verdict(c, choices) != games::Verdict::Game) continue;
            SetTarget(p);
            isGame_ = true;
            break;
        }
    }
    if (pid_ && (!window_ || !IsWindow(window_))) window_ = BiggestWindow(pid_);
}

// The app drew a frame in the last 3 s. Chromium and Electron apps draw from a GPU process of
// the same program file.
bool TargetTracker::AppDrawing(DWORD pid, const std::wstring& path, const FrameCapture& capture)
{
    if (capture.IsPresenting(pid, 3.0)) return true;
    if (path.empty()) return false;
    for (DWORD p : capture.PresentingPids(3.0))
        if (p != pid && Lookup(p).path == path) return true;
    return false;
}

TargetTracker::Spot TargetTracker::Look(POINT pt, const FrameCapture& capture, const games::Choices& choices)
{
    spotExe_.clear();
    // Top-level windows under the point, top to bottom.
    struct Search { POINT pt; std::vector<HWND> hits; } s{ pt };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<Search*>(lp);
        RECT r;
        if (IsWindowVisible(h) && !IsIconic(h) && GetWindowRect(h, &r) && PtInRect(&r, s->pt)) s->hits.push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));

    const DWORD self = GetCurrentProcessId();
    for (HWND w : s.hits) {
        if (IsDesktopWindow(w)) return Spot::Other;     // nothing open there
        if (!IsSolidAppWindow(w)) continue;             // the taskbar, overlays, tooltips
        DWORD owner = 0;
        GetWindowThreadProcessId(w, &owner);
        if (owner == self) continue;
        const DWORD pid = WindowPid(w);
        if (pid == pid_) return Spot::Game;

        // Look through overlays, scalers and screenshot tools, and through Windows' pop-ups:
        // Start, search and the emoji panel draw on layers as big as the screen.
        Candidate& c = Lookup(pid);
        wchar_t cls[64] = {};
        GetClassNameW(w, cls, 64);
        if (games::IsSeeThrough(c.exe) || games::IsWindowsPopup(c.exe, cls)) continue;
        spotExe_ = c.exe;
        const std::wstring path = c.path;
        const games::Verdict verdict = Verdict(c, choices);
        const bool player = games::IsVideoApp(c.exe) || c.video;
        if (verdict != games::Verdict::Game && games::FillsMonitor(w)) return Spot::FullScreenApp;
        if (player && AppDrawing(pid, path, capture)) return Spot::Video;
        return Spot::Other;
    }
    return Spot::Other;
}

void TargetTracker::Refine(PresentSource source)
{
    if (!pid_ || (int)source == apiForSource_) return;
    apiForSource_ = (int)source;

    auto has = [this](const wchar_t* m) { return win::ProcessHasModule(pid_, m); };
    switch (source) {
        case PresentSource::Dxgi:
            if (has(L"d3d12.dll"))          api_ = "DX12";
            else if (has(L"vulkan-1.dll"))  api_ = "Vulkan";
            else if (has(L"d3d11.dll"))     api_ = "DX11";
            else if (has(L"opengl32.dll"))  api_ = "OpenGL";
            else if (has(L"d3d10.dll") || has(L"d3d10_1.dll")) api_ = "DX10";
            else                            api_ = "DXGI";
            break;
        case PresentSource::D3D9:
            api_ = "DX9";
            break;
        case PresentSource::DxgkPresent:
        case PresentSource::DxgkFlipBlt:
            if (has(L"vulkan-1.dll"))       api_ = "Vulkan";
            else if (has(L"opengl32.dll"))  api_ = "OpenGL";
            else                            api_.clear();
            break;
        default:
            api_.clear();
            break;
    }
}
