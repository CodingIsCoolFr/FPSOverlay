#include "platform/win_util.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <vector>

namespace win {

std::string ToUtf8(const wchar_t* w)
{
    if (!w || !w[0]) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string ToUtf8(const std::wstring& w) { return ToUtf8(w.c_str()); }

std::wstring ToWide(const char* utf8)
{
    if (!utf8 || !utf8[0]) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    return w;
}

std::wstring ToWide(const std::string& utf8) { return ToWide(utf8.c_str()); }

std::wstring ExePath()
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
        if (n == 0) return {};
        if (n < buf.size()) return std::wstring(buf.data(), n);
        buf.resize(buf.size() * 2);
    }
}

std::wstring ExeDir()
{
    std::wstring p = ExePath();
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}

static bool DirWritable(const std::wstring& dir)
{
    const std::wstring probe = dir + L".fpsoverlay-write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

std::wstring DataDir()
{
    static std::wstring cached;
    if (!cached.empty()) return cached;

    const std::wstring exeDir = ExeDir();
    if (!exeDir.empty() && DirWritable(exeDir)) {
        cached = exeDir;
        return cached;
    }

    PWSTR local = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) {
        cached = std::wstring(local) + L"\\FPSOverlay\\";
        CoTaskMemFree(local);
        CreateDirectoryW(cached.c_str(), nullptr);
    } else {
        cached = exeDir;
    }
    return cached;
}

bool IsElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elev = {};
    DWORD len = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &len);
    CloseHandle(token);
    return ok && elev.TokenIsElevated;
}

// ── Dark mode ────────────────────────────────────────────────────────────────

void EnableDarkMenus()
{
    // uxtheme.dll ordinals (Windows 10 1903+): 135 SetPreferredAppMode, 136 FlushMenuThemes.
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(2 /* ForceDark */);
    if (flush) flush();
    // uxtheme stays loaded for the life of the process; menus keep using it.
}

void ApplyDarkTitleBar(HWND hwnd, COLORREF caption, COLORREF text, COLORREF border)
{
    const BOOL dark = TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Windows 10 20H1+), 19 on older 1809-1909 builds.
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
    // Windows 11: exact caption / border / text colors. Ignored on Windows 10.
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
}

// ── Processes ────────────────────────────────────────────────────────────────

static std::string FileDescription(const wchar_t* path)
{
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return {};
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return {};

    struct LangCodePage { WORD lang; WORD codePage; } *tr = nullptr;
    UINT trLen = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &trLen) ||
        !tr || trLen < sizeof(LangCodePage))
        return {};

    wchar_t block[64];
    swprintf_s(block, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr[0].lang, tr[0].codePage);
    wchar_t* desc = nullptr;
    UINT descLen = 0;
    if (!VerQueryValueW(data.data(), block, reinterpret_cast<void**>(&desc), &descLen) || !desc || !desc[0])
        return {};
    std::string s = ToUtf8(desc);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

bool QueryProcessInfo(DWORD pid, ProcessInfo& out)
{
    out = {};
    if (!pid) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    std::vector<wchar_t> buf(1024);
    DWORD len = (DWORD)buf.size();
    const bool ok = QueryFullProcessImageNameW(h, 0, buf.data(), &len) != FALSE;
    CloseHandle(h);
    if (!ok) return false;

    out.path.assign(buf.data(), len);
    const size_t slash = out.path.find_last_of(L"\\/");
    out.exe = ToUtf8(slash == std::wstring::npos ? out.path : out.path.substr(slash + 1));
    out.description = FileDescription(out.path.c_str());
    return true;
}

bool ProcessHasModule(DWORD pid, const wchar_t* moduleName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
        if (_wcsicmp(me.szModule, moduleName) == 0) { found = true; break; }
    }
    CloseHandle(snap);
    return found;
}

bool ProcessHasModuleWhere(DWORD pid, bool (*match)(std::wstring_view moduleName))
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    bool found = false;
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me))
        found = match(me.szModule);
    CloseHandle(snap);
    return found;
}

bool ProcessAlive(DWORD pid)
{
    if (!pid) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) {
        // Access denied still means the process exists.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

bool IsCloaked(HWND hwnd)
{
    DWORD cloaked = 0;
    return hwnd && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked;
}

// ── Monitors ─────────────────────────────────────────────────────────────────

UINT DpiForMonitor(HMONITOR mon)
{
    UINT x = 96, y = 96;
    if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y)) && x > 0) return x;
    return 96;
}

namespace {
struct EnumCtx { MonitorInfo* out; int cap; int n; };
BOOL CALLBACK EnumMonProc(HMONITOR mon, HDC, LPRECT, LPARAM lp)
{
    auto* ctx = reinterpret_cast<EnumCtx*>(lp);
    if (ctx->n >= ctx->cap) return FALSE;
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    MonitorInfo& m = ctx->out[ctx->n++];
    m.handle = mon;
    m.rect = mi.rcMonitor;
    m.work = mi.rcWork;
    m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m.dpi = DpiForMonitor(mon);
    m.name = ToUtf8(mi.szDevice);
    return TRUE;
}
} // namespace

int EnumerateMonitors(MonitorInfo* out, int cap)
{
    EnumCtx ctx{ out, cap, 0 };
    EnumDisplayMonitors(nullptr, nullptr, EnumMonProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.n;
}

// ── Hotkeys ──────────────────────────────────────────────────────────────────

static std::string KeyName(UINT vk)
{
    switch (vk) {
        case 0:           return "";
        case VK_LBUTTON:  return "Mouse 1";
        case VK_RBUTTON:  return "Mouse 2";
        case VK_MBUTTON:  return "Mouse 3";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_PAUSE:    return "Pause";
        default: break;
    }
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = (LONG)(sc << 16);
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR: case VK_NEXT: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
        case VK_NUMLOCK: case VK_SNAPSHOT: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
            lp |= (1 << 24);
            break;
        default: break;
    }
    wchar_t name[64] = {};
    if (GetKeyNameTextW(lp, name, 64) > 0) return ToUtf8(name);
    char buf[16];
    snprintf(buf, sizeof(buf), "Key 0x%02X", vk);
    return buf;
}

std::string HotkeyName(UINT vk, UINT mods)
{
    if (!vk) return {};
    std::string s;
    if (mods & MOD_CONTROL) s += "Ctrl+";
    if (mods & MOD_ALT)     s += "Alt+";
    if (mods & MOD_SHIFT)   s += "Shift+";
    if (mods & MOD_WIN)     s += "Win+";
    s += KeyName(vk);
    return s;
}

} // namespace win
