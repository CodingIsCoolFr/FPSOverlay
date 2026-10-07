#include "sensors/pdh_reader.h"
#include "app/log.h"

#include <PdhMsg.h>

#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

namespace {

float RegistryBaseMHz()
{
    DWORD mhz = 0, size = sizeof(mhz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz",
                     RRF_RT_REG_DWORD, nullptr, &mhz, &size) == ERROR_SUCCESS)
        return (float)mhz;
    return 0.f;
}

// "..._luid_0x00000000_0x0000D1F2_phys_0..." -> "luid:00000000:0000d1f2"
bool ParseLuid(const wchar_t* inst, std::string& key, const wchar_t** after)
{
    const wchar_t* p = wcsstr(inst, L"luid_0x");
    if (!p) return false;
    unsigned long hi = 0, lo = 0;
    if (swscanf_s(p, L"luid_0x%lx_0x%lx", &hi, &lo) != 2) return false;
    char buf[48];
    snprintf(buf, sizeof(buf), "luid:%08lx:%08lx", hi, lo);
    key = buf;
    if (after) *after = p;
    return true;
}

bool GetArray(PDH_HCOUNTER counter, std::vector<BYTE>& storage, PDH_FMT_COUNTERVALUE_ITEM_W*& items, DWORD& count)
{
    DWORD bytes = 0;
    count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
    if (st != PDH_MORE_DATA || bytes == 0) return false;
    storage.resize(bytes);
    items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(storage.data());
    st = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
    return st == ERROR_SUCCESS;
}

} // namespace

PdhReader::~PdhReader() { Close(); }

bool PdhReader::Open()
{
    if (query_) return true;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
        query_ = nullptr;
        logx::Warn("PdhOpenQuery failed");
        return false;
    }
    // English names work on every display language.
    if (PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Utility", 0, &cpuUtilCounter_) != ERROR_SUCCESS)
        cpuUtilCounter_ = nullptr;
    if (PdhAddEnglishCounterW(query_, L"\\Processor Information(_Total)\\% Processor Performance", 0, &cpuPerfCounter_) != ERROR_SUCCESS)
        cpuPerfCounter_ = nullptr;
    if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpuEngineCounter_) != ERROR_SUCCESS)
        gpuEngineCounter_ = nullptr;
    if (PdhAddEnglishCounterW(query_, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &gpuMemCounter_) != ERROR_SUCCESS)
        gpuMemCounter_ = nullptr;

    baseMHz_ = RegistryBaseMHz();
    samples_ = 0;
    lastReexpand_ = GetTickCount64();
    logx::Info("PDH open: cpuUtil=%d cpuPerf=%d gpuEngine=%d gpuMem=%d baseMHz=%.0f",
               cpuUtilCounter_ != nullptr, cpuPerfCounter_ != nullptr, gpuEngineCounter_ != nullptr,
               gpuMemCounter_ != nullptr, baseMHz_);
    return true;
}

void PdhReader::Close()
{
    if (query_) PdhCloseQuery(query_);
    query_ = nullptr;
    cpuUtilCounter_ = cpuPerfCounter_ = gpuEngineCounter_ = gpuMemCounter_ = nullptr;
    gpuLoad_.clear();
    gpuMemMB_.clear();
}

void PdhReader::Sample()
{
    if (!query_) return;

    // Wildcard instances are fixed when a counter is added, so games started later would be
    // missing from the GPU engine list. Re-adding the counter every few seconds picks them up.
    const ULONGLONG now = GetTickCount64();
    if (gpuEngineCounter_ && now - lastReexpand_ > 5000) {
        lastReexpand_ = now;
        PdhRemoveCounter(gpuEngineCounter_);
        gpuEngineCounter_ = nullptr;
        if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &gpuEngineCounter_) != ERROR_SUCCESS)
            gpuEngineCounter_ = nullptr;
        // The re-added rate counter needs one priming sample; take it now so this tick still
        // produces a value on the next call.
    }

    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return;
    ++samples_;

    PDH_FMT_COUNTERVALUE v = {};
    cpuUtility_ = NAN;
    if (cpuUtilCounter_ && PdhGetFormattedCounterValue(cpuUtilCounter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS &&
        v.CStatus == ERROR_SUCCESS) {
        double u = v.doubleValue;
        if (u < 0) u = 0;
        if (u > 100) u = 100;     // Task Manager caps the same way
        cpuUtility_ = (float)u;
    }
    cpuClock_ = NAN;
    if (cpuPerfCounter_ && baseMHz_ > 0 &&
        PdhGetFormattedCounterValue(cpuPerfCounter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) == ERROR_SUCCESS &&
        v.CStatus == ERROR_SUCCESS && v.doubleValue > 0) {
        cpuClock_ = (float)(baseMHz_ * v.doubleValue / 100.0);
    }

    ReadGpuEngines();
    ReadGpuMemory();
}

void PdhReader::ReadGpuEngines()
{
    if (!gpuEngineCounter_) return;
    std::vector<BYTE> storage;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
    DWORD count = 0;
    if (!GetArray(gpuEngineCounter_, storage, items, count)) return;

    // Task Manager: GPU % = busiest engine, where an engine's load is the sum over processes.
    std::unordered_map<std::string, double> perEngine;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        std::string luid;
        const wchar_t* tail = nullptr;
        if (!ParseLuid(items[i].szName, luid, &tail)) continue;
        // Engine identity = adapter + "_phys_N_eng_M" (ASCII), summed over all processes.
        std::string engineKey = luid;
        if (const wchar_t* phys = wcsstr(tail, L"_phys_")) {
            const wchar_t* end = wcsstr(phys, L"_engtype_");
            for (const wchar_t* c = phys; *c && c != end; ++c) engineKey.push_back((char)*c);
        }
        perEngine[engineKey] += items[i].FmtValue.doubleValue;
    }
    // A freshly re-added counter has no valid rate yet; keep the previous values for that tick.
    if (perEngine.empty()) return;
    gpuLoad_.clear();
    for (const auto& kv : perEngine) {
        const std::string luid = kv.first.substr(0, 22);
        double v = kv.second;
        if (v > 100) v = 100;
        auto it = gpuLoad_.find(luid);
        if (it == gpuLoad_.end() || v > it->second) gpuLoad_[luid] = (float)v;
    }
}

void PdhReader::ReadGpuMemory()
{
    if (!gpuMemCounter_) return;
    std::vector<BYTE> storage;
    PDH_FMT_COUNTERVALUE_ITEM_W* items = nullptr;
    DWORD count = 0;
    if (!GetArray(gpuMemCounter_, storage, items, count)) return;
    gpuMemMB_.clear();
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != ERROR_SUCCESS) continue;
        std::string luid;
        if (!ParseLuid(items[i].szName, luid, nullptr)) continue;
        gpuMemMB_[luid] += (float)(items[i].FmtValue.doubleValue / (1024.0 * 1024.0));
    }
}

float PdhReader::GpuLoad(const std::string& luidKey) const
{
    auto it = gpuLoad_.find(luidKey);
    return it == gpuLoad_.end() ? NAN : it->second;
}

float PdhReader::GpuDedicatedUsedMB(const std::string& luidKey) const
{
    auto it = gpuMemMB_.find(luidKey);
    return it == gpuMemMB_.end() ? NAN : it->second;
}
