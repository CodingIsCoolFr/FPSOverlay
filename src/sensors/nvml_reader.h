// NVIDIA Management Library, loaded at runtime from the driver (nvml.dll). Each query takes
// microseconds, where the LibreHardwareMonitor NVIDIA path costs ~60 ms per update.
#pragma once

#include "sensors/snapshot.h"

#include <windows.h>

#include <string>
#include <vector>

class NvmlReader {
public:
    ~NvmlReader();
    bool Open();            // false when no NVIDIA driver is present
    void Close();
    bool IsOpen() const { return lib_ != nullptr; }

    // Device index for a PCI slot or name, -1 if none.
    int Find(bool hasPci, unsigned bus, unsigned device, const std::string& name) const;

    // Fills load, temp, power, core/mem clock, fan % and VRAM totals. Leaves other fields alone.
    void Read(int device, GpuReadings& out) const;

private:
    struct Device {
        void*        handle = nullptr;
        std::string  name;
        unsigned     bus = 0, device = 0;
    };

    HMODULE lib_ = nullptr;
    std::vector<Device> devices_;

    // Function pointers (nvmlReturn_t == int, 0 == success)
    int (*shutdown_)() = nullptr;
    int (*getTemperature_)(void*, int, unsigned*) = nullptr;
    int (*getPowerUsage_)(void*, unsigned*) = nullptr;
    int (*getClockInfo_)(void*, int, unsigned*) = nullptr;
    int (*getFanSpeed_)(void*, unsigned*) = nullptr;
    int (*getUtilization_)(void*, void*) = nullptr;
    int (*getMemoryInfo_)(void*, void*) = nullptr;
};
