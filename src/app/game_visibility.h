// "Hide when no game is running": decides, a few times a second, whether the HUD shows.
//
// Shown while a game is drawing. Hidden when there is no game, when the game's window is
// minimized or on another virtual desktop, after the user alt-tabs out of it (the game had
// focus, then another app took it on the game's screen), and, while the game is not the app in
// front, when the HUD would sit on a playing video or on another app's full-screen window. A game
// that never had focus (a VR game, one left running behind a chat window) was not alt-tabbed out
// of, so it keeps the HUD unless a video or a full-screen app is under it. Alt-tabbing out is remembered per game, so the tracker
// briefly switching to another app does not forget it. Loading screens (3 s without frames) and
// quick changes (0.4 s) do not make it blink. Pure logic, so the unit tests can drive it.
#pragma once

#include <cstdint>

class GameVisibility {
public:
    // What is in front right now, as seen from the game.
    enum class Front {
        Moving,         // no foreground window, or one of Windows' pop-ups (Start, Alt+Tab)
        Ours,           // this app's settings, menus, or a Ctrl-drag of the HUD
        Game,           // the game itself
        GameScreen,     // another app, on the game's screen
        OtherScreen,    // another app, on a different screen
    };
    struct Inputs {
        uint32_t target = 0;                // pid being measured, 0 = none
        bool isGame = false;
        double secondsSinceFrame = 1e9;
        bool minimized = false;             // the game's window (or it is on another desktop)
        Front front = Front::Moving;
        bool overVideo = false;             // under the HUD: a playing video or a full-screen app
    };

    bool Update(const Inputs& in, uint64_t nowMs);

    bool Shown() const { return shown_; }
    bool AltTabbedOut() const { return target_ && leftPid_ == target_; }
    bool Drawing() const { return drawing_; }

private:
    uint32_t target_ = 0;
    uint32_t focusedPid_ = 0;   // the game that had focus last, until the user left it
    uint32_t leftPid_ = 0;      // the game the user alt-tabbed out of, until they go back
    bool drawing_ = false;
    bool shown_ = false;
    uint64_t lastGoodMs_ = 0;   // last update where every condition held
};
