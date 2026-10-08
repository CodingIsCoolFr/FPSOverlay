#include "app/game_visibility.h"

bool GameVisibility::Update(const Inputs& in, uint64_t nowMs)
{
    if (in.target != target_) {     // a new target starts clean
        target_ = in.target;
        focused_ = false;
        left_ = false;
    }

    switch (in.front) {
        case Front::Game:
            focused_ = true;
            left_ = false;
            break;
        case Front::GameScreen:
            if (focused_) {
                left_ = true;
                focused_ = false;
            }
            break;
        default:                    // focus moving, our own windows, another screen
            break;
    }

    drawing_ = in.secondsSinceFrame < (shown_ ? 3.0 : 0.5);
    if (in.target && in.isGame && drawing_ && !in.minimized && !left_) {
        shown_ = true;
        lastGoodMs_ = nowMs;
    } else if (shown_ && nowMs - lastGoodMs_ > 400) {
        shown_ = false;
    }
    return shown_;
}
