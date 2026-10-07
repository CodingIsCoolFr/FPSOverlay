// Picks the right LibreHardwareMonitor sensors out of the hundreds it exposes.
// Pure C++ so the rules are unit-tested against real sensor dumps.
#pragma once

#include <string>
#include <vector>

struct LhmSensor {
    int hardware = -1;
    std::string hwName;
    std::string hwType;     // "Cpu", "GpuNvidia", "GpuAmd", "GpuIntel", "SuperIO", "Motherboard", ...
    std::string name;
    std::string type;       // "Temperature", "Power", "Fan", "Clock", "Load", "SmallData", "Data", ...
    std::string id;
};

struct LhmGpuPick {
    int hardware = -1;
    std::string hwName;
    std::string hwType;
    int temp = -1, hotspot = -1, power = -1, coreClock = -1, memClock = -1, fan = -1, load = -1,
        vramUsed = -1, vramTotal = -1;
};

struct LhmPicks {
    int cpuTemp = -1;
    int cpuPower = -1;
    int cpuFan = -1;
    std::vector<LhmGpuPick> gpus;
    std::vector<int> cpuTempChoices;    // ordered best first
    std::vector<int> fanChoices;        // every non-GPU fan
};

// Preferences are sensor identifiers saved in config; empty = automatic.
LhmPicks SelectLhmSensors(const std::vector<LhmSensor>& sensors,
                          const std::string& cpuTempPref,
                          const std::string& cpuFanPref);

// Index into picks.gpus for a DXGI adapter (name + PCI vendor id), -1 if none matches.
int MatchLhmGpu(const LhmPicks& picks, const std::string& adapterName, unsigned vendorId);
