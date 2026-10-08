#include "capture/game_detect.h"

#include <algorithm>
#include <cctype>

namespace games {
namespace {

// Apps that draw frames all day but are not games. Browsers and Electron apps present from a
// GPU child process; media players, launchers and tools present while they animate.
constexpr const char* kNotGames[] = {
    // browsers
    "chrome.exe", "msedge.exe", "msedgewebview2.exe", "firefox.exe", "opera.exe", "brave.exe", "vivaldi.exe",
    "arc.exe", "librewolf.exe", "waterfox.exe", "floorp.exe", "zen.exe", "thorium.exe", "chromium.exe", "iexplore.exe",
    // chat, AI and other Electron apps
    "discord.exe", "discordptb.exe", "discordcanary.exe", "vesktop.exe", "singularity.exe", "slack.exe", "teams.exe",
    "ms-teams.exe", "telegram.exe", "whatsapp.exe", "signal.exe", "element.exe", "guilded.exe", "zoom.exe", "skype.exe",
    "claude.exe", "chatgpt.exe", "code.exe", "cursor.exe", "windsurf.exe", "spotify.exe", "notion.exe", "obsidian.exe",
    "figma.exe", "electron.exe",
    // game launchers and stores
    "steam.exe", "steamwebhelper.exe", "epicgameslauncher.exe", "epicwebhelper.exe", "battle.net.exe",
    "riotclientservices.exe", "riotclientux.exe", "riotclientuxrender.exe", "eadesktop.exe", "origin.exe", "upc.exe",
    "ubisoftconnect.exe", "galaxyclient.exe", "playnite.desktopapp.exe", "playnite.fullscreenapp.exe", "xboxpcapp.exe",
    "gamebar.exe", "heroic.exe", "itch.exe", "overwolf.exe", "medal.exe",
    // video and music
    "vlc.exe", "mpc-hc.exe", "mpc-hc64.exe", "mpc-be.exe", "mpc-be64.exe", "mpv.exe", "potplayermini.exe",
    "potplayermini64.exe", "wmplayer.exe", "video.ui.exe", "microsoft.media.player.exe", "plex.exe", "kodi.exe",
    "stremio.exe", "hayase.exe",
    // streaming, capture, hardware and desktop tools
    "obs64.exe", "obs32.exe", "streamlabs obs.exe", "nvidia overlay.exe", "nvidia app.exe", "radeonsoftware.exe",
    "msiafterburner.exe", "rtss.exe", "steelseriesggclient.exe", "steelseriesgg.exe", "icue.exe", "nzxt cam.exe",
    "wallpaper32.exe", "wallpaper64.exe", "lively.exe", "losslessscaling.exe", "vtube studio.exe", "blender.exe",
    "vrmonitor.exe", "vrserver.exe", "vrcompositor.exe", "vrdashboard.exe", "vrwebhelper.exe", "vrstartup.exe",
    // Windows itself
    "explorer.exe", "dwm.exe", "searchhost.exe", "startmenuexperiencehost.exe", "shellexperiencehost.exe",
    "applicationframehost.exe", "textinputhost.exe", "lockapp.exe", "taskmgr.exe", "systemsettings.exe",
    "fpsoverlay.exe",
};

// Folders games are installed into, lower-case, matched anywhere in the exe path.
constexpr const wchar_t* kGameFolders[] = {
    L"\\steamapps\\common\\", L"\\epic games\\", L"\\gog galaxy\\games\\", L"\\gog games\\", L"\\xboxgames\\",
    L"\\riot games\\", L"\\ubisoft game launcher\\games\\", L"\\ea games\\", L"\\origin games\\",
    L"\\amazon games\\library\\", L"\\itch\\apps\\", L"\\roblox\\versions\\", L"\\games\\",
};

std::wstring LowerW(std::wstring_view s)
{
    std::wstring o(s);
    if (!o.empty()) CharLowerBuffW(o.data(), (DWORD)o.size());
    return o;
}

bool Contains(const std::vector<std::string>& list, const std::string& v)
{
    return std::find(list.begin(), list.end(), v) != list.end();
}

} // namespace

std::string Lower(std::string_view s)
{
    std::string o(s);
    for (char& c : o) c = (char)std::tolower((unsigned char)c);
    return o;
}

Verdict Classify(std::wstring_view exePath, std::string_view exeName, const Choices& choices,
                 const std::vector<std::wstring>& knownGames)
{
    const std::string exe = Lower(exeName);
    if (exe.empty()) return Verdict::Unknown;
    if (Contains(choices.games, exe)) return Verdict::Game;
    if (Contains(choices.notGames, exe)) return Verdict::NotGame;
    for (const char* n : kNotGames)
        if (exe == n) return Verdict::NotGame;

    const std::wstring path = LowerW(exePath);
    if (path.empty()) return Verdict::Unknown;
    if (std::find(knownGames.begin(), knownGames.end(), path) != knownGames.end()) return Verdict::Game;
    for (const wchar_t* f : kGameFolders)
        if (path.find(f) != std::wstring::npos) return Verdict::Game;
    return Verdict::Unknown;
}

const std::vector<std::wstring>& WindowsGameList()
{
    static std::vector<std::wstring> list;
    static ULONGLONG readAt = 0;
    const ULONGLONG now = GetTickCount64();
    if (readAt && now - readAt < 30000) return list;
    readAt = now;
    list.clear();

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"System\\GameConfigStore\\Children", 0, KEY_READ, &root) != ERROR_SUCCESS)
        return list;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD len = 256;
        if (RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t path[MAX_PATH * 2] = {};
        DWORD size = sizeof(path);
        if (RegGetValueW(root, name, L"MatchedExeFullPath", RRF_RT_REG_SZ, nullptr, path, &size) == ERROR_SUCCESS && path[0])
            list.push_back(LowerW(path));
    }
    RegCloseKey(root);
    return list;
}

bool FillsMonitor(HWND hwnd)
{
    if (!hwnd || IsIconic(hwnd)) return false;
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return false;
    MONITORINFO mi = { sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    const RECT& m = mi.rcMonitor;
    // A few pixels of slack: some borderless games leave a 1 px gap to dodge exclusive mode.
    return r.left <= m.left + 2 && r.top <= m.top + 2 && r.right >= m.right - 2 && r.bottom >= m.bottom - 2;
}

} // namespace games
