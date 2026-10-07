// Present tracking through Event Tracing for Windows (the same data PresentMon uses).
//
// No injection, no hooks: the OS reports every Present call it sees. That makes it work with
// anti-cheat protected games, but it needs administrator rights and it only sees what goes
// through DXGI / D3D9 / the kernel graphics driver (so every API, including Vulkan and OpenGL).
#pragma once

#include "capture/frame_stats.h"

#include <windows.h>
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

    void ResetHistory(DWORD pid);

    // Diagnostics
    uint64_t EventsSeen() const { return events_.load(); }

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
    std::mutex   flushMutex_;
    std::condition_variable flushCv_;
    double       qpcFreq_ = 1.0;
    uint64_t     lastPurgeQpc_ = 0;
    DWORD        selfPid_ = 0;

    mutable std::mutex mutex_;
    std::unordered_map<DWORD, std::unique_ptr<Proc>> procs_;
    std::atomic<uint64_t> events_{ 0 };
};

const char* PresentSourceName(PresentSource s);
