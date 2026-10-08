// Notices a frame capture that has stopped delivering and asks for a restart.
//
// ETW can keep a session that looks healthy (it started, flushes succeed, no error anywhere)
// while the consumer receives nothing. It happened after another app used up all of the
// system's memory, and only a restart brought the capture back. Two checks catch it: the
// heartbeat event FrameCapture writes into its own session must come back out, and while a game
// is in front, some present must arrive. Restarts back off so a broken system is not hammered.
// Pure logic, so the unit tests can drive it.
#pragma once

#include <cstdint>

class CaptureWatchdog {
public:
    struct Inputs {
        bool running = false;           // the session is up (Start succeeded)
        bool consumerEnded = false;     // ProcessTrace returned: the session is gone
        int heartbeatsMissing = 0;      // heartbeats written since the last one came back
        uint64_t events = 0;            // present events received so far, any process
        bool gameInFront = false;       // a game has focus and is not minimized
    };

    // True when the capture should be restarted now.
    bool Update(const Inputs& in, uint64_t nowMs);

    const char* Reason() const { return reason_; }
    int Restarts() const { return restarts_; }      // in a row, with no events in between

    static constexpr int kHeartbeatsLost = 8;       // 4 s at two heartbeats a second
    static constexpr uint64_t kNoPresentsMs = 6000;
    static constexpr uint64_t kFirstHoldMs = 10000; // wait after a restart, doubling each time
    static constexpr uint64_t kMaxHoldMs = 120000;

private:
    bool started_ = false;
    uint64_t lastEvents_ = 0;
    uint64_t eventsChangedMs_ = 0;
    uint64_t gameSinceMs_ = 0;      // 0 = no game in front
    uint64_t holdUntilMs_ = 0;
    int restarts_ = 0;
    const char* reason_ = "";
};
