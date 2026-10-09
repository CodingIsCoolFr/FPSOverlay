#include "ui/scroll_glide.h"

#include <algorithm>
#include <cmath>

void ScrollGlide::Jump(float pos)
{
    pos_ = target_ = pos;
    vel_ = 0.f;
    time_ = kSeconds;
}

void ScrollGlide::SetTarget(float target)
{
    if (target == target_) return;
    target_ = target;
    // Keep the current speed if it heads toward the new target, but no faster than three times
    // the average (more and the curve overshoots). From rest, start at twice the average, a
    // quadratic ease-out, so a single notch moves at once.
    const float dist = target - pos_;
    const float dir = dist < 0.f ? -1.f : 1.f;
    const float average = std::fabs(dist) / kSeconds;
    fromVel_ = dir * (std::fabs(vel_) < 1.f ? 2.f * average : std::clamp(vel_ * dir, 0.f, 3.f * average));
    from_ = pos_;
    time_ = 0.f;
}

void ScrollGlide::Advance(float seconds)
{
    if (time_ >= kSeconds) return;
    time_ = std::min(time_ + std::max(seconds, 0.f), kSeconds);
    if (time_ >= kSeconds) {
        pos_ = target_;
        vel_ = 0.f;
        return;
    }
    // Cubic Hermite from (from_, fromVel_) to (target_, 0) over kSeconds, and its derivative.
    const float s = time_ / kSeconds, s2 = s * s, s3 = s2 * s;
    pos_ = (2 * s3 - 3 * s2 + 1) * from_ + (s3 - 2 * s2 + s) * kSeconds * fromVel_ + (3 * s2 - 2 * s3) * target_;
    vel_ = (6 * s2 - 6 * s) * (from_ - target_) / kSeconds + (3 * s2 - 4 * s + 1) * fromVel_;
}
