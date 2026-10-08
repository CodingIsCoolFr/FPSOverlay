#include "sensors/sensor_hub.h"
#include "app/log.h"
#include "platform/win_util.h"
#include "sensors/cpu_direct.h"
#include "sensors/gpu_adapters.h"
#include "sensors/lhm_select.h"
#include "sensors/nvml_reader.h"
#include "sensors/pawnio.h"
#include "sensors/pdh_reader.h"

#include "../../bridge/lhm_bridge.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>

bool SensorRequest::operator==(const SensorRequest& o) const
{
    return gpuLoad == o.gpuLoad && gpuTemp == o.gpuTemp && gpuHotspot == o.gpuHotspot &&
           gpuPower == o.gpuPower && gpuClock == o.gpuClock && gpuMemClock == o.gpuMemClock &&
           gpuFan == o.gpuFan && vram == o.vram && cpuLoad == o.cpuLoad && cpuTemp == o.cpuTemp &&
           cpuPower == o.cpuPower && cpuClock == o.cpuClock && cpuFan == o.cpuFan && ram == o.ram &&
           wantChoices == o.wantChoices && dumpTemps == o.dumpTemps && gpuKey == o.gpuKey && cpuTempPref == o.cpuTempPref &&
           cpuFanPref == o.cpuFanPref && intervalMs == o.intervalMs &&
           cpuSampleStepMs == o.cpuSampleStepMs && cpuMode == o.cpuMode;
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

    // ── CPU temperature and power ──
    // Direct: one package register through PawnIO, read 10 times a second (cheap, wakes no core).
    // LHM: LibreHardwareMonitor's CPU update, which moves the thread across every core, so it
    // runs once per tick and only when the direct path cannot give the value asked for.
    CpuDirect direct;
    bool directTried = false;
    ULONGLONG directRetryAt = 0;
    enum class CpuPath { None, Direct, Lhm };
    CpuPath tempPath = CpuPath::None;
    int cpuHw = -1;                 // LHM hardware updated each tick, -1 = none
    int cpuTempIdx = -1, cpuPowerIdx = -1;
    double tempSum = 0, powerSum = 0;
    int tempCount = 0, powerCount = 0;
    bool atLimit = false;
    bool lhmSubsample = false;      // diagnostics: the 2.0.1 behaviour (LHM 4 times a second)
    bool directPowerPrimed = false; // the energy counter was read last tick
    float cpuSampleMs = 0.f;
    float shownTemp = kNoValue;     // display hysteresis

    void CpuSample()
    {
        const auto t0 = std::chrono::steady_clock::now();
        if (tempPath == CpuPath::Direct) {
            float t;
            bool limit = false;
            if (direct.ReadTemperature(t, limit)) {
                tempSum += t;
                ++tempCount;
                atLimit = atLimit || limit;
            }
        }
        if (cpuHw >= 0 && lhm.open) {
            lhm.Update(cpuHw);
            if (tempPath == CpuPath::Lhm) {
                const float t = lhm.Value(cpuTempIdx);
                if (t == t && t > 0.f) { tempSum += t; ++tempCount; }
            }
            const float w = lhm.Value(cpuPowerIdx);
            if (w == w && w > 0.05f) { powerSum += w; ++powerCount; }    // without the driver LHM reports 0 W
        }
        cpuSampleMs = (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    // Readings between ticks: only the direct path, unless diagnostics ask for the old behaviour.
    bool SubsampleWanted() const { return tempPath == CpuPath::Direct || (lhmSubsample && cpuHw >= 0 && lhm.open); }
    void SubSample()
    {
        if (tempPath == CpuPath::Direct && !lhmSubsample) {
            const int hw = cpuHw;
            cpuHw = -1;             // LHM keeps its once-per-tick pace
            CpuSample();
            cpuHw = hw;
        } else {
            CpuSample();
        }
    }
    // Mean of the readings since the last call, then starts over.
    void TakeCpuMeans(float& temp, float& power, bool& limit)
    {
        temp = tempCount ? (float)(tempSum / tempCount) : kNoValue;
        power = powerCount ? (float)(powerSum / powerCount) : kNoValue;
        limit = atLimit;
        tempSum = powerSum = 0;
        tempCount = powerCount = 0;
        atLimit = false;
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
            m.direct.Close();
            m.directRetryAt = 0;
            m.directTried = false;
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
        // ── Direct CPU reading (decided first: it can make LHM unnecessary) ──
        if ((req.cpuTemp || req.cpuPower) && req.cpuMode == 0 && !m.direct.IsOpen() && GetTickCount64() >= m.directRetryAt) {
            const bool first = !m.directTried;
            m.directTried = true;
            m.directRetryAt = GetTickCount64() + 30000;     // PawnIO may be installed outside the app later
            std::string err;
            if (m.direct.Open(err)) logx::Info("CPU temperature: read directly, %s", m.direct.Description().c_str());
            else if (first) logx::Info("CPU temperature: no direct read (%s), using LibreHardwareMonitor", err.c_str());
        }
        const bool directOk = m.direct.IsOpen() && req.cpuMode == 0;
        const bool needLhm = (req.cpuTemp && !(directOk && req.cpuTempPref.empty())) ||
                             (req.cpuPower && !(directOk && m.direct.HasPower())) || req.cpuFan || req.gpuHotspot ||
                             req.wantChoices ||
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

        // ── CPU temperature and power ──
        {
            const auto& sensors = m.lhm.Sensors();     // empty until LHM is open
            // A preference names an LHM sensor; the package ones are what the direct path reads.
            bool prefIsPackage = req.cpuTempPref.empty();
            if (!prefIsPackage) {
                for (const auto& s : sensors) {
                    if (s.id != req.cpuTempPref) continue;
                    prefIsPackage = s.name == "CPU Package" || s.name == "Package" || s.name == "Core (Tctl/Tdie)" ||
                                    s.name == "Core (Tdie)";
                    break;
                }
            }
            const bool direct = m.direct.IsOpen() && req.cpuMode == 0;
            Impl::CpuPath tempPath = Impl::CpuPath::None;
            if (req.cpuTemp)
                tempPath = (direct && prefIsPackage) ? Impl::CpuPath::Direct : m.lhm.open ? Impl::CpuPath::Lhm : Impl::CpuPath::None;
            const bool directPower = req.cpuPower && direct && m.direct.HasPower();
            const int tempIdx = tempPath == Impl::CpuPath::Lhm ? m.picks.cpuTemp : -1;
            const int powerIdx = (req.cpuPower && !directPower && m.lhm.open) ? m.picks.cpuPower : -1;
            int cpuHw = -1;
            if (tempIdx >= 0) cpuHw = sensors[tempIdx].hardware;
            else if (powerIdx >= 0) cpuHw = sensors[powerIdx].hardware;
            if (tempPath != m.tempPath || cpuHw != m.cpuHw || tempIdx != m.cpuTempIdx || powerIdx != m.cpuPowerIdx) {
                float dropT, dropW;
                bool dropL;
                m.TakeCpuMeans(dropT, dropW, dropL);   // readings from another source: discard
                m.shownTemp = kNoValue;
            }
            m.tempPath = tempPath;
            m.cpuHw = cpuHw;
            m.cpuTempIdx = tempIdx;
            m.cpuPowerIdx = powerIdx;
            m.lhmSubsample = req.cpuMode == 2;
            m.CpuSample();
            float meanTemp, meanPower;
            bool atLimit;
            m.TakeCpuMeans(meanTemp, meanPower, atLimit);
            if (directPower) {
                // The energy counter gives the exact average over the whole tick.
                if (!m.directPowerPrimed) m.direct.PackagePowerW();
                meanPower = m.direct.PackagePowerW();
            }
            m.directPowerPrimed = directPower;
            status.cpuTempRaw = meanTemp;
            // The shown whole degree only moves once the reading is 0.75 °C away from it, so the
            // last digit does not flicker between two values.
            if (!Has(meanTemp)) m.shownTemp = kNoValue;
            else if (!Has(m.shownTemp) || std::fabs(meanTemp - m.shownTemp) >= 0.75f) m.shownTemp = std::round(meanTemp);
            if (req.cpuTemp) {
                snap.cpu.temp = m.shownTemp;
                snap.cpu.atLimit = atLimit;
                if (tempPath == Impl::CpuPath::Direct) snap.cpu.limitC = m.direct.LimitC();
            }
            if (req.cpuPower) snap.cpu.power = meanPower;
            status.cpuSampleMs = m.cpuSampleMs;
            status.cpuTempSource = tempPath == Impl::CpuPath::Direct ? "direct, " + m.direct.Description()
                                 : tempIdx >= 0                       ? "LibreHardwareMonitor, " + sensors[tempIdx].name
                                                                      : std::string("none");
        }

        // ── LibreHardwareMonitor readings ──
        if (m.lhm.open) {
            const auto l0 = std::chrono::steady_clock::now();
            const auto& sensors = m.lhm.Sensors();
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
        // bursts last milliseconds. Reading it 10 times a second and showing the mean gives
        // the real temperature over the interval, not one random moment of it.
        bool woken = false;
        const std::chrono::milliseconds step(req.cpuSampleStepMs);
        if (m.SubsampleWanted() && step.count() > 0) {
            for (auto next = tickStart + step; next + step / 2 < until; next += step) {
                if (cv_.wait_until(lock, next, wake)) {
                    woken = true;
                    break;
                }
                lock.unlock();
                m.SubSample();
                lock.lock();
            }
        }
        if (!woken) cv_.wait_until(lock, until, wake);
    }

    m.lhm.Close();
    m.nvml.Close();
    m.pdh.Close();
}
