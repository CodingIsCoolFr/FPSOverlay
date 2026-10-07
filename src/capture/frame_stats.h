// Frame-time history and the statistics shown on the HUD. Pure C++, unit-tested.
//
// Definitions (the same ones CapFrameX and most reviewers use):
//   FPS        = frames in the averaging window / sum of their frame times
//   frame time = mean frame time in the averaging window (ms)
//   1% low     = 1000 / mean of the slowest 1% of frame times in the lows window
//   0.1% low   = same with the slowest 0.1% (at least one frame)
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

struct FrameSample {
    double t;       // present time, seconds (QPC based)
    float  ms;      // time since the previous present of the same swap chain
};

// Fixed-capacity ring of frame samples, oldest first when iterated.
class FrameRing {
public:
    explicit FrameRing(size_t capacity = 16384);

    void   Push(double t, float ms);
    void   Clear();
    size_t Size() const { return size_; }
    const FrameSample& At(size_t i) const;      // 0 = oldest
    const FrameSample& Back() const { return At(size_ - 1); }

private:
    std::vector<FrameSample> buf_;
    size_t head_ = 0;   // next write slot
    size_t size_ = 0;
};

struct FrameStats {
    bool  valid = false;
    float fps = 0.f;
    float frameTimeMs = 0.f;
    float low1 = 0.f;
    float low01 = 0.f;
    int   frames = 0;       // frames inside the lows window
};

// now: time of the newest event the caller considers current (seconds, same clock as samples).
FrameStats ComputeFrameStats(const FrameRing& ring, double now, double avgWindowSec, double lowsWindowSec);

// Copies up to maxCount most recent frame times (oldest first) into out.
void CopyRecentFrameTimes(const FrameRing& ring, size_t maxCount, std::vector<float>& out);
