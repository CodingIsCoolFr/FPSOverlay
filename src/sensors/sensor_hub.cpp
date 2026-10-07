#include "sensors/sensor_hub.h"
#include "app/log.h"
#include "platform/win_util.h"
#include "sensors/gpu_adapters.h"
#include "sensors/lhm_select.h"
#include "sensors/nvml_reader.h"
#include "sensors/pawnio.h"
#include "sensors/pdh_reader.h"

#include "../../bridge/lhm_bridge.h"

#include <windows.h>

#include <algorithm>
#include <chrono>

bool SensorRequest::operator==(const SensorRequest& o) const
{
    return gpuLoad == o.gpuLoad && gpuTemp == o.gpuTemp && gpuHotspot == o.gpuHotspot &&
           gpuPower == o.gpuPower && gpuClock == o.gpuClock && gpuMemClock == o.gpuMemClock &&
           gpuFan == o.gpuFan && vram == o.vram && cpuLoad == o.cpuLoad && cpuTemp == o.cpuTemp &&
           cpuPower == o.cpuPower && cpuClock == o.cpuClock && cpuFan == o.cpuFan && ram == o.ram &&
           wantChoices == o.wantChoices && dumpTemps == o.dumpTemps && gpuKey == o.gpuKey && cpuTempPref == o.cpuTempPref &&
           cpuFanPref == o.cpuFanPref && intervalMs == o.intervalMs &&
           cpuSampleStepMs == o.cpuSampleStepMs;
}

namespace {

constexpr int kLhmFlags = LHM_HW_CPU | LHM_HW_GPU | LHM_HW_MOTHERBOARD;
constexpr ULONGLONG kLhmGpuMinIntervalMs = 2000;    // the LHM NVIDIA update alone costs ~60 ms

// LibreHardwareMonitor through FPSOverlay.Sensors.dll. The DLL hosts the .NET runtime, which
// cannot be unloaded again, so it is loaded once and kept.
class LhmClient {
public:
    bool Load(std::string& err)
    {
        if (lib_) return true;
        const std::wstring path = win::ExeDir() + L"FPSOverlay.Sensors.dll";
        lib_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!lib_) {
            err = "FPSOverlay.Sensors.dll could not be loaded (error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        version_ = reinterpret_cast<lhm_version_fn>(GetProcAddress(lib_, "lhm_version"));
        open_ = reinterpret_cast<lhm_open_fn>(GetProcAddress(lib_, "lhm_open"));
        close_ = reinterpret_cast<lhm_close_fn>(GetProcAddress(lib_, "lhm_close"));
        count_ = reinterpret_cast<lhm_sensor_count_fn>(GetProcAddress(lib_, "lhm_sensor_count"));
        info_ = reinterpret_cast<lhm_sensor_info_fn>(GetProcAddress(lib_, "lhm_sensor_info"));
        update_ = reinterpret_cast<lhm_update_fn>(GetProcAddress(lib_, "lhm_update"));
        value_ = reinterpret_cast<lhm_value_fn>(GetProcAddress(lib_, "lhm_value"));
        if (!version_ || !open_ || !close_ || !count_ || !info_ || !update_ || !value_ ||
            version_() != LHM_BRIDGE_VERSION) {
            err = "FPSOverlay.Sensors.dll does not match this version of the app.";
            open_ = nullptr;
            return false;
        }
        return true;
    }

    bool Open(int flags, std::string& err)
    {
        if (!open_) return false;
        char buf[512] = {};
        if (!open_(flags, buf, sizeof(buf))) {
            err = buf[0] ? buf : "LibreHardwareMonitor failed to start.";
            return false;
        }
        sensors_.clear();
        const int n = count_();
        sensors_.reserve(n > 0 ? n : 0);
        for (int i = 0; i < n; ++i) {
            LhmSensorInfo si = {};
            LhmSensor s;
            if (info_(i, &si)) {
                s.hardware = si.hardware;
                s.hwName = si.hardwareName;
                s.hwType = si.hardwareType;
                s.name = si.name;
                s.type = si.type;
                s.id = si.id;
            }
            sensors_.push_back(std::move(s));
        }
        open = true;
        return true;
    }

    void Close()
    {
        if (close_ && open) close_();
        open = false;
        sensors_.clear();
    }

    bool  Update(int hw) { return open && update_(hw) != 0; }
    float Value(int sensor) { return open && sensor >= 0 ? value_(sensor) : kNoValue; }
    const std::vector<LhmSensor>& Sensors() const { return sensors_; }

