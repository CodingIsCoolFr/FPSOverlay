// Small Win32 helpers shared across the app: text conversion, paths, elevation,
// dark-mode plumbing and process inspection.
#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace win {

std::string  ToUtf8(const wchar_t* w);
std::string  ToUtf8(const std::wstring& w);
std::wstring ToWide(const char* utf8);
std::wstring ToWide(const std::string& utf8);

std::wstring ExePath();
std::wstring ExeDir();          // with trailing backslash

// Folder for config.ini and the log: next to the exe when that folder is writable
// (portable install), otherwise %LOCALAPPDATA%\FPSOverlay.
std::wstring DataDir();         // with trailing backslash

bool IsElevated();

// Dark context menus (tray / overlay menu) and a dark caption for normal windows.
// Uses documented DWM attributes plus the uxtheme app-mode ordinals that Explorer
// and Notepad use; every call is a no-op where unsupported.
void EnableDarkMenus();
void ApplyDarkTitleBar(HWND hwnd, COLORREF caption, COLORREF text, COLORREF border);

// "game.exe" and the file description from version info ("Game Title"), UTF-8.
struct ProcessInfo {
    std::string exe;            // file name only
    std::string description;    // may be empty
    std::wstring path;          // full image path, may be empty if access is denied
};
bool QueryProcessInfo(DWORD pid, ProcessInfo& out);

// True if the process has a module with this (case-insensitive) file name loaded.
// Fails closed (false) when the process cannot be opened, e.g. anti-cheat protected games.
bool ProcessHasModule(DWORD pid, const wchar_t* moduleName);
// The same for any module whose file name `match` accepts.
bool ProcessHasModuleWhere(DWORD pid, bool (*match)(std::wstring_view moduleName));

bool ProcessAlive(DWORD pid);

// Hidden by Windows itself although "visible": on another virtual desktop, or a suspended Store app.
bool IsCloaked(HWND hwnd);

// Physical monitor list in enumeration order (index 0 is not necessarily the primary).
struct MonitorInfo {
    HMONITOR handle = nullptr;
    RECT     rect = {};         // full monitor rectangle, virtual-screen pixels
    RECT     work = {};         // work area (excludes taskbar)
    bool     primary = false;
    UINT     dpi = 96;
    std::string name;           // "\\.\DISPLAY1"
};
int  EnumerateMonitors(MonitorInfo* out, int cap);

UINT DpiForMonitor(HMONITOR mon);

// Display name for a virtual-key + modifier combination, e.g. "Ctrl+Shift+F".
std::string HotkeyName(UINT vk, UINT mods);

} // namespace win
