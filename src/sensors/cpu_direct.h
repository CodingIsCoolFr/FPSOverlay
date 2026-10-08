// CPU package temperature and power read straight from the CPU through the PawnIO driver.
//
// LibreHardwareMonitor's CPU update moves its thread onto every core in turn (74 moves per
// update on an i9-13900K) to read per-core registers, which wakes idle cores and adds a little
// of the heat it measures. The package temperature is one package-wide register that any core
// can read, so this reads just that, on whatever core the thread is on:
//   Intel: IA32_PACKAGE_THERM_STATUS (0x1B1) against TjMax from MSR_TEMPERATURE_TARGET (0x1A2),
//          package energy from MSR_PKG_ENERGY_STATUS (0x611) in RAPL units (0x606).
//   AMD Zen 1-5: Tctl from SMN THM_TCON_CUR_TMP (0x59800); Tdie on the parts that add an offset;
//          package energy from MSR 0xC001029B in units from 0xC0010299.
// Needs admin rights and PawnIO; Open() fails cleanly otherwise and the caller falls back to LHM.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

// Register decoding, kept pure for the unit tests.
namespace cputemp {

// IA32_PACKAGE_THERM_STATUS / IA32_THERM_STATUS: degrees below TjMax in bits 22:16, valid when
// bit 31 is set; bit 0 says the sensor is at the thermal limit right now.
inline bool DecodeIntel(uint64_t msr, int tjMax, float& celsius, bool& atLimit)
{
    if (!(msr & (1ull << 31))) return false;
    celsius = (float)(tjMax - (int)((msr >> 16) & 0x7F));
    atLimit = (msr & 1) != 0;
    return true;
}

// AMD THM_TCON_CUR_TMP (SMN 0x59800): bits 31:21 in 1/8 °C, 49 °C lower when RANGE_SEL (bit 19)
// is set or TJ_SEL (bits 17:16) is 3, as in Linux k10temp. tdieOffset turns Tctl into Tdie.
inline float DecodeAmd(uint32_t reg, float tdieOffset)
{
    float t = (float)((reg >> 21) & 0x7FF) * 0.125f;
    if ((reg & 0x80000) || (reg & 0x30000) == 0x30000) t -= 49.f;
    return t - tdieOffset;
}

} // namespace cputemp

class CpuDirect {
public:
    ~CpuDirect() { Close(); }

    bool Open(std::string& err);
    void Close();
    bool IsOpen() const { return dev_ != INVALID_HANDLE_VALUE; }

    // One reading of the package temperature in °C. atLimit: the CPU reports that it is at its
    // thermal limit right now (Intel status bit), or the reading is at the limit (AMD).
    bool ReadTemperature(float& celsius, bool& atLimit);

    // Average package power since the previous call, from the energy counter. NaN on the first
    // call or when the CPU has no readable counter.
    float PackagePowerW();
    bool HasPower() const { return energyUnitJ_ > 0; }

    float LimitC() const { return limitC_; }        // throttle point, for colouring
    const std::string& Description() const { return desc_; }

private:
    bool Execute(const char* fn, uint64_t in, uint64_t& out);
    bool ReadMsr(uint32_t msr, uint64_t& value) { return Execute("ioctl_read_msr", msr, value); }
    bool ReadSmn(uint32_t addr, uint32_t& value);
    bool ReadEnergy(uint64_t& raw);

    enum class Vendor { None, Intel, Amd };
    HANDLE dev_ = INVALID_HANDLE_VALUE;
    HANDLE pciMutex_ = nullptr;     // AMD: "Global\Access_PCI", shared with LHM and other tools
    Vendor vendor_ = Vendor::None;
    int tjMax_ = 100;
    float amdOffset_ = 0.f;         // Tctl - Tdie on Ryzen 1000/2000 "X" and early Threadripper
    float limitC_ = 100.f;
    double energyUnitJ_ = 0.0;
    uint32_t energyMsr_ = 0;
    uint32_t lastEnergy_ = 0;
    LARGE_INTEGER lastEnergyQpc_ = {};
    bool haveEnergy_ = false;
    std::string desc_;
};