    bool open = false;

private:
    HMODULE lib_ = nullptr;
    lhm_version_fn version_ = nullptr;
    lhm_open_fn open_ = nullptr;
    lhm_close_fn close_ = nullptr;
    lhm_sensor_count_fn count_ = nullptr;
    lhm_sensor_info_fn info_ = nullptr;
    lhm_update_fn update_ = nullptr;
    lhm_value_fn value_ = nullptr;
    std::vector<LhmSensor> sensors_;
};

std::string CpuNameFromRegistry()
{
    wchar_t buf[256] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return "Unknown processor";
    std::string s = win::ToUtf8(buf);
    while (!s.empty() && s.front() == ' ') s.erase(0, 1);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

float CpuLoadFromSystemTimes()
{
    static ULONGLONG lastIdle = 0, lastTotal = 0;
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return kNoValue;
    auto u64 = [](const FILETIME& f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    const ULONGLONG i = u64(idle), t = u64(kernel) + u64(user);
    const ULONGLONG di = i - lastIdle, dt = t - lastTotal;
    const bool first = lastTotal == 0;
    lastIdle = i;
    lastTotal = t;
    if (first || dt == 0) return kNoValue;
    return 100.f * (1.f - (float)di / (float)dt);
}

} // namespace

struct SensorHub::Impl {
    PdhReader pdh;
    NvmlReader nvml;
    LhmClient lhm;
    bool nvmlTried = false;
    bool lhmTried = false;
    std::string lhmError;
    LhmPicks picks;
    std::string picksTempPref, picksFanPref;

    std::vector<AdapterEntry> adapters;
    int adapter = -1;
    int nvmlDevice = -1;
    int lhmGpu = -1;
    std::string adapterKeyUsed = "\x01";    // forces the first pick
    ULONGLONG lastLhmGpuUpdate = 0;
    LhmPicks lastGpuPicks;
    GpuReadings lhmGpuCache;

    std::string cpuName;
    ULONGLONG lastPawnioCheck = 0;

    // CPU readings taken between ticks; each tick shows their mean (see CpuSample).
    int cpuHw = -1;
    int cpuTempIdx = -1, cpuPowerIdx = -1;
    double tempSum = 0, powerSum = 0;
    int tempCount = 0, powerCount = 0;
    float cpuSampleMs = 0.f;

    void CpuSample()
    {
        if (cpuHw < 0 || !lhm.open) return;
        const auto t0 = std::chrono::steady_clock::now();
        lhm.Update(cpuHw);
        cpuSampleMs = (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const float t = lhm.Value(cpuTempIdx);
        if (t == t && t > 0.f) { tempSum += t; ++tempCount; }
        const float w = lhm.Value(cpuPowerIdx);
        if (w == w && w > 0.05f) { powerSum += w; ++powerCount; }    // without the driver LHM reports 0 W
    }
    // Mean of the readings since the last call, then starts over.
    void TakeCpuMeans(float& temp, float& power)
    {
        temp = tempCount ? (float)(tempSum / tempCount) : kNoValue;
        power = powerCount ? (float)(powerSum / powerCount) : kNoValue;
        tempSum = powerSum = 0;
        tempCount = powerCount = 0;
    }
};

SensorHub::SensorHub() : impl_(std::make_unique<Impl>()) {}
SensorHub::~SensorHub() { Stop(); }

void SensorHub::Start(const SensorRequest& req)
{
    if (running_) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_ = req;
        requestChanged_ = true;
    }
    running_ = true;
    thread_ = std::thread([this] { Run(); });
}

void SensorHub::Stop()
{
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SensorHub::SetRequest(const SensorRequest& req)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_ == req) return;
        request_ = req;
        requestChanged_ = true;
    }
    cv_.notify_all();
}

SensorSnapshot SensorHub::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void SensorHub::InstallPawnIo()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        installPawnIo_ = true;
        snapshot_.status.pawnioBusy = true;
        snapshot_.status.pawnioMessage.clear();
        seq_.fetch_add(1000000, std::memory_order_acq_rel);     // force readers to re-fetch
    }
    cv_.notify_all();
}

