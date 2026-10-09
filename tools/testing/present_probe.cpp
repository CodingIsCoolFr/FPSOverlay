// Lists every process that presents frames, once a second, with its rate, the foreground app,
// and how much of its monitor its biggest window covers. Built with FPSO_TEST_INSTANCE so its
// ETW session never collides with the user's running overlay.
//
// Build (x64 Native Tools prompt, from the repo root):
//   cl /nologo /std:c++20 /EHsc /utf-8 /O2 /DFPSO_TEST_INSTANCE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /I src
//      tools\testing\present_probe.cpp src\capture\frame_capture.cpp src\capture\frame_stats.cpp
//      src\platform\win_util.cpp src\app\log.cpp advapi32.lib tdh.lib ole32.lib oleaut32.lib
//      shell32.lib user32.lib dwmapi.lib uxtheme.lib version.lib shcore.lib /Fe:build\probe\probe.exe
// Run: build\probe\probe.exe [seconds]   (needs admin or the Performance Log Users group)
#include "capture/frame_capture.h"
#include "platform/win_util.h"
#include "app/log.h"

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <map>
#include <string>

struct Biggest { HWND hwnd = nullptr; long long area = 0; DWORD pid = 0; };

static BOOL CALLBACK EnumWin(HWND h, LPARAM lp)
{
    auto* b = (Biggest*)lp;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != b->pid) return TRUE;
    RECT r;
    GetWindowRect(h, &r);
    const long long a = (long long)(r.right - r.left) * (r.bottom - r.top);
    if (a > b->area) { b->area = a; b->hwnd = h; }
    return TRUE;
}

static double Coverage(DWORD pid)
{
    Biggest b;
    b.pid = pid;
    EnumWindows(EnumWin, (LPARAM)&b);
    if (!b.hwnd) return 0;
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(b.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT r, x;
    GetWindowRect(b.hwnd, &r);
    IntersectRect(&x, &r, &mi.rcMonitor);
    const double m = (double)(mi.rcMonitor.right - mi.rcMonitor.left) * (mi.rcMonitor.bottom - mi.rcMonitor.top);
    return m > 0 ? (double)(x.right - x.left) * (x.bottom - x.top) / m : 0;
}

int wmain(int argc, wchar_t** argv)
{
    const int seconds = argc > 1 ? _wtoi(argv[1]) : 30;
    FrameCapture cap;
    std::string err;
    if (!cap.Start(err)) { printf("capture failed: %s\n", err.c_str()); return 1; }
    std::map<std::string, int> seen;     // exe -> seconds seen presenting
    for (int s = 0; s < seconds; ++s) {
        Sleep(1000);
        DWORD fgPid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fgPid);
        win::ProcessInfo fgi;
        win::QueryProcessInfo(fgPid, fgi);
        printf("t=%2d fg=%s\n", s + 1, fgi.exe.c_str());
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = { sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            const auto r = cap.Query(pe.th32ProcessID, 1.0, 10.0);
            if (r.secondsSinceLastFrame > 1.0) continue;
            const std::string exe = win::ToUtf8(pe.szExeFile);
            ++seen[exe];
            printf("   %-28s pid %6lu  %6.1f fps  %3d frames  src %-8s cover %3.0f%%%s\n", exe.c_str(),
                   pe.th32ProcessID, r.stats.valid ? r.stats.fps : 0.f, r.stats.frames, PresentSourceName(r.source),
                   Coverage(pe.th32ProcessID) * 100, pe.th32ProcessID == fgPid ? "  <- foreground" : "");
        }
        CloseHandle(snap);
        fflush(stdout);
    }
    cap.Stop();
    printf("\nseconds seen presenting:\n");
    for (auto& [exe, n] : seen) printf("   %-28s %d\n", exe.c_str(), n);
    return 0;
}
