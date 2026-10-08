#include "capture/frame_capture.h"
#include "app/log.h"
#include "app/version.h"

#include <evntcons.h>

#include <chrono>
#include <cstring>

namespace {

// Microsoft-Windows-DXGI   {CA11C036-0102-4A2D-A6AD-F03CFED5D3C9}
const GUID kDxgiProvider = { 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };
// Microsoft-Windows-D3D9   {783ACA0A-790E-4D7F-8451-AA850511C6B9}
const GUID kD3d9Provider = { 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };
// Microsoft-Windows-DxgKrnl {802EC45A-1E99-4B83-9920-87C98277BA9D}
const GUID kDxgkProvider = { 0x802EC45A, 0x1E99, 0x4B83, { 0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D } };
// This app's heartbeat {6A3C1F52-8E4B-4D1A-9B7E-2F5D0C8A41E3}: proves the session still delivers.
const GUID kHeartbeatProvider = { 0x6A3C1F52, 0x8E4B, 0x4D1A, { 0x9B, 0x7E, 0x2F, 0x5D, 0x0C, 0x8A, 0x41, 0xE3 } };

constexpr USHORT kDxgiPresentStart = 42;
constexpr USHORT kDxgiPresentMpoStart = 55;
constexpr USHORT kD3d9PresentStart = 1;
constexpr USHORT kDxgkBlitInfo = 166;
constexpr USHORT kDxgkFlipInfo = 168;
constexpr USHORT kDxgkPresentInfo = 184;
constexpr USHORT kDxgkFlipMpoInfo = 252;

constexpr ULONGLONG kDxgkKeywordBase = 0x1;
constexpr ULONGLONG kDxgkKeywordPresent = 0x8000000;

constexpr UINT kDxgiPresentTest = 0x1;     // DXGI_PRESENT_TEST: an occlusion probe, not a frame

constexpr double kGapSeconds = 1.0;        // longer pauses are not frames (loading screens, alt-tab)
constexpr double kSourceTimeoutSec = 1.0;  // a source counts as active if it fired within this window
constexpr double kPurgeAfterSec = 15.0;

struct TraceProps {
    EVENT_TRACE_PROPERTIES p;
    wchar_t name[256];
};

void InitProps(TraceProps& tp)
{
    memset(&tp, 0, sizeof(tp));
    tp.p.Wnode.BufferSize = sizeof(tp);
    tp.p.LoggerNameOffset = offsetof(TraceProps, name);
}

ULONG EnableProvider(TRACEHANDLE session, const GUID& provider, ULONGLONG keywords, const USHORT* ids, USHORT count)
{
    std::vector<BYTE> buf(offsetof(EVENT_FILTER_EVENT_ID, Events) + sizeof(USHORT) * count);
    auto* filter = reinterpret_cast<EVENT_FILTER_EVENT_ID*>(buf.data());
    filter->FilterIn = TRUE;
    filter->Reserved = 0;
    filter->Count = count;
    memcpy(filter->Events, ids, sizeof(USHORT) * count);

    EVENT_FILTER_DESCRIPTOR desc = {};
    desc.Ptr = reinterpret_cast<ULONGLONG>(buf.data());
    desc.Size = (ULONG)buf.size();
    desc.Type = EVENT_FILTER_TYPE_EVENT_ID;

    ENABLE_TRACE_PARAMETERS params = {};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableFilterDesc = &desc;
    params.FilterDescCount = 1;
    ULONG rc = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                              keywords, 0, 0, &params);
    if (rc != ERROR_SUCCESS) {
        // Older systems: no filtering, the callback discards what it does not need.
        logx::Warn("Event ID filter rejected (%lu); enabling without it", rc);
        rc = EnableTraceEx2(session, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, keywords,
                            0, 0, nullptr);
    }
    return rc;
}

uint64_t ReadPointer(const EVENT_RECORD* rec, size_t offset)
{
    const bool is64 = (rec->EventHeader.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) != 0;
    const size_t size = is64 ? 8 : 4;
    if (!rec->UserData || rec->UserDataLength < offset + size) return 0;
    uint64_t v = 0;
    memcpy(&v, static_cast<const uint8_t*>(rec->UserData) + offset, size);
    return v;
}

uint32_t ReadU32(const EVENT_RECORD* rec, size_t offset)
{
    if (!rec->UserData || rec->UserDataLength < offset + 4) return 0;
    uint32_t v = 0;
    memcpy(&v, static_cast<const uint8_t*>(rec->UserData) + offset, 4);
    return v;
}

} // namespace

