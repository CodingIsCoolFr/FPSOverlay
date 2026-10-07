// Plain data published by the sensor thread. NaN means "no reading".
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

inline constexpr float kNoValue = NAN;
inline bool Has(float v) { return !std::isnan(v); }

struct GpuReadings {
    float load = kNoValue;          // %
    float temp = kNoValue;          // °C
    float hotspot = kNoValue;       // °C
    float power = kNoValue;         // W
    float coreClock = kNoValue;     // MHz
    float memClock = kNoValue;      // MHz
    float fanRpm = kNoValue;
    float fanPercent = kNoValue;
    float vramUsedMB = kNoValue;
    float vramTotalMB = kNoValue;
};

struct CpuReadings {
    float load = kNoValue;          // %
    float temp = kNoValue;          // °C
    float power = kNoValue;         // W
    float clock = kNoValue;         // MHz, effective
    float fanRpm = kNoValue;
};

struct MemReadings {
    float usedGB = kNoValue;
    float totalGB = kNoValue;
};

struct GpuAdapter {
    std::string key;                // stable id for config: "<name>|pci:01:00.0"
    std::string name;
    uint32_t vendorId = 0;
    uint64_t dedicatedBytes = 0;
    bool integrated = false;
};

// Status of each data source, shown on the settings Sensors page.
enum class SourceState : int { Off = 0, Starting, Ok, Failed };

struct SensorStatus {
    SourceState nvml = SourceState::Off;
    SourceState pdh = SourceState::Off;
    SourceState lhm = SourceState::Off;
    std::string lhmError;
    bool pawnioInstalled = false;
    bool pawnioOutdated = false;    // older than the installer bundled with the app
    bool pawnioBusy = false;        // install running
    std::string pawnioVersion;
    int pawnioResult = 0;           // last install attempt: 0 none, 1 ok, 2 failed
    std::string pawnioMessage;      // failure detail (English)
    bool cpuTempAvailable = false;
    float lhmUpdateMs = 0.f;        // cost of the last LibreHardwareMonitor update
    float cpuSampleMs = 0.f;        // cost of one CPU reading (taken 4 times a second)
    float tickMs = 0.f;             // cost of the last full sensor tick
};

struct LhmSensorChoice {
    std::string id;                 // "/intelcpu/0/temperature/26"
    std::string label;              // "CPU Package"
};

struct SensorSnapshot {
    uint64_t seq = 0;
    std::string cpuName;
    std::vector<GpuAdapter> adapters;
    int activeAdapter = -1;         // index into adapters
    GpuReadings gpu;
    CpuReadings cpu;
    MemReadings ram;
    SensorStatus status;
    std::vector<LhmSensorChoice> cpuTempChoices;   // for the settings dropdowns
    std::vector<LhmSensorChoice> fanChoices;
    std::string cpuTempSensorUsed;                 // identifiers actually used
    std::string cpuFanSensorUsed;
    std::vector<std::string> tempDump;             // every LHM temperature, when SensorRequest::dumpTemps is set
};
