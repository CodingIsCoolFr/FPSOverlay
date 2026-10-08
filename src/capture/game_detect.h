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
//   3. Windows' own list of games (`knownGames`: lower-case full paths, see WindowsGameList),
//   4. game library folders (Steam, Epic, GOG, Xbox, ...).
// Unknown means "a game if it fills the screen" (see FillsMonitor).
Verdict Classify(std::wstring_view exePath, std::string_view exeName, const Choices& choices,
                 const std::vector<std::wstring>& knownGames);

// The games Windows has recognized on this PC (the Game Bar's GameConfigStore): lower-case full
// exe paths. Read from the registry at most every 30 seconds.
const std::vector<std::wstring>& WindowsGameList();

// True if the window covers its whole monitor: borderless or exclusive fullscreen.
bool FillsMonitor(HWND hwnd);

std::string Lower(std::string_view s);

} // namespace games
