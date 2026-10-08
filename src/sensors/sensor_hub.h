// Owns every hardware data source and polls them on one background thread, so a slow sensor
// can never stall the HUD. The UI reads an immutable snapshot.
#pragma once

#include "sensors/snapshot.h"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct SensorRequest {
    bool gpuLoad = true, gpuTemp = true, gpuHotspot = false, gpuPower = false, gpuClock = false,
         gpuMemClock = false, gpuFan = false, vram = true;
    bool cpuLoad = true, cpuTemp = true, cpuPower = false, cpuClock = false, cpuFan = false;
    bool ram = true;
    bool wantChoices = false;       // settings window is open: load LHM to list sensors
    bool dumpTemps = false;         // diagnostics (--probe): list every temperature LHM has
    std::string gpuKey;
    std::string cpuTempPref;
    std::string cpuFanPref;
    int intervalMs = 1000;
    int cpuSampleStepMs = 100;      // extra CPU temperature readings between ticks (0 = none)
    int cpuMode = 0;                // 0 direct when possible, 1 LibreHardwareMonitor only, 2 LHM 4x/s (diagnostics)

    bool operator==(const SensorRequest& o) const;
};

class SensorHub {
public:
    SensorHub();
    ~SensorHub();

    void Start(const SensorRequest& req);
    void Stop();
    void SetRequest(const SensorRequest& req);

    SensorSnapshot Snapshot() const;
    uint64_t Seq() const { return seq_.load(std::memory_order_acquire); }   // changes with each snapshot

    // Installs or updates the PawnIO driver on the sensor thread, then reloads
    // LibreHardwareMonitor so CPU temperatures appear without restarting the app.
    void InstallPawnIo();

private:
    struct Impl;
    void Run();

    std::unique_ptr<Impl> impl_;
    std::thread thread_;
    std::atomic<bool> running_{ false };
    mutable std::mutex mutex_;          // guards request_, snapshot_, flags below
    std::condition_variable cv_;
    SensorRequest request_;
    bool requestChanged_ = false;
    bool installPawnIo_ = false;
    SensorSnapshot snapshot_;
    std::atomic<uint64_t> seq_{ 0 };
};