const char* PresentSourceName(PresentSource s)
{
    switch (s) {
        case PresentSource::Dxgi:        return "DXGI";
        case PresentSource::D3D9:        return "D3D9";
        case PresentSource::DxgkPresent: return "Kernel present";
        case PresentSource::DxgkFlipBlt: return "Kernel flip";
        default:                         return "None";
    }
}

FrameCapture::FrameCapture()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpcFreq_ = (double)f.QuadPart;
    selfPid_ = GetCurrentProcessId();
    sessionName_ = std::wstring(APP_ID_W) + L"_Presents";
}

FrameCapture::~FrameCapture() { Stop(); }

double FrameCapture::Now() const
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / qpcFreq_;
}

bool FrameCapture::Start(std::string& error)
{
    if (running_) return true;

    TraceProps tp;
    InitProps(tp);
    // A previous run that crashed leaves its session alive; take it down first.
    ControlTraceW(0, sessionName_.c_str(), &tp.p, EVENT_TRACE_CONTROL_STOP);

    InitProps(tp);
    tp.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    tp.p.Wnode.ClientContext = 1;               // QPC timestamps
    tp.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    tp.p.BufferSize = 32;                       // KB
    tp.p.MinimumBuffers = 16;
    tp.p.MaximumBuffers = 64;
    tp.p.FlushTimer = 1;                        // seconds; FlushLoop flushes much more often

    ULONG rc = StartTraceW(&session_, sessionName_.c_str(), &tp.p);
    accessDenied_ = rc == ERROR_ACCESS_DENIED;
    if (rc != ERROR_SUCCESS) {
        error = accessDenied_ ? "Frame capture needs administrator rights."
                              : "Could not start the frame capture session (error " + std::to_string(rc) + ").";
        logx::Error("StartTrace failed: %lu", rc);
        session_ = 0;
        return false;
    }

    // Only the present events are needed. Filtering by event ID inside ETW (Windows 8.1+) cuts the
    // system-wide event stream several times over, which is most of this app's CPU cost.
    const USHORT dxgiIds[] = { kDxgiPresentStart, kDxgiPresentMpoStart };
    const USHORT d3d9Ids[] = { kD3d9PresentStart };
    const USHORT dxgkIds[] = { kDxgkPresentInfo, kDxgkFlipInfo, kDxgkBlitInfo, kDxgkFlipMpoInfo };
    rc = EnableProvider(session_, kDxgiProvider, 0, dxgiIds, 2);
    if (rc != ERROR_SUCCESS) logx::Warn("DXGI provider: %lu", rc);
    rc = EnableProvider(session_, kD3d9Provider, 0, d3d9Ids, 1);
    if (rc != ERROR_SUCCESS) logx::Warn("D3D9 provider: %lu", rc);
    rc = EnableProvider(session_, kDxgkProvider, kDxgkKeywordPresent, dxgkIds, 4);
    if (rc != ERROR_SUCCESS) logx::Warn("DxgKrnl provider: %lu", rc);
    if (!heartbeat_ && EventRegister(&kHeartbeatProvider, nullptr, nullptr, &heartbeat_) != ERROR_SUCCESS) heartbeat_ = 0;
    rc = EnableTraceEx2(session_, &kHeartbeatProvider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION,
                        0, 0, 0, nullptr);
    if (rc != ERROR_SUCCESS) logx::Warn("Heartbeat provider: %lu", rc);

    EVENT_TRACE_LOGFILEW lf = {};
    lf.LoggerName = const_cast<LPWSTR>(sessionName_.c_str());
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD |
                          PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = &FrameCapture::OnEvent;
    lf.Context = this;

    trace_ = OpenTraceW(&lf);
    if (trace_ == INVALID_PROCESSTRACE_HANDLE) {
        const DWORD e = GetLastError();
        error = "Could not open the frame capture session (error " + std::to_string(e) + ").";
        logx::Error("OpenTrace failed: %lu", e);
        InitProps(tp);
        ControlTraceW(session_, nullptr, &tp.p, EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
        if (heartbeat_) EventUnregister(heartbeat_);
        heartbeat_ = 0;
        return false;
    }

    const unsigned gen = ++generation_;
    heartbeatsMissing_ = 0;
    consumerEnded_ = false;
    firstHeartbeat_ = true;
    startQpc_ = (uint64_t)(Now() * qpcFreq_);
    consumerExit_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    running_ = true;
    consumer_ = std::thread([this, gen, h = trace_, exit = consumerExit_] {
        SetThreadDescription(GetCurrentThread(), L"ETW consumer");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        TRACEHANDLE handle = h;
        const ULONG r = ProcessTrace(&handle, 1, nullptr, nullptr);
        if (running_ && gen == generation_) {
            // Nobody asked it to stop: the session was stopped from outside or broke.
            consumerEnded_ = true;
            logx::Warn("ProcessTrace returned %lu while capturing", r);
        } else if (r != ERROR_SUCCESS && r != ERROR_CANCELLED) {
            logx::Warn("ProcessTrace returned %lu", r);
        }
        if (exit) SetEvent(exit);
    });
    flusher_ = std::thread([this] { FlushLoop(); });
    logx::Info("Frame capture started");
    return true;
}

void FrameCapture::Stop()
{
    if (!running_.exchange(false)) return;

    {
        std::lock_guard<std::mutex> lock(flushMutex_);
    }
    flushCv_.notify_all();
    if (flusher_.joinable()) flusher_.join();

    if (trace_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(trace_);
        trace_ = INVALID_PROCESSTRACE_HANDLE;
    }
    if (consumer_.joinable()) {
        // A broken session must not freeze the app while the watchdog restarts it.
        if (!consumerExit_ || WaitForSingleObject(consumerExit_, 1500) == WAIT_OBJECT_0) {
            consumer_.join();
            if (consumerExit_) CloseHandle(consumerExit_);
        } else {
            logx::Warn("ETW consumer did not stop; leaving it behind");
            consumer_.detach();     // it still owns consumerExit_
        }
    }
    consumerExit_ = nullptr;

    TraceProps tp;
    InitProps(tp);
    ControlTraceW(session_, nullptr, &tp.p, EVENT_TRACE_CONTROL_STOP);
    session_ = 0;
    if (heartbeat_) EventUnregister(heartbeat_);
    heartbeat_ = 0;

    std::lock_guard<std::mutex> lock(mutex_);
    procs_.clear();
    logx::Info("Frame capture stopped");
}

void FrameCapture::FlushLoop()
{
    SetThreadDescription(GetCurrentThread(), L"ETW flush");
    // Real-time sessions hand buffers to the consumer only when they fill up or when the
    // flush timer fires (1 s minimum). Flushing every 100 ms keeps the frame graph live.
    bool warned = false;
    unsigned tick = 0;
    std::unique_lock<std::mutex> lock(flushMutex_);
    while (running_) {
        flushCv_.wait_for(lock, std::chrono::milliseconds(100));
        if (!running_) break;
        // Counted whether or not the write succeeds: a session whose buffers never drain makes
        // the write fail, and that is exactly the case to catch.
        if (heartbeat_ && ++tick % 5 == 0) {
            EventWriteString(heartbeat_, TRACE_LEVEL_INFORMATION, 0, L"");
            heartbeatsMissing_.fetch_add(1, std::memory_order_relaxed);
        }
        TraceProps tp;
        InitProps(tp);
        const ULONG rc = ControlTraceW(session_, nullptr, &tp.p, EVENT_TRACE_CONTROL_FLUSH);
        if (rc != ERROR_SUCCESS) {
            flushFailures_.fetch_add(1, std::memory_order_relaxed);
            if (!warned) {
                logx::Warn("ETW flush failed: %lu", rc);
                warned = true;
            }
        }
    }
}

void WINAPI FrameCapture::OnEvent(PEVENT_RECORD rec)
{
    auto* self = static_cast<FrameCapture*>(rec->UserContext);
    if (self) self->HandleEvent(rec);
}

void FrameCapture::HandleEvent(PEVENT_RECORD rec)
{
    if (!running_.load(std::memory_order_relaxed)) return;
    const EVENT_HEADER& h = rec->EventHeader;
    const DWORD pid = h.ProcessId;
    if (IsEqualGUID(h.ProviderId, kHeartbeatProvider)) {
        if (pid != selfPid_) return;        // another copy of the app (a test build)
        heartbeatsMissing_.store(0, std::memory_order_relaxed);
        if (firstHeartbeat_.exchange(false))
            logx::Info("Frame capture: events flowing (%.0f ms after start)",
                       1000.0 * ((double)h.TimeStamp.QuadPart - (double)startQpc_) / qpcFreq_);
        return;
    }
    events_.fetch_add(1, std::memory_order_relaxed);

    if (pid == 0 || pid == 4 || pid == selfPid_) return;
    const USHORT id = h.EventDescriptor.Id;
    const uint64_t qpc = (uint64_t)h.TimeStamp.QuadPart;

    if (IsEqualGUID(h.ProviderId, kDxgiProvider)) {
        if (id != kDxgiPresentStart && id != kDxgiPresentMpoStart) return;
        // Present_Start: pIDXGISwapChain, Flags, SyncInterval
        const bool is64 = (h.Flags & EVENT_HEADER_FLAG_64_BIT_HEADER) != 0;
        const uint64_t chain = ReadPointer(rec, 0);
        const uint32_t flags = ReadU32(rec, is64 ? 8 : 4);
        if (flags & kDxgiPresentTest) return;
        OnPresent(pid, PresentSource::Dxgi, qpc, chain);
    } else if (IsEqualGUID(h.ProviderId, kD3d9Provider)) {
        if (id != kD3d9PresentStart) return;
        OnPresent(pid, PresentSource::D3D9, qpc, ReadPointer(rec, 0));
    } else if (IsEqualGUID(h.ProviderId, kDxgkProvider)) {
        // Present_Info fires once per kernel present call. Blit/Flip fire for the same call,
        // so they are tracked as a separate source and never added on top (that would double
        // the frame rate of Vulkan and OpenGL games).
        if (id == kDxgkPresentInfo)
            OnPresent(pid, PresentSource::DxgkPresent, qpc, 0);
        else if (id == kDxgkFlipInfo || id == kDxgkBlitInfo || id == kDxgkFlipMpoInfo)
            OnPresent(pid, PresentSource::DxgkFlipBlt, qpc, 0);
    }
}

void FrameCapture::OnPresent(DWORD pid, PresentSource srcId, uint64_t qpc, uint64_t chainKey)
{
    const uint64_t oneSec = (uint64_t)qpcFreq_;
    const uint64_t timeoutTicks = (uint64_t)(qpcFreq_ * kSourceTimeoutSec);
    const uint64_t gapTicks = (uint64_t)(qpcFreq_ * kGapSeconds);

    std::lock_guard<std::mutex> lock(mutex_);

    if (qpc - lastPurgeQpc_ > 5 * oneSec) {
        PurgeIdleLocked(qpc);
        lastPurgeQpc_ = qpc;
    }

    auto& slot = procs_[pid];
    if (!slot) slot = std::make_unique<Proc>();
    Proc& p = *slot;
    Source& src = p.src[(int)srcId];

    // Pick the swap chain slot for this present (per-source; only DXGI/D3D9 have keys).
    Chain* chain = nullptr;
    for (Chain& c : src.chains)
        if (c.lastQpc && c.key == chainKey) { chain = &c; break; }
    if (!chain) {
        // Reuse the stalest slot.
        chain = &src.chains[0];
        for (Chain& c : src.chains)
            if (c.lastQpc < chain->lastQpc) chain = &c;
        *chain = Chain();
        chain->key = chainKey;
        chain->windowStart = qpc;
    }

    const uint64_t prevChainQpc = chain->lastQpc;
    chain->lastQpc = qpc;
    if (qpc - chain->windowStart >= oneSec) {
        chain->lastRate = chain->windowCount;
        chain->windowCount = 0;
        chain->windowStart = qpc;
    }
    ++chain->windowCount;
    src.lastQpc = qpc;

    // The main chain is the busiest one that is still alive (games sometimes keep a second,
    // nearly idle swap chain for a launcher or splash window).
    const Chain* main = nullptr;
    uint32_t best = 0;
    for (const Chain& c : src.chains) {
        if (!c.lastQpc || qpc - c.lastQpc > timeoutTicks) continue;
        const uint32_t rate = c.lastRate > c.windowCount ? c.lastRate : c.windowCount;
        if (!main || rate > best) { main = &c; best = rate; }
    }

    // Highest-priority source that is still firing wins.
    PresentSource active = PresentSource::None;
    for (int i = 0; i < (int)PresentSource::Count; ++i) {
        if (p.src[i].lastQpc && qpc - p.src[i].lastQpc <= timeoutTicks) { active = (PresentSource)i; break; }
    }
    p.active = active;

    if (srcId != active || chain != main) return;
    if (!prevChainQpc || qpc <= prevChainQpc) return;
    const uint64_t dt = qpc - prevChainQpc;
    if (dt > gapTicks) return;

    p.ring.Push(QpcToSec(qpc), (float)(1000.0 * (double)dt / qpcFreq_));
    p.lastFrameQpc = qpc;
}

void FrameCapture::PurgeIdleLocked(uint64_t nowQpc)
{
    const uint64_t limit = (uint64_t)(qpcFreq_ * kPurgeAfterSec);
    for (auto it = procs_.begin(); it != procs_.end();) {
        uint64_t last = 0;
        for (const Source& s : it->second->src)
            if (s.lastQpc > last) last = s.lastQpc;
        if (nowQpc > last && nowQpc - last > limit) it = procs_.erase(it);
        else ++it;
    }
}

FrameCapture::Result FrameCapture::Query(DWORD pid, double avgWindowSec, double lowsWindowSec,
                                         std::vector<float>* graph, size_t graphCount) const
{
    Result r;
    if (graph) graph->clear();
    const double now = Now();

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = procs_.find(pid);
    if (it == procs_.end()) return r;
    const Proc& p = *it->second;
    r.source = p.active;
    if (!p.lastFrameQpc) return r;
    const double last = QpcToSec(p.lastFrameQpc);
    r.secondsSinceLastFrame = now - last;
    // Windows are anchored at the newest frame, not at "now": ETW hands events over in
    // batches, so "now" is always a little ahead of the data.
    r.stats = ComputeFrameStats(p.ring, last, avgWindowSec, lowsWindowSec);
    if (graph && graphCount) CopyRecentFrameTimes(p.ring, graphCount, *graph);
    return r;
}

std::string FrameCapture::Describe(DWORD pid) const
{
    const double now = Now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = procs_.find(pid);
    if (it == procs_.end()) return "no present events from this process";
    const Proc& p = *it->second;
    std::string out;
    static const char* kNames[] = { "dxgi", "d3d9", "kernel-present", "kernel-flip" };
    for (int i = 0; i < (int)PresentSource::Count; ++i) {
        const Source& s = p.src[i];
        char part[96];
        if (!s.lastQpc) {
            snprintf(part, sizeof(part), "%s none", kNames[i]);
        } else {
            uint32_t rate = 0;
            for (const Chain& c : s.chains) rate = std::max(rate, std::max(c.lastRate, c.windowCount));
            snprintf(part, sizeof(part), "%s %u/s (%.2f s ago)", kNames[i], rate, now - QpcToSec(s.lastQpc));
        }
        if (!out.empty()) out += ", ";
        out += part;
    }
    char tail[128];
    snprintf(tail, sizeof(tail), "; using %s, last counted frame %.2f s ago", PresentSourceName(p.active),
             p.lastFrameQpc ? now - QpcToSec(p.lastFrameQpc) : -1.0);
    return out + tail;
}

std::string FrameCapture::SessionStats() const
{
    TraceProps tp;
    InitProps(tp);
    const ULONG rc = ControlTraceW(0, sessionName_.c_str(), &tp.p, EVENT_TRACE_CONTROL_QUERY);
    char out[200];
    if (rc != ERROR_SUCCESS)
        snprintf(out, sizeof(out), "session query failed: %lu", rc);
    else
        snprintf(out, sizeof(out), "%llu events, %lu buffers written, %lu events lost, %lu buffers lost, %lu of %lu buffers free",
                 (unsigned long long)events_.load(), tp.p.BuffersWritten, tp.p.EventsLost, tp.p.RealTimeBuffersLost,
                 tp.p.FreeBuffers, tp.p.NumberOfBuffers);
    return out;
}

bool FrameCapture::IsPresenting(DWORD pid, double withinSec) const
{
    const double now = Now();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = procs_.find(pid);
    if (it == procs_.end() || !it->second->lastFrameQpc) return false;
    return now - QpcToSec(it->second->lastFrameQpc) <= withinSec;
}

std::vector<DWORD> FrameCapture::PresentingPids(double withinSec) const
{
    const double now = Now();
    std::vector<DWORD> out;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [pid, proc] : procs_)
        if (proc->lastFrameQpc && now - QpcToSec(proc->lastFrameQpc) <= withinSec) out.push_back(pid);
    return out;
}

void FrameCapture::ResetHistory(DWORD pid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = procs_.find(pid);
    if (it != procs_.end()) it->second->ring.Clear();
}
