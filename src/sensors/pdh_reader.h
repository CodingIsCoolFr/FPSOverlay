// Windows performance counters: the same sources Task Manager uses for CPU utility,
// effective CPU clock, GPU engine load and dedicated video memory. No driver, any vendor.
#pragma once

#include <windows.h>
#include <pdh.h>

#include <string>
#include <unordered_map>

class PdhReader {
public:
    ~PdhReader();
    bool Open();
    void Close();
    bool IsOpen() const { return query_ != nullptr; }

    // Collects one sample. Rate counters need two samples, so the first call returns no CPU data.
    void Sample();

    float CpuUtility() const { return cpuUtility_; }        // %, NaN if unknown
    float CpuClockMHz() const { return cpuClock_; }         // effective MHz, NaN if unknown

    // Per-adapter values keyed by LuidKey(): busiest engine %, dedicated bytes in use.
    float GpuLoad(const std::string& luidKey) const;
    float GpuDedicatedUsedMB(const std::string& luidKey) const;

private:
    void ReadGpuEngines();
    void ReadGpuMemory();

    PDH_HQUERY   query_ = nullptr;
    PDH_HCOUNTER cpuUtilCounter_ = nullptr;
    PDH_HCOUNTER cpuPerfCounter_ = nullptr;
    PDH_HCOUNTER gpuEngineCounter_ = nullptr;
    PDH_HCOUNTER gpuMemCounter_ = nullptr;
    float baseMHz_ = 0.f;
    int   samples_ = 0;
    ULONGLONG lastReexpand_ = 0;

    float cpuUtility_ = NAN;
    float cpuClock_ = NAN;
    std::unordered_map<std::string, float> gpuLoad_;
    std::unordered_map<std::string, float> gpuMemMB_;
};
