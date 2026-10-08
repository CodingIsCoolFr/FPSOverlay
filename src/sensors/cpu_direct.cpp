#include "sensors/cpu_direct.h"

#include <intrin.h>

#include <cmath>
#include <cstring>

namespace {

// PawnIO interface (namazso/PawnIO, pawnio_um.h): CTL_CODE(41394, 0x821 / 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS).
constexpr DWORD kIoctlLoadBinary = 0xA1B22084;
constexpr DWORD kIoctlExecuteFn = 0xA1B22104;
constexpr size_t kFnNameLength = 32;

// Signed PawnIO.Modules 0.1.6 blobs embedded by resource.rc (LGPL-2.1-or-later, libs/pawnio).
constexpr int kIdrIntelMsr = 102;
constexpr int kIdrAmdFamily17 = 103;

bool LoadBlob(int id, const void*& data, DWORD& size)
{
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL mem = res ? LoadResource(self, res) : nullptr;
    data = mem ? LockResource(mem) : nullptr;
    size = res ? SizeofResource(self, res) : 0;
    return data && size;
}

std::string CpuBrand()
{
    int regs[4];
    char brand[49] = {};
    for (int i = 0; i < 3; ++i) {
        __cpuid(regs, 0x80000002 + i);
        memcpy(brand + 16 * i, regs, 16);
    }
    return brand;
}

} // namespace

