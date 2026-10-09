// Is this program a game? Used by "Hide when no game is running", and so a chat app or a browser
// in front never takes the HUD away from a game that is still running.
#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

namespace games {

enum class Verdict { NotGame, Game, Unknown };

// Programs the user marked from the tray menu: lower-case exe file names.
struct Choices {
    std::vector<std::string> games;
    std::vector<std::string> notGames;
};

// Decides from the program file alone, first match wins:
//   1. the user's own choice,
//   2. well-known apps that draw frames but are not games (browsers, chat, launchers, players),
//   3. game library folders (Steam, Epic, GOG, Xbox, ...),
//   4. a video player engine loaded in the program (`playsVideo`, see IsVideoEngine): a film
//      shown full screen in an unknown player is not a game,
//   5. Windows' own list of games (`knownGames`: lower-case full paths, see WindowsGameList).
// Unknown means "a game if it fills the screen" (see FillsMonitor).
Verdict Classify(std::wstring_view exePath, std::string_view exeName, const Choices& choices,
                 const std::vector<std::wstring>& knownGames, bool playsVideo = false);

// Video players and streaming apps, by exe file name. Never games, and the HUD hides while one
// plays a video under it.
bool IsVideoApp(std::string_view exeName);

// A DLL that only video players load: mpv, VLC, PlayReady (the DRM of Netflix, Disney+, Apple
// TV and other streaming apps). Games inside game folders may load these too; Classify checks
// the folders first.
bool IsVideoEngine(std::wstring_view moduleName);

// Apps that draw over a game without hiding it: overlays, frame scalers (Lossless Scaling,
// Magpie) and screenshot tools. Their windows are looked through, never at.
bool IsSeeThrough(std::string_view exeName);

// Windows' own pop-ups: Start, search, the Alt+Tab switcher, the taskbar, notifications, quick
// settings, the emoji panel and the screenshot capture layer. Opening one is not leaving the game.
// `windowClass` is the foreground window's class.
bool IsWindowsPopup(std::string_view exeName, std::wstring_view windowClass);

// The games Windows has recognized on this PC (the Game Bar's GameConfigStore): lower-case full
// exe paths. Read from the registry at most every 30 seconds.
const std::vector<std::wstring>& WindowsGameList();

// True if the window covers its whole monitor without a title bar: borderless or exclusive
// fullscreen.
bool FillsMonitor(HWND hwnd);

std::string Lower(std::string_view s);

} // namespace games
