// "Hide when no game is running": decides, a few times a second, whether the HUD shows.
//
// Shown while a game is drawing. Hidden when there is no game, when the game's window is
// minimized, or after the user alt-tabs out of it: the game had focus, then another app took it
// on the game's screen. A game that never had focus (a VR game, one left running behind a video)
// was not alt-tabbed out of, so it keeps the HUD. Loading screens (3 s without frames) and quick
// focus changes (0.4 s) do not make it blink. Pure logic, so the unit tests can drive it.
#pragma once

#include <cstdint>

class GameVisibility {
public:
    // What is in front right now, as seen from the game.
    enum class Front {
        Moving,         // no foreground window: focus is changing (alt-tab)
        Ours,           // this app's settings, menus, or a Ctrl-drag of the HUD
        Game,           // the game itself
        GameScreen,     // another app, on the game's screen
        OtherScreen,    // another app, on a different screen
    };
    struct Inputs {
        uint32_t target = 0;                // pid being measured, 0 = none
        bool isGame = false;
        double secondsSinceFrame = 1e9;
        bool minimized = false;             // the game's window
        Front front = Front::Moving;
    };

    bool Update(const Inputs& in, uint64_t nowMs);

    bool Shown() const { return shown_; }
    bool AltTabbedOut() const { return left_; }
    bool Drawing() const { return drawing_; }

private:
    uint32_t target_ = 0;
    bool focused_ = false;      // the game had focus since it became the target
    bool left_ = false;         // ... and the user then alt-tabbed out of it
    bool drawing_ = false;
    bool shown_ = false;
    uint64_t lastGoodMs_ = 0;   // last update where every condition held
};