bool CpuDirect::Open(std::string& err)
{
    Close();
    int regs[4];
    __cpuid(regs, 0);
    const int maxLeaf = regs[0];
    char vendor[13] = {};
    memcpy(vendor, &regs[1], 4);
    memcpy(vendor + 4, &regs[3], 4);
    memcpy(vendor + 8, &regs[2], 4);
    __cpuid(regs, 1);
    const unsigned baseFamily = (regs[0] >> 8) & 0xF;
    const unsigned family = baseFamily == 0xF ? baseFamily + ((regs[0] >> 20) & 0xFF) : baseFamily;

    int blobId = 0;
    if (strcmp(vendor, "GenuineIntel") == 0) {
        if (maxLeaf >= 6) __cpuid(regs, 6);
        if (maxLeaf < 6 || !(regs[0] & (1 << 6))) {     // CPUID.06H:EAX[6], package thermal management
            err = "This Intel CPU has no package temperature sensor.";
            return false;
        }
        vendor_ = Vendor::Intel;
        blobId = kIdrIntelMsr;
    } else if (strcmp(vendor, "AuthenticAMD") == 0 && family >= 0x17 && family <= 0x1A) {
        vendor_ = Vendor::Amd;
        blobId = kIdrAmdFamily17;
    } else {
        err = "This CPU is read through LibreHardwareMonitor.";
        return false;
    }

    const void* blob = nullptr;
    DWORD blobSize = 0;
    if (!LoadBlob(blobId, blob, blobSize)) {
        err = "The PawnIO module is missing from this build.";
        vendor_ = Vendor::None;
        return false;
    }
    dev_ = CreateFileW(L"\\\\?\\GLOBALROOT\\Device\\PawnIO", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
    if (dev_ == INVALID_HANDLE_VALUE) {
        err = GetLastError() == ERROR_ACCESS_DENIED ? "PawnIO needs administrator rights." : "PawnIO is not installed.";
        vendor_ = Vendor::None;
        return false;
    }
    DWORD ret = 0;
    if (!DeviceIoControl(dev_, kIoctlLoadBinary, const_cast<void*>(blob), blobSize, nullptr, 0, &ret, nullptr)) {
        err = "PawnIO did not accept the CPU module (error " + std::to_string(GetLastError()) + ").";
        Close();
        return false;
    }

    uint64_t v = 0;
    if (vendor_ == Vendor::Intel) {
        if (ReadMsr(0x1A2, v)) {
            const int tj = (int)((v >> 16) & 0xFF);
            if (tj >= 70 && tj <= 130) tjMax_ = tj;
        }
        limitC_ = (float)tjMax_;
        if (ReadMsr(0x606, v)) {
            energyUnitJ_ = 1.0 / (double)(1ull << ((v >> 8) & 0x1F));
            energyMsr_ = 0x611;
        }
        desc_ = "Intel package sensor (TjMax " + std::to_string(tjMax_) + " C)";
    } else {
        pciMutex_ = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");
        // Linux k10temp's table of Tctl offsets; every later Ryzen reports Tctl = Tdie.
        static const struct { const char* model; float offset; } kOffsets[] = {
            { "AMD Ryzen 5 1600X", 20.f }, { "AMD Ryzen 7 1700X", 20.f }, { "AMD Ryzen 7 1800X", 20.f },
            { "AMD Ryzen 7 2700X", 10.f }, { "AMD Ryzen Threadripper 19", 27.f }, { "AMD Ryzen Threadripper 29", 27.f },
        };
        const std::string brand = CpuBrand();
        for (const auto& o : kOffsets)
            if (brand.find(o.model) != std::string::npos) amdOffset_ = o.offset;
        limitC_ = 95.f;
        if (ReadMsr(0xC0010299, v)) {
            energyUnitJ_ = 1.0 / (double)(1ull << ((v >> 8) & 0x1F));
            energyMsr_ = 0xC001029B;
        }
        desc_ = amdOffset_ > 0 ? "AMD Tdie (Tctl - " + std::to_string((int)amdOffset_) + " C)" : std::string("AMD Tctl");
    }

    float t = 0.f;
    bool atLimit = false;
    if (!ReadTemperature(t, atLimit)) {
        err = "The CPU temperature register did not answer.";
        Close();
        return false;
    }
    return true;
}

void CpuDirect::Close()
{
    if (dev_ != INVALID_HANDLE_VALUE) CloseHandle(dev_);
    dev_ = INVALID_HANDLE_VALUE;
    if (pciMutex_) CloseHandle(pciMutex_);
    pciMutex_ = nullptr;
    vendor_ = Vendor::None;
    energyUnitJ_ = 0.0;
    energyMsr_ = 0;
    haveEnergy_ = false;
}

bool CpuDirect::Execute(const char* fn, uint64_t in, uint64_t& out)
{
    if (dev_ == INVALID_HANDLE_VALUE) return false;
    unsigned char buf[kFnNameLength + sizeof(uint64_t)] = {};
    strncpy_s(reinterpret_cast<char*>(buf), kFnNameLength, fn, _TRUNCATE);
    memcpy(buf + kFnNameLength, &in, sizeof(in));
    DWORD ret = 0;
    out = 0;
    return DeviceIoControl(dev_, kIoctlExecuteFn, buf, sizeof(buf), &out, sizeof(out), &ret, nullptr) && ret >= sizeof(out);
}

bool CpuDirect::ReadSmn(uint32_t addr, uint32_t& value)
{
    // The SMN index/data pair is two PCI config accesses: hold the mutex every reader shares.
    if (pciMutex_) {
        const DWORD w = WaitForSingleObject(pciMutex_, 10);
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return false;
    }
    uint64_t out = 0;
    const bool ok = Execute("ioctl_read_smn", addr, out);
    if (pciMutex_) ReleaseMutex(pciMutex_);
    value = (uint32_t)out;
    return ok;
}

bool CpuDirect::ReadTemperature(float& celsius, bool& atLimit)
{
    if (vendor_ == Vendor::Intel) {
        uint64_t v = 0;
        return ReadMsr(0x1B1, v) && cputemp::DecodeIntel(v, tjMax_, celsius, atLimit);
    }
    if (vendor_ == Vendor::Amd) {
        uint32_t r = 0;
        if (!ReadSmn(0x00059800, r)) return false;
        const float t = cputemp::DecodeAmd(r, amdOffset_);
        if (t < -30.f || t > 150.f) return false;
        celsius = t;
        atLimit = t >= limitC_ - 0.5f;
        return true;
    }
    return false;
}

float CpuDirect::PackagePowerW()
{
    uint64_t raw = 0;
    if (!energyMsr_ || !ReadMsr(energyMsr_, raw)) return NAN;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const uint32_t energy = (uint32_t)raw;      // 32-bit counter; unsigned subtraction handles the wrap
    float watts = NAN;
    if (haveEnergy_) {
        const double dt = (double)(now.QuadPart - lastEnergyQpc_.QuadPart) / (double)freq.QuadPart;
        if (dt > 0.05) watts = (float)((double)(uint32_t)(energy - lastEnergy_) * energyUnitJ_ / dt);
    }
    lastEnergy_ = energy;
    lastEnergyQpc_ = now;
    haveEnergy_ = true;
    return watts;
}
