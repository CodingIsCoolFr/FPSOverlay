#include "capture/frame_stats.h"

#include <algorithm>
#include <functional>

FrameRing::FrameRing(size_t capacity) : buf_(capacity ? capacity : 1) {}

void FrameRing::Push(double t, float ms)
{
    buf_[head_] = { t, ms };
    head_ = (head_ + 1) % buf_.size();
    if (size_ < buf_.size()) ++size_;
}

void FrameRing::Clear()
{
    head_ = 0;
    size_ = 0;
}

const FrameSample& FrameRing::At(size_t i) const
{
    const size_t start = (head_ + buf_.size() - size_) % buf_.size();
    return buf_[(start + i) % buf_.size()];
}

namespace {

// Mean of the largest `count` values. Partially sorts `v`.
double MeanOfWorst(std::vector<float>& v, size_t count)
{
    if (v.empty() || count == 0) return 0.0;
    if (count > v.size()) count = v.size();
    std::nth_element(v.begin(), v.begin() + (count - 1), v.end(), std::greater<float>());
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += v[i];
    return sum / (double)count;
}

} // namespace

FrameStats ComputeFrameStats(const FrameRing& ring, double now, double avgWindowSec, double lowsWindowSec)
{
    FrameStats s;
    const size_t n = ring.Size();
    if (n == 0) return s;

    const double avgFrom = now - avgWindowSec;
    const double lowsFrom = now - lowsWindowSec;

    double avgSumMs = 0.0;
    int avgCount = 0;
    std::vector<float> lows;
    lows.reserve(std::min<size_t>(n, 16384));

    // Walk newest to oldest and stop once both windows are covered.
    for (size_t i = n; i-- > 0;) {
        const FrameSample& f = ring.At(i);
        if (f.t > now) continue;            // ignore samples newer than "now" (clock skew)
        const bool inAvg = f.t >= avgFrom;
        const bool inLows = f.t >= lowsFrom;
        if (!inAvg && !inLows) break;
        if (inAvg) { avgSumMs += f.ms; ++avgCount; }
        if (inLows) lows.push_back(f.ms);
    }

    if (avgCount == 0 || avgSumMs <= 0.0) return s;

    s.valid = true;
    s.fps = (float)(1000.0 * avgCount / avgSumMs);
    s.frameTimeMs = (float)(avgSumMs / avgCount);
    s.frames = (int)lows.size();

    if (!lows.empty()) {
        const size_t n1 = std::max<size_t>(1, lows.size() / 100);
        const size_t n01 = std::max<size_t>(1, lows.size() / 1000);
        // Worst 0.1% is a subset of the worst 1%, so compute 1% first on the full set,
        // then the 0.1% on the already partitioned prefix.
        const double worst1 = MeanOfWorst(lows, n1);
        lows.resize(n1);
        const double worst01 = MeanOfWorst(lows, n01);
        s.low1 = worst1 > 0.0 ? (float)(1000.0 / worst1) : 0.f;
        s.low01 = worst01 > 0.0 ? (float)(1000.0 / worst01) : 0.f;
    }
    return s;
}

void CopyRecentFrameTimes(const FrameRing& ring, size_t maxCount, std::vector<float>& out)
{
    out.clear();
    const size_t n = ring.Size();
    const size_t count = std::min(n, maxCount);
    out.reserve(count);
    for (size_t i = n - count; i < n; ++i) out.push_back(ring.At(i).ms);
}
