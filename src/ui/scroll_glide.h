// Smooth wheel scrolling for the settings window: the page glides to its target instead of
// jumping a whole step per notch, like Windows' own lists.
//
// Each new target starts a 150 ms curve (cubic Hermite) from where the page is, at the speed it
// already has, ending at rest on the target. A spinning wheel so scrolls at an even speed, and a
// single notch moves at once and stops cleanly. Until 2.0.12 the page eased toward its target at a
// speed set by the distance left: it lurched at every notch and then crawled a pixel at a time for
// a quarter second. Pure logic, so the unit tests can drive it.
#pragma once

class ScrollGlide {
public:
    static constexpr float kSeconds = 0.15f;    // length of each curve

    void Jump(float pos);           // moved by something else (scrollbar, keys, page change): stop there
    void SetTarget(float target);   // a new target starts a new curve; the current one changes nothing
    void Advance(float seconds);

    float Position() const { return pos_; }
    float Target() const { return target_; }
    float Speed() const { return vel_; }    // pixels per second
    bool Gliding() const { return time_ < kSeconds; }

private:
    float pos_ = 0.f, vel_ = 0.f, target_ = 0.f;
    float from_ = 0.f, fromVel_ = 0.f;      // where the current curve started, and how fast
    float time_ = kSeconds;                 // time into it; at kSeconds the page rests
};
