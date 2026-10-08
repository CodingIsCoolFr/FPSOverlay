#include "capture/capture_watchdog.h"

#include <algorithm>

bool CaptureWatchdog::Update(const Inputs& in, uint64_t nowMs)
{
    if (!started_ || in.events != lastEvents_) {
        if (started_) restarts_ = 0;            // events are flowing again
        started_ = true;
        lastEvents_ = in.events;
        eventsChangedMs_ = nowMs;
    }
    if (!in.gameInFront) gameSinceMs_ = 0;
    else if (!gameSinceMs_) gameSinceMs_ = nowMs;

    const char* reason = nullptr;
    if (!in.running) reason = "is not running";
    else if (in.consumerEnded) reason = "session ended";
    else if (in.heartbeatsMissing >= kHeartbeatsLost) reason = "stopped delivering events";
    else if (gameSinceMs_ && nowMs - std::max(gameSinceMs_, eventsChangedMs_) >= kNoPresentsMs)
        reason = "saw no frames while a game was in front";
    if (!reason || nowMs < holdUntilMs_) return false;

    reason_ = reason;
    holdUntilMs_ = nowMs + std::min(kFirstHoldMs << std::min(restarts_, 4), kMaxHoldMs);
    ++restarts_;
    // The restarted session gets the full grace period again.
    eventsChangedMs_ = nowMs;
    if (gameSinceMs_) gameSinceMs_ = nowMs;
    return true;
}
