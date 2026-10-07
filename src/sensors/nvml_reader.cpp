#include "sensors/nvml_reader.h"
#include "app/log.h"

#include <cstring>

namespace {

struct NvmlPciInfo {                // nvmlPciInfo_t as used by nvmlDeviceGetPciInfo_v3
    char     busIdLegacy[16];
    unsigned domain;
    unsigned bus;
    unsigned device;
    unsigned pciDeviceId;
    unsigned pciSubSystemId;
    char     busId[32];
};
struct NvmlUtilization { unsigned gpu; unsigned memory; };
struct NvmlMemory { unsigned long long total, free, used; };

constexpr int kTempGpu = 0;
constexpr int kClockGraphics = 0;
constexpr int kClockMem = 2;

template <typename T>
bool Bind(HMODULE lib, const char* name, T& fn)
{
    fn = reinterpret_cast<T>(GetProcAddress(lib, name));
    return fn != nullptr;
}

} // namespace

NvmlReader::~NvmlReader() { Close(); }

bool NvmlReader::Open()
{
    if (lib_) return true;
    HMODULE lib = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) lib = LoadLibraryExW(L"C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll", nullptr,
                                   LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib) return false;

    int (*init)() = nullptr;
    int (*getCount)(unsigned*) = nullptr;
    int (*getHandle)(unsigned, void**) = nullptr;
    int (*getName)(void*, char*, unsigned) = nullptr;
    int (*getPci)(void*, NvmlPciInfo*) = nullptr;
    const bool ok = Bind(lib, "nvmlInit_v2", init) && Bind(lib, "nvmlShutdown", shutdown_) &&
                    Bind(lib, "nvmlDeviceGetCount_v2", getCount) &&
                    Bind(lib, "nvmlDeviceGetHandleByIndex_v2", getHandle) &&
                    Bind(lib, "nvmlDeviceGetName", getName) &&
                    Bind(lib, "nvmlDeviceGetPciInfo_v3", getPci) &&
                    Bind(lib, "nvmlDeviceGetTemperature", getTemperature_) &&
                    Bind(lib, "nvmlDeviceGetPowerUsage", getPowerUsage_) &&
                    Bind(lib, "nvmlDeviceGetClockInfo", getClockInfo_) &&
                    Bind(lib, "nvmlDeviceGetFanSpeed", getFanSpeed_) &&
                    Bind(lib, "nvmlDeviceGetUtilizationRates", getUtilization_) &&
                    Bind(lib, "nvmlDeviceGetMemoryInfo", getMemoryInfo_);
    if (!ok || init() != 0) {
        logx::Warn("NVML present but unusable");
        FreeLibrary(lib);
        shutdown_ = nullptr;
        return false;
    }
    lib_ = lib;

    unsigned count = 0;
    getCount(&count);
    for (unsigned i = 0; i < count && i < 16; ++i) {
        Device d;
        if (getHandle(i, &d.handle) != 0) continue;
        char name[96] = {};
        if (getName(d.handle, name, sizeof(name)) == 0) d.name = name;
        NvmlPciInfo pci = {};
        if (getPci(d.handle, &pci) == 0) {
            d.bus = pci.bus;
            d.device = pci.device;
        }
        devices_.push_back(d);
    }
    logx::Info("NVML: %u device(s)", (unsigned)devices_.size());
    return true;
}

void NvmlReader::Close()
{
    if (!lib_) return;
    if (shutdown_) shutdown_();
    FreeLibrary(lib_);
    lib_ = nullptr;
    devices_.clear();
}

int NvmlReader::Find(bool hasPci, unsigned bus, unsigned device, const std::string& name) const
{
    if (hasPci) {
        for (size_t i = 0; i < devices_.size(); ++i)
            if (devices_[i].bus == bus && devices_[i].device == device) return (int)i;
    }
    for (size_t i = 0; i < devices_.size(); ++i) {
        // DXGI says "NVIDIA GeForce RTX 4080", NVML may drop the vendor prefix.
        if (!devices_[i].name.empty() && name.find(devices_[i].name) != std::string::npos) return (int)i;
    }
    return -1;
}

void NvmlReader::Read(int device, GpuReadings& out) const
{
    if (!lib_ || device < 0 || device >= (int)devices_.size()) return;
    void* h = devices_[device].handle;
    unsigned v = 0;

    NvmlUtilization util = {};
    if (getUtilization_(h, &util) == 0) out.load = (float)util.gpu;
    if (getTemperature_(h, kTempGpu, &v) == 0) out.temp = (float)v;
    if (getPowerUsage_(h, &v) == 0) out.power = v / 1000.f;
    if (getClockInfo_(h, kClockGraphics, &v) == 0) out.coreClock = (float)v;
    if (getClockInfo_(h, kClockMem, &v) == 0) out.memClock = (float)v;
    if (getFanSpeed_(h, &v) == 0) out.fanPercent = (float)v;
    NvmlMemory mem = {};
    if (getMemoryInfo_(h, &mem) == 0 && mem.total) {
        out.vramTotalMB = (float)(mem.total / (1024.0 * 1024.0));
        out.vramUsedMB = (float)(mem.used / (1024.0 * 1024.0));
    }
}