void SensorHub::Run()
{
    SetThreadDescription(GetCurrentThread(), L"Sensors");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Impl& m = *impl_;

    m.cpuName = CpuNameFromRegistry();
    m.adapters = EnumerateGpuAdapters();
    m.pdh.Open();
    CpuLoadFromSystemTimes();   // prime

    SensorStatus status;
    std::string pawnioMessage;
    int pawnioResult = 0;

    while (running_) {
        const auto tickStart = std::chrono::steady_clock::now();
        SensorRequest req;
        bool doInstall = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            req = request_;
            requestChanged_ = false;
            doInstall = installPawnIo_;
            installPawnIo_ = false;
        }

        // ── PawnIO install (blocks this thread only) ──
        if (doInstall) {
            m.lhm.Close();          // the driver cannot be replaced while LHM holds it open
            std::string detail;
            const auto r = pawnio::RunInstaller(detail);
            pawnioResult = (r == pawnio::InstallResult::Ok) ? 1 : 2;
            pawnioMessage = detail;
            m.lhmTried = false;     // reopen with the new driver
            m.picksTempPref = "\x01";
        }

        // ── GPU adapter selection ──
        if (req.gpuKey != m.adapterKeyUsed) {
            m.adapterKeyUsed = req.gpuKey;
            m.adapter = PickAdapter(m.adapters, req.gpuKey);
            m.nvmlDevice = -1;
            m.lhmGpu = -1;
            m.lhmGpuCache = GpuReadings();
            if (m.adapter >= 0) {
                const AdapterEntry& a = m.adapters[m.adapter];
                if (a.info.vendorId == 0x10DE) {
                    if (!m.nvmlTried) {
                        m.nvmlTried = true;
                        status.nvml = m.nvml.Open() ? SourceState::Ok : SourceState::Failed;
                    }
                    if (m.nvml.IsOpen()) m.nvmlDevice = m.nvml.Find(a.hasPci, a.pciBus, a.pciDevice, a.info.name);
                }
                m.lhmGpu = MatchLhmGpu(m.picks, a.info.name, a.info.vendorId);
            }
        }
        const bool nvidiaFast = m.nvmlDevice >= 0;

        // ── LibreHardwareMonitor (lazy) ──
        const bool needLhm = req.cpuTemp || req.cpuPower || req.cpuFan || req.gpuHotspot || req.wantChoices ||
                             (!nvidiaFast && (req.gpuTemp || req.gpuPower || req.gpuClock || req.gpuMemClock || req.gpuFan));
        // Publish a first snapshot from the fast sources before paying LHM's startup cost.
        if (needLhm && !m.lhmTried && snapshot_.seq > 0) {
            m.lhmTried = true;
            status.lhm = SourceState::Starting;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.status.lhm = SourceState::Starting;
            }
            const auto t0 = std::chrono::steady_clock::now();
            std::string err;
            if (m.lhm.Load(err) && m.lhm.Open(kLhmFlags, err)) {
                status.lhm = SourceState::Ok;
                m.lhmError.clear();
                logx::Info("LibreHardwareMonitor ready: %u sensors in %.0f ms", (unsigned)m.lhm.Sensors().size(),
                           std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            } else {
                status.lhm = SourceState::Failed;
                m.lhmError = err;
                logx::Warn("LibreHardwareMonitor unavailable: %s", err.c_str());
            }
            m.picksTempPref = "\x01";   // force re-selection
        }
        if (m.lhm.open && (req.cpuTempPref != m.picksTempPref || req.cpuFanPref != m.picksFanPref)) {
            m.picks = SelectLhmSensors(m.lhm.Sensors(), req.cpuTempPref, req.cpuFanPref);
            m.picksTempPref = req.cpuTempPref;
            m.picksFanPref = req.cpuFanPref;
            if (m.adapter >= 0)
                m.lhmGpu = MatchLhmGpu(m.picks, m.adapters[m.adapter].info.name, m.adapters[m.adapter].info.vendorId);
        }

        SensorSnapshot snap;
        snap.cpuName = m.cpuName;
        for (const auto& a : m.adapters) snap.adapters.push_back(a.info);
        snap.activeAdapter = m.adapter;

        // ── System memory ──
        MEMORYSTATUSEX ms = { sizeof(ms) };
        if (GlobalMemoryStatusEx(&ms)) {
            snap.ram.totalGB = (float)(ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
            snap.ram.usedGB = (float)((ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0));
        }

        // ── Performance counters ──
        m.pdh.Sample();
        status.pdh = m.pdh.IsOpen() ? SourceState::Ok : SourceState::Failed;
        snap.cpu.load = m.pdh.CpuUtility();
        const float sysLoad = CpuLoadFromSystemTimes();
        if (!Has(snap.cpu.load)) snap.cpu.load = sysLoad;
        snap.cpu.clock = m.pdh.CpuClockMHz();

        // ── GPU ──
        GpuReadings& g = snap.gpu;
        if (m.adapter >= 0) {
            const AdapterEntry& a = m.adapters[m.adapter];
            if (nvidiaFast) m.nvml.Read(m.nvmlDevice, g);
            const std::string luid = LuidKey(a.luid);
            const float pdhLoad = m.pdh.GpuLoad(luid);
            if (!Has(g.load)) g.load = pdhLoad;
            // Dedicated usage from the OS matches Task Manager; NVML's "used" includes reserved memory.
            const float pdhUsed = m.pdh.GpuDedicatedUsedMB(luid);
            if (Has(pdhUsed)) g.vramUsedMB = pdhUsed;
            if (a.info.dedicatedBytes) g.vramTotalMB = (float)(a.info.dedicatedBytes / (1024.0 * 1024.0));
        }

        // ── LibreHardwareMonitor readings ──
        if (m.lhm.open) {
            const auto l0 = std::chrono::steady_clock::now();
            const auto& sensors = m.lhm.Sensors();
            int cpuHw = -1;
            if (req.cpuTemp && m.picks.cpuTemp >= 0) cpuHw = sensors[m.picks.cpuTemp].hardware;
            else if (req.cpuPower && m.picks.cpuPower >= 0) cpuHw = sensors[m.picks.cpuPower].hardware;
            const int tempIdx = req.cpuTemp ? m.picks.cpuTemp : -1;
            const int powerIdx = req.cpuPower ? m.picks.cpuPower : -1;
            if (cpuHw != m.cpuHw || tempIdx != m.cpuTempIdx || powerIdx != m.cpuPowerIdx) {
                float drop1, drop2;
                m.TakeCpuMeans(drop1, drop2);   // readings of another sensor: discard
            }
            m.cpuHw = cpuHw;
            m.cpuTempIdx = tempIdx;
            m.cpuPowerIdx = powerIdx;
            m.CpuSample();
            float meanTemp, meanPower;
            m.TakeCpuMeans(meanTemp, meanPower);
            if (req.cpuTemp) snap.cpu.temp = meanTemp;
            if (req.cpuPower) snap.cpu.power = meanPower;
            status.cpuSampleMs = cpuHw >= 0 ? m.cpuSampleMs : 0.f;
            if (req.cpuFan && m.picks.cpuFan >= 0) {
                m.lhm.Update(sensors[m.picks.cpuFan].hardware);
                snap.cpu.fanRpm = m.lhm.Value(m.picks.cpuFan);
            }

            // GPU through LHM: only what the fast paths could not provide, at most every 2 s.
            if (m.lhmGpu >= 0 && m.lhmGpu < (int)m.picks.gpus.size()) {
                const LhmGpuPick& gp = m.picks.gpus[m.lhmGpu];
                const bool wantSlow = req.gpuHotspot || req.gpuFan ||
                                      (!nvidiaFast && (req.gpuTemp || req.gpuPower || req.gpuClock ||
                                                       req.gpuMemClock || req.gpuLoad || req.vram));
                const ULONGLONG nowMs = GetTickCount64();
                if (wantSlow && nowMs - m.lastLhmGpuUpdate >= (nvidiaFast ? kLhmGpuMinIntervalMs : 0)) {
                    m.lastLhmGpuUpdate = nowMs;
                    m.lhm.Update(gp.hardware);
                    GpuReadings& c = m.lhmGpuCache;
                    c.temp = m.lhm.Value(gp.temp);
                    c.hotspot = m.lhm.Value(gp.hotspot);
                    c.power = m.lhm.Value(gp.power);
                    c.coreClock = m.lhm.Value(gp.coreClock);
                    c.memClock = m.lhm.Value(gp.memClock);
                    c.fanRpm = m.lhm.Value(gp.fan);
                    c.load = m.lhm.Value(gp.load);
                    c.vramUsedMB = m.lhm.Value(gp.vramUsed);
                    c.vramTotalMB = m.lhm.Value(gp.vramTotal);
                }
                const GpuReadings& c = m.lhmGpuCache;
                auto fill = [](float& dst, float src) { if (!Has(dst)) dst = src; };
                fill(g.temp, c.temp);
                fill(g.hotspot, c.hotspot);
                fill(g.power, c.power);
                fill(g.coreClock, c.coreClock);
                fill(g.memClock, c.memClock);
                fill(g.load, c.load);
                fill(g.vramUsedMB, c.vramUsedMB);
                fill(g.vramTotalMB, c.vramTotalMB);
                if (Has(c.fanRpm)) g.fanRpm = c.fanRpm;
            }
            status.lhmUpdateMs = (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - l0).count();

            for (int idx : m.picks.cpuTempChoices)
                snap.cpuTempChoices.push_back({ sensors[idx].id, sensors[idx].name });
            for (int idx : m.picks.fanChoices)
                snap.fanChoices.push_back({ sensors[idx].id, sensors[idx].hwName + " / " + sensors[idx].name });
            if (m.picks.cpuTemp >= 0) snap.cpuTempSensorUsed = sensors[m.picks.cpuTemp].id;
            if (m.picks.cpuFan >= 0) snap.cpuFanSensorUsed = sensors[m.picks.cpuFan].id;

            if (req.dumpTemps) {
                std::vector<int> updated;
                for (int i = 0; i < (int)sensors.size(); ++i) {
                    const LhmSensor& s = sensors[i];
                    if (s.type != "Temperature") continue;
                    if (std::find(updated.begin(), updated.end(), s.hardware) == updated.end()) {
                        updated.push_back(s.hardware);
                        m.lhm.Update(s.hardware);
                    }
                    char line[320];
                    snprintf(line, sizeof(line), "%-11s %-32.32s %-30.30s %6.1f  %s%s", s.hwType.c_str(), s.hwName.c_str(),
                             s.name.c_str(), m.lhm.Value(i), s.id.c_str(), i == m.picks.cpuTemp ? "  <- shown as CPU" : "");
                    snap.tempDump.push_back(line);
                }
            }
        }

        // ── Status ──
        status.lhmError = m.lhmError;
        const ULONGLONG nowTick = GetTickCount64();
        if (doInstall || m.lastPawnioCheck == 0 || nowTick - m.lastPawnioCheck > 10000) {
            m.lastPawnioCheck = nowTick;
            status.pawnioInstalled = pawnio::IsInstalled();
        }
        if (status.pawnioInstalled && (doInstall || status.pawnioVersion.empty())) {
            const auto inst = pawnio::InstalledVersion();
            status.pawnioVersion = inst.valid() ? inst.ToString() : std::string();
            const auto bundled = pawnio::BundledVersion();
            status.pawnioOutdated = inst.valid() && bundled.valid() && pawnio::Compare(inst, bundled) < 0;
        } else if (!status.pawnioInstalled) {
            status.pawnioVersion.clear();
            status.pawnioOutdated = false;
        }
        status.pawnioMessage = pawnioMessage;
        status.pawnioResult = pawnioResult;
        status.cpuTempAvailable = Has(snap.cpu.temp);
        status.tickMs = (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tickStart).count();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            snap.seq = snapshot_.seq + 1;
            snap.status = status;
            snap.status.pawnioBusy = installPawnIo_;
            snapshot_ = std::move(snap);
            seq_.store(snapshot_.seq, std::memory_order_release);
        }

        std::unique_lock<std::mutex> lock(mutex_);
        // The very first wait is short so the deferred LHM start happens right away.
        const int waitMs = (needLhm && !m.lhmTried) ? 50 : req.intervalMs;
        const auto wake = [this] { return !running_ || requestChanged_ || installPawnIo_; };
        const auto until = tickStart + std::chrono::milliseconds(waitMs);
        // A CPU's temperature jumps several degrees from one moment to the next, because boost
        // bursts last milliseconds. Reading it four times a second and showing the mean gives
        // the real temperature over the interval, not one random moment of it.
        bool woken = false;
        const std::chrono::milliseconds step(req.cpuSampleStepMs);
        if (m.cpuHw >= 0 && m.lhm.open && step.count() > 0) {
            for (auto next = tickStart + step; next + step / 2 < until; next += step) {
                if (cv_.wait_until(lock, next, wake)) {
                    woken = true;
                    break;
                }
                lock.unlock();
                m.CpuSample();
                lock.lock();
            }
        }
        if (!woken) cv_.wait_until(lock, until, wake);
    }

    m.lhm.Close();
    m.nvml.Close();
    m.pdh.Close();
}
