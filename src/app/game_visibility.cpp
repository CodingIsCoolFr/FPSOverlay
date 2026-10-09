#include "app/game_visibility.h"

bool GameVisibility::Update(const Inputs& in, uint64_t nowMs)
{
    target_ = in.target;

    switch (in.front) {
        case Front::Game:
            if (in.target && in.isGame) {
                focusedPid_ = in.target;
                if (leftPid_ == in.target) leftPid_ = 0;
            }
            break;
        case Front::GameScreen:
            if (in.target && focusedPid_ == in.target) {
                leftPid_ = in.target;
                focusedPid_ = 0;
            }
            break;
        default:                    // focus moving, Windows' pop-ups, our own windows, another screen
            break;
    }

    // While the game has focus the user is playing it, whatever a windowed game leaves under the HUD.
    const bool onVideo = in.overVideo && in.front != Front::Game;
    drawing_ = in.secondsSinceFrame < (shown_ ? 3.0 : 0.5);
    if (in.target && in.isGame && drawing_ && !in.minimized && !AltTabbedOut() && !onVideo) {
        shown_ = true;
        lastGoodMs_ = nowMs;
    } else if (shown_ && nowMs - lastGoodMs_ > 400) {
        shown_ = false;
    }
    return shown_;
}
