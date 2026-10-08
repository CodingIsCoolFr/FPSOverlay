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

void TargetTracker::Update(const FrameCapture& capture, const games::Choices& choices)
{
    if (pid_ && !win::ProcessAlive(pid_)) SetTarget(0);

    HWND fgWnd = nullptr;
    const DWORD fg = ForegroundPid(&fgWnd);
    if (fg && fg != fg_.pid) {
        fg_ = {};
        fg_.pid = fg;
        win::ProcessInfo info;
        if (win::QueryProcessInfo(fg, info)) {
            fg_.path = info.path;
            fg_.exe = info.exe;
        }
    }
    if (fg) fg_.fullscreen = games::FillsMonitor(fgWnd);

    if (fg && fg != pid_) {
        const bool fgDrawing = capture.IsPresenting(fg, 1.5);
        const bool curDrawing = pid_ && capture.IsPresenting(pid_, 2.0);
        // A game in front always takes over. Any other app only while no game is still drawing,
        // so a tool or a browser in front never steals the HUD from a running game.
        if (!curDrawing || (fgDrawing && (!isGame_ || Judge(fg_, choices)))) SetTarget(fg);
    }
    if (fg && fg == pid_) fullscreen_ = fg_.fullscreen;
    isGame_ = pid_ && Judge(Candidate{ pid_, path_, exe_, fullscreen_ }, choices);
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
