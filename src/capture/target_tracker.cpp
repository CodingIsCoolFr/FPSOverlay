#include "capture/target_tracker.h"
#include "capture/frame_capture.h"
#include "platform/win_util.h"

namespace {

bool IsShellWindow(HWND hwnd)
{
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
           wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
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
    if (!fg || IsShellWindow(fg)) return 0;

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return 0;

    // Store apps: the frame window belongs to ApplicationFrameHost, the content to the app.
    wchar_t cls[64] = {};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"ApplicationFrameWindow") == 0) {
        if (HWND core = FindWindowExW(fg, nullptr, L"Windows.UI.Core.CoreWindow", nullptr)) {
            DWORD inner = 0;
            GetWindowThreadProcessId(core, &inner);
            if (inner) pid = inner;
        }
    }
    return pid;
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

bool TargetTracker::Judge(const Candidate& c, const games::Choices& choices) const
{
    switch (games::Classify(c.path, c.exe, choices, games::WindowsGameList())) {
        case games::Verdict::Game:    return true;
        case games::Verdict::NotGame: return false;
        default:                      return c.fullscreen;
    }
}

const TargetTracker::Candidate& TargetTracker::Lookup(DWORD pid)
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
    isGame_ = pid_ && Judge(Candidate{ pid_, path_, exe_, fullscreen_ }, choices);

    // No game drawing in front. One may still be drawing somewhere: a VR game, a game started
    // before this app, a game behind a chat window that was never clicked. Pick it up.
    if (!isGame_ || !capture.IsPresenting(pid_, 2.0)) {
        for (DWORD p : capture.PresentingPids(1.0)) {
            if (p == pid_) continue;
            const Candidate& c = Lookup(p);
            if (games::Classify(c.path, c.exe, choices, games::WindowsGameList()) != games::Verdict::Game) continue;
            SetTarget(p);
            isGame_ = true;
            break;
        }
    }
    if (pid_ && (!window_ || !IsWindow(window_))) window_ = BiggestWindow(pid_);
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
