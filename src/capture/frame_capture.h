// Present tracking through Event Tracing for Windows (the same data PresentMon uses).
//
// No injection, no hooks: the OS reports every Present call it sees. That makes it work with
// anti-cheat protected games, but it needs administrator rights and it only sees what goes
// through DXGI / D3D9 / the kernel graphics driver (so every API, including Vulkan and OpenGL).
#pragma once

#include "capture/frame_stats.h"

#include <windows.h>
#include <evntprov.h>
#include <evntrace.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

enum class PresentSource : int {
    Dxgi = 0,       // DXGI Present (Direct3D 10 / 11 / 12, and Vulkan / OpenGL on DXGI swap chains)
    D3D9,           // Direct3D 9 Present
    DxgkPresent,    // kernel present (Vulkan / OpenGL / anything else)
    DxgkFlipBlt,    // kernel flip / blit, last resort
    Count,
    None = -1
};

class FrameCapture {
public:
    FrameCapture();
    ~FrameCapture();
    FrameCapture(const FrameCapture&) = delete;
    FrameCapture& operator=(const FrameCapture&) = delete;

    bool Start(std::string& error);
    void Stop();
    bool Running() const { return running_.load(); }
    // The last Start failed for lack of administrator rights (retrying will not help).
    bool AccessDenied() const { return accessDenied_; }

    // Health, for CaptureWatchdog. A heartbeat event goes into the session twice a second and
    // must come back out through the consumer.
    int HeartbeatsMissing() const { return heartbeatsMissing_.load(); }
    bool ConsumerEnded() const { return consumerEnded_.load(); }

    // Seconds on the same clock as the frame samples.
    double Now() const;

    struct Result {
        FrameStats    stats;
        PresentSource source = PresentSource::None;
        double        secondsSinceLastFrame = 1e9;
    };
    // Statistics for one process. graph (optional) receives the newest graphCount frame times.
    Result Query(DWORD pid, double avgWindowSec, double lowsWindowSec,
                 std::vector<float>* graph = nullptr, size_t graphCount = 0) const;

    // True if pid presented a frame within the last `withinSec` seconds.
    bool IsPresenting(DWORD pid, double withinSec) const;
    // Every process that presented a frame within the last `withinSec` seconds.
    std::vector<DWORD> PresentingPids(double withinSec) const;

    void ResetHistory(DWORD pid);

    // Diagnostics
    uint64_t EventsSeen() const { return events_.load(); }
    uint64_t FlushFailures() const { return flushFailures_.load(); }
    // One line about pid: per source, presents in the last second and the age of the newest
    // event, the source in use, and the age of the newest counted frame.
    std::string Describe(DWORD pid) const;
    // The session's own counters (buffers written, events and buffers lost), for the log.
    std::string SessionStats() const;

private:
    struct Chain {
        uint64_t key = 0;
        uint64_t lastQpc = 0;
        uint64_t windowStart = 0;
        uint32_t windowCount = 0;
        uint32_t lastRate = 0;
    };
    struct Source {
        uint64_t lastQpc = 0;
        Chain    chains[4];
    };
    struct Proc {
        Source   src[(int)PresentSource::Count];
        FrameRing ring{ 16384 };
        uint64_t lastFrameQpc = 0;
        PresentSource active = PresentSource::None;
    };

    static void WINAPI OnEvent(PEVENT_RECORD rec);
    void HandleEvent(PEVENT_RECORD rec);
    void OnPresent(DWORD pid, PresentSource src, uint64_t qpc, uint64_t chainKey);
    void PurgeIdleLocked(uint64_t nowQpc);
    void FlushLoop();

    double QpcToSec(uint64_t qpc) const { return (double)qpc / qpcFreq_; }

    std::wstring sessionName_;
    TRACEHANDLE  session_ = 0;
    TRACEHANDLE  trace_ = INVALID_PROCESSTRACE_HANDLE;
    std::thread  consumer_;
    std::thread  flusher_;
    std::atomic<bool> running_{ false };
    bool         accessDenied_ = false;
    REGHANDLE    heartbeat_ = 0;
    std::atomic<int>  heartbeatsMissing_{ 0 };
    std::atomic<bool> consumerEnded_{ false };
    std::atomic<bool> firstHeartbeat_{ false };     // not logged yet for this session
    HANDLE       consumerExit_ = nullptr;           // set when ProcessTrace returns
    std::atomic<unsigned> generation_{ 0 };         // which Start a consumer thread belongs to
    uint64_t     startQpc_ = 0;
    std::mutex   flushMutex_;
    std::condition_variable flushCv_;
    double       qpcFreq_ = 1.0;
    uint64_t     lastPurgeQpc_ = 0;
    DWORD        selfPid_ = 0;

    mutable std::mutex mutex_;
    std::unordered_map<DWORD, std::unique_ptr<Proc>> procs_;
    std::atomic<uint64_t> events_{ 0 };
    std::atomic<uint64_t> flushFailures_{ 0 };
};

const char* PresentSourceName(PresentSource s);
